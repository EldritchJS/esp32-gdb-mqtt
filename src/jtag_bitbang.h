#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    TAP_RESET,
    TAP_IDLE,
    TAP_SELECT_DR,
    TAP_CAPTURE_DR,
    TAP_SHIFT_DR,
    TAP_EXIT1_DR,
    TAP_PAUSE_DR,
    TAP_EXIT2_DR,
    TAP_UPDATE_DR,
    TAP_SELECT_IR,
    TAP_CAPTURE_IR,
    TAP_SHIFT_IR,
    TAP_EXIT1_IR,
    TAP_PAUSE_IR,
    TAP_EXIT2_IR,
    TAP_UPDATE_IR,
} tap_state_t;

void jtag_init(void);

void jtag_reset(void);
void jtag_goto_idle(void);

void jtag_shift_ir(const uint8_t *tdi, uint8_t *tdo, size_t bits);
void jtag_shift_dr(const uint8_t *tdi, uint8_t *tdo, size_t bits);

void jtag_clock(bool tms, bool tdi);
bool jtag_clock_tdo(bool tms, bool tdi);

tap_state_t jtag_get_state(void);

void jtag_set_clock_delay_us(uint32_t delay_us);
