#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jtag_bitbang.h"
#include "ecp5_jtag.h"

static const char *TAG = "ecp5-jtag";

#define ECP5_IR_LEN 8

static void send_ir(uint8_t ir)
{
    jtag_shift_ir(&ir, NULL, ECP5_IR_LEN);
}

int ecp5_init(void)
{
    jtag_init();
    jtag_reset();
    jtag_goto_idle();

    uint32_t id = ecp5_read_idcode();
    if (id == ECP5_IDCODE_LFE5U_25F ||
        id == ECP5_IDCODE_LFE5U_45F ||
        id == ECP5_IDCODE_LFE5U_85F) {
        ESP_LOGI(TAG, "ECP5 detected, IDCODE: 0x%08lx", (unsigned long)id);
        return 0;
    }

    ESP_LOGE(TAG, "Unknown IDCODE: 0x%08lx", (unsigned long)id);
    return -1;
}

uint32_t ecp5_read_idcode(void)
{
    send_ir(ECP5_IR_IDCODE);

    uint32_t id = 0;
    jtag_shift_dr((const uint8_t *)"\x00\x00\x00\x00", (uint8_t *)&id, 32);
    return id;
}

uint32_t ecp5_read_status(void)
{
    send_ir(ECP5_IR_LSC_READ_STATUS);

    uint32_t status = 0;
    jtag_shift_dr((const uint8_t *)"\x00\x00\x00\x00", (uint8_t *)&status, 32);
    return status;
}

bool ecp5_is_done(void)
{
    return (ecp5_read_status() & ECP5_STATUS_DONE) != 0;
}

