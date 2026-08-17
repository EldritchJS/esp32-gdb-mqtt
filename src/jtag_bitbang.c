#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "jtag_bitbang.h"

#define PIN_TCK CONFIG_JTAG_PIN_TCK
#define PIN_TMS CONFIG_JTAG_PIN_TMS
#define PIN_TDI CONFIG_JTAG_PIN_TDI
#define PIN_TDO CONFIG_JTAG_PIN_TDO

static tap_state_t s_state;
static uint32_t s_clock_delay_us = 1;

void jtag_init(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_TCK) | (1ULL << PIN_TMS) | (1ULL << PIN_TDI),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << PIN_TDO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    gpio_set_level(PIN_TCK, 0);
    gpio_set_level(PIN_TMS, 1);
    gpio_set_level(PIN_TDI, 0);

    s_state = TAP_RESET;
}

void jtag_set_clock_delay_us(uint32_t delay_us)
{
    s_clock_delay_us = delay_us;
}

static void clock_pulse(void)
{
    gpio_set_level(PIN_TCK, 1);
    if (s_clock_delay_us) esp_rom_delay_us(s_clock_delay_us);
    gpio_set_level(PIN_TCK, 0);
    if (s_clock_delay_us) esp_rom_delay_us(s_clock_delay_us);
}

void jtag_clock(bool tms, bool tdi)
{
    gpio_set_level(PIN_TMS, tms);
    gpio_set_level(PIN_TDI, tdi);
    clock_pulse();
}

bool jtag_clock_tdo(bool tms, bool tdi)
{
    gpio_set_level(PIN_TMS, tms);
    gpio_set_level(PIN_TDI, tdi);
    gpio_set_level(PIN_TCK, 1);
    if (s_clock_delay_us) esp_rom_delay_us(s_clock_delay_us);
    bool tdo = gpio_get_level(PIN_TDO);
    gpio_set_level(PIN_TCK, 0);
    if (s_clock_delay_us) esp_rom_delay_us(s_clock_delay_us);
    return tdo;
}

static const tap_state_t next_state[16][2] = {
    /* TAP_RESET      */ { TAP_IDLE,      TAP_RESET      },
    /* TAP_IDLE       */ { TAP_IDLE,      TAP_SELECT_DR  },
    /* TAP_SELECT_DR  */ { TAP_CAPTURE_DR, TAP_SELECT_IR },
    /* TAP_CAPTURE_DR */ { TAP_SHIFT_DR,  TAP_EXIT1_DR   },
    /* TAP_SHIFT_DR   */ { TAP_SHIFT_DR,  TAP_EXIT1_DR   },
    /* TAP_EXIT1_DR   */ { TAP_PAUSE_DR,  TAP_UPDATE_DR  },
    /* TAP_PAUSE_DR   */ { TAP_PAUSE_DR,  TAP_EXIT2_DR   },
    /* TAP_EXIT2_DR   */ { TAP_SHIFT_DR,  TAP_UPDATE_DR  },
    /* TAP_UPDATE_DR  */ { TAP_IDLE,      TAP_SELECT_DR  },
    /* TAP_SELECT_IR  */ { TAP_CAPTURE_IR, TAP_RESET     },
    /* TAP_CAPTURE_IR */ { TAP_SHIFT_IR,  TAP_EXIT1_IR   },
    /* TAP_SHIFT_IR   */ { TAP_SHIFT_IR,  TAP_EXIT1_IR   },
    /* TAP_EXIT1_IR   */ { TAP_PAUSE_IR,  TAP_UPDATE_IR  },
    /* TAP_PAUSE_IR   */ { TAP_PAUSE_IR,  TAP_EXIT2_IR   },
    /* TAP_EXIT2_IR   */ { TAP_SHIFT_IR,  TAP_UPDATE_IR  },
    /* TAP_UPDATE_IR  */ { TAP_IDLE,      TAP_SELECT_DR  },
};

static void navigate(bool tms)
{
    s_state = next_state[s_state][tms ? 1 : 0];
}

void jtag_reset(void)
{
    for (int i = 0; i < 5; i++) {
        jtag_clock(true, false);
    }
    s_state = TAP_RESET;
}

void jtag_goto_idle(void)
{
    if (s_state == TAP_RESET) {
        jtag_clock(false, false);
        s_state = TAP_IDLE;
    }
}

static void goto_shift_ir(void)
{
    switch (s_state) {
    case TAP_IDLE:
        jtag_clock(true, false); navigate(true);   /* -> Select-DR */
        jtag_clock(true, false); navigate(true);   /* -> Select-IR */
        jtag_clock(false, false); navigate(false);  /* -> Capture-IR */
        jtag_clock(false, false); navigate(false);  /* -> Shift-IR */
        break;
    case TAP_UPDATE_DR:
    case TAP_UPDATE_IR:
        jtag_clock(true, false); navigate(true);   /* -> Select-DR */
        jtag_clock(true, false); navigate(true);   /* -> Select-IR */
        jtag_clock(false, false); navigate(false);  /* -> Capture-IR */
        jtag_clock(false, false); navigate(false);  /* -> Shift-IR */
        break;
    default:
        jtag_reset();
        jtag_goto_idle();
        goto_shift_ir();
        break;
    }
}

static void goto_shift_dr(void)
{
    switch (s_state) {
    case TAP_IDLE:
        jtag_clock(true, false); navigate(true);   /* -> Select-DR */
        jtag_clock(false, false); navigate(false);  /* -> Capture-DR */
        jtag_clock(false, false); navigate(false);  /* -> Shift-DR */
        break;
    case TAP_UPDATE_DR:
    case TAP_UPDATE_IR:
        jtag_clock(true, false); navigate(true);   /* -> Select-DR */
        jtag_clock(false, false); navigate(false);  /* -> Capture-DR */
        jtag_clock(false, false); navigate(false);  /* -> Shift-DR */
        break;
    default:
        jtag_reset();
        jtag_goto_idle();
        goto_shift_dr();
        break;
    }
}

static void shift_bits(const uint8_t *tdi, uint8_t *tdo, size_t bits)
{
    for (size_t i = 0; i < bits; i++) {
        bool tdi_bit = false;
        if (tdi) {
            tdi_bit = (tdi[i / 8] >> (i % 8)) & 1;
        }
        bool last = (i == bits - 1);
        bool tdo_bit = jtag_clock_tdo(last, tdi_bit);
        if (tdo) {
            if (i % 8 == 0) tdo[i / 8] = 0;
            if (tdo_bit) tdo[i / 8] |= (1 << (i % 8));
        }
    }
    /* Last bit was clocked with TMS=1, moving to Exit1 */
    navigate(true);
    /* Go to Update */
    jtag_clock(true, false);
    navigate(true);
}

void jtag_shift_ir(const uint8_t *tdi, uint8_t *tdo, size_t bits)
{
    goto_shift_ir();
    shift_bits(tdi, tdo, bits);
}

void jtag_shift_dr(const uint8_t *tdi, uint8_t *tdo, size_t bits)
{
    goto_shift_dr();
    shift_bits(tdi, tdo, bits);
}

tap_state_t jtag_get_state(void)
{
    return s_state;
}