static bool wait_not_busy(int timeout_ms)
{
    for (int i = 0; i < timeout_ms; i++) {
        send_ir(ECP5_IR_LSC_CHECK_BUSY);
        uint8_t busy = 0;
        jtag_shift_dr((const uint8_t *)"\x00", &busy, 1);
        if (!busy) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

int ecp5_program_begin(void)
{
    jtag_reset();
    jtag_goto_idle();

    send_ir(ECP5_IR_ISC_ENABLE);
    uint8_t isc_data = 0x00;
    jtag_shift_dr(&isc_data, NULL, 8);

    if (!wait_not_busy(1000)) {
        ESP_LOGE(TAG, "Timeout entering ISC mode");
        return -1;
    }

    send_ir(ECP5_IR_ISC_ERASE);
    uint8_t erase_data = 0x01;
    jtag_shift_dr(&erase_data, NULL, 8);

    if (!wait_not_busy(5000)) {
        ESP_LOGE(TAG, "Timeout erasing SRAM");
        return -1;
    }

    send_ir(ECP5_IR_LSC_INIT_ADDRESS);
    uint8_t addr_data = 0x01;
    jtag_shift_dr(&addr_data, NULL, 8);

    send_ir(ECP5_IR_LSC_BITSTREAM_BURST);
    return 0;
}

int ecp5_program_data(const uint8_t *data, size_t len)
{
    size_t total_bits = len * 8;
    size_t chunk_bits = 4096;
    size_t offset = 0;

    while (offset < total_bits) {
        size_t remaining = total_bits - offset;
        size_t bits = (remaining < chunk_bits) ? remaining : chunk_bits;
        jtag_shift_dr(&data[offset / 8], NULL, bits);
        offset += bits;
    }
    return 0;
}

int ecp5_program_end(void)
{
    if (!wait_not_busy(1000)) {
        ESP_LOGE(TAG, "Timeout after bitstream load");
        return -1;
    }

    send_ir(ECP5_IR_ISC_DISABLE);
    jtag_shift_dr((const uint8_t *)"\x00", NULL, 8);

    if (!wait_not_busy(1000)) {
        ESP_LOGE(TAG, "Timeout exiting ISC mode");
        return -1;
    }

    uint32_t status = ecp5_read_status();
    if (status & ECP5_STATUS_FAIL) {
        ESP_LOGE(TAG, "Programming failed, status: 0x%08lx", (unsigned long)status);
        return -1;
    }

    if (!(status & ECP5_STATUS_DONE)) {
        ESP_LOGW(TAG, "DONE not asserted, status: 0x%08lx", (unsigned long)status);
        return -1;
    }

    ESP_LOGI(TAG, "Bitstream programmed successfully");
    return 0;
}

int ecp5_program_bitstream(const uint8_t *bitstream, size_t len)
{
    ESP_LOGI(TAG, "Programming bitstream (%u bytes)", (unsigned)len);

    int rc = ecp5_program_begin();
    if (rc != 0)
        return rc;

    rc = ecp5_program_data(bitstream, len);
    if (rc != 0)
        return rc;

    return ecp5_program_end();
}

/*
 * JTAGG tunneling: the ECP5's ER1/ER2 instructions route data through
 * the JTAGG primitive to the soft core's JTAG TAP. ER1 accesses the
 * first user register (JTDI→core, core→JTDO1), ER2 the second.
 *
 * The soft core's debug TAP is instantiated in HDL and connected to
 * the JTAGG primitive's signals. When we shift ER1, the data passes
 * through to the soft core's DR/IR chain.
 *
 * VexRiscv with JtagTap plugin expects ER1 to carry {tms, tdi} on
 * each clock, and returns tdo. We bit-bang the soft core's TAP
 * state machine through this tunnel.
 */

void ecp5_jtagg_shift_ir(const uint8_t *tdi, uint8_t *tdo, size_t bits)
{
    send_ir(ECP5_IR_ER1);

    /*
     * Through the JTAGG tunnel, we need to drive the soft core's TAP.
     * Each DR shift through ER1 carries one {tms, tdi} pair to the
     * soft core and returns one tdo bit.
     *
     * Navigate soft core TAP: Idle -> Select-DR -> Select-IR ->
     * Capture-IR -> Shift-IR
     */
    uint8_t tunnel_in, tunnel_out;

    /* Idle -> Select-DR: tms=1 */
    tunnel_in = 0x01; /* bit0=tms=1, bit1=tdi=0 */
    jtag_shift_dr(&tunnel_in, NULL, 2);
    send_ir(ECP5_IR_ER1);

    /* Select-DR -> Select-IR: tms=1 */
    tunnel_in = 0x01;
    jtag_shift_dr(&tunnel_in, NULL, 2);
    send_ir(ECP5_IR_ER1);

    /* Select-IR -> Capture-IR: tms=0 */
    tunnel_in = 0x00;
    jtag_shift_dr(&tunnel_in, NULL, 2);
    send_ir(ECP5_IR_ER1);

    /* Capture-IR -> Shift-IR: tms=0 */
    tunnel_in = 0x00;
    jtag_shift_dr(&tunnel_in, NULL, 2);

    /* Shift the IR data */
    for (size_t i = 0; i < bits; i++) {
        send_ir(ECP5_IR_ER1);
        bool tdi_bit = tdi ? ((tdi[i / 8] >> (i % 8)) & 1) : 0;
        bool last = (i == bits - 1);
        tunnel_in = (last ? 0x01 : 0x00) | (tdi_bit ? 0x02 : 0x00);
        jtag_shift_dr(&tunnel_in, &tunnel_out, 2);
        if (tdo) {
            if (i % 8 == 0) tdo[i / 8] = 0;
            if (tunnel_out & 0x01) tdo[i / 8] |= (1 << (i % 8));
        }
    }

    /* Exit1-IR -> Update-IR: tms=1 */
    send_ir(ECP5_IR_ER1);
    tunnel_in = 0x01;
    jtag_shift_dr(&tunnel_in, NULL, 2);

    /* Update-IR -> Idle: tms=0 */
    send_ir(ECP5_IR_ER1);
    tunnel_in = 0x00;
    jtag_shift_dr(&tunnel_in, NULL, 2);
}

void ecp5_jtagg_shift_dr(const uint8_t *tdi, uint8_t *tdo, size_t bits)
{
    send_ir(ECP5_IR_ER1);

    uint8_t tunnel_in, tunnel_out;

    /* Idle -> Select-DR: tms=1 */
    tunnel_in = 0x01;
    jtag_shift_dr(&tunnel_in, NULL, 2);
    send_ir(ECP5_IR_ER1);

    /* Select-DR -> Capture-DR: tms=0 */
    tunnel_in = 0x00;
    jtag_shift_dr(&tunnel_in, NULL, 2);
    send_ir(ECP5_IR_ER1);

    /* Capture-DR -> Shift-DR: tms=0 */
    tunnel_in = 0x00;
    jtag_shift_dr(&tunnel_in, NULL, 2);

    /* Shift the DR data */
    for (size_t i = 0; i < bits; i++) {
        send_ir(ECP5_IR_ER1);
        bool tdi_bit = tdi ? ((tdi[i / 8] >> (i % 8)) & 1) : 0;
        bool last = (i == bits - 1);
        tunnel_in = (last ? 0x01 : 0x00) | (tdi_bit ? 0x02 : 0x00);
        jtag_shift_dr(&tunnel_in, &tunnel_out, 2);
        if (tdo) {
            if (i % 8 == 0) tdo[i / 8] = 0;
            if (tunnel_out & 0x01) tdo[i / 8] |= (1 << (i % 8));
        }
    }

    /* Exit1-DR -> Update-DR: tms=1 */
    send_ir(ECP5_IR_ER1);
    tunnel_in = 0x01;
    jtag_shift_dr(&tunnel_in, NULL, 2);

    /* Update-DR -> Idle: tms=0 */
    send_ir(ECP5_IR_ER1);
    tunnel_in = 0x00;
    jtag_shift_dr(&tunnel_in, NULL, 2);
}
