#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv_dm.h"

#ifdef CONFIG_JTAG_TRANSPORT_ECP5
#include "ecp5_jtag.h"
#define dm_shift_ir ecp5_jtagg_shift_ir
#define dm_shift_dr ecp5_jtagg_shift_dr
#else
#include "jtag_bitbang.h"
#define dm_shift_ir jtag_shift_ir
#define dm_shift_dr jtag_shift_dr
#endif

static const char *TAG = "riscv-dm";

static uint8_t s_dtm_ir = 0xFF;

static void dtm_set_ir(uint8_t ir)
{
    if (s_dtm_ir == ir) return;
    s_dtm_ir = ir;
    dm_shift_ir(&ir, NULL, DTM_IR_LEN);
}

static void pack_dmi(uint8_t *buf, uint8_t addr, uint32_t data, uint8_t op)
{
    memset(buf, 0, 6);
    /* bits [1:0] = op */
    buf[0] = op & 0x03;
    /* bits [33:2] = data, shifted left by 2 */
    uint64_t val = ((uint64_t)data << 2) | op;
    val |= ((uint64_t)addr << 34);
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
    buf[4] = (val >> 32) & 0xFF;
    buf[5] = (val >> 40) & 0xFF;
}

static void unpack_dmi(const uint8_t *buf, uint8_t *addr, uint32_t *data, uint8_t *op)
{
    uint64_t val = 0;
    for (int i = 5; i >= 0; i--) {
        val = (val << 8) | buf[i];
    }
    *op = val & 0x03;
    *data = (val >> 2) & 0xFFFFFFFF;
    *addr = (val >> 34) & 0x7F;
}

static int dmi_transfer(uint8_t addr, uint32_t wdata, uint8_t op,
                        uint32_t *rdata)
{
    dtm_set_ir(DTM_IR_DMI);

    uint8_t tdi[6], tdo[6];
    pack_dmi(tdi, addr, wdata, op);
    dm_shift_dr(tdi, tdo, DMI_TOTAL_BITS);

    /* The response comes back on the *next* transaction */
    for (int retry = 0; retry < 16; retry++) {
        pack_dmi(tdi, 0, 0, DMI_OP_NOP);
        dm_shift_dr(tdi, tdo, DMI_TOTAL_BITS);

        uint8_t resp_op, resp_addr;
        uint32_t resp_data;
        unpack_dmi(tdo, &resp_addr, &resp_data, &resp_op);

        if (resp_op == DMI_STATUS_OK) {
            if (rdata) *rdata = resp_data;
            return 0;
        }
        if (resp_op == DMI_STATUS_BUSY) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        ESP_LOGE(TAG, "DMI error, op=%d addr=0x%02x", resp_op, addr);
        return -1;
    }

    ESP_LOGE(TAG, "DMI timeout, addr=0x%02x", addr);
    return -1;
}

int dm_read(uint8_t addr, uint32_t *value)
{
    return dmi_transfer(addr, 0, DMI_OP_READ, value);
}

int dm_write(uint8_t addr, uint32_t value)
{
    return dmi_transfer(addr, value, DMI_OP_WRITE, NULL);
}

int dm_init(void)
{
    s_dtm_ir = 0xFF;

    if (dm_write(DM_DMCONTROL, DMCONTROL_DMACTIVE) != 0) {
        ESP_LOGE(TAG, "Failed to activate debug module");
        return -1;
    }

    uint32_t dmstatus;
    if (dm_read(DM_DMSTATUS, &dmstatus) != 0) {
        ESP_LOGE(TAG, "Failed to read dmstatus");
        return -1;
    }

    ESP_LOGI(TAG, "Debug Module active, dmstatus=0x%08lx", (unsigned long)dmstatus);
    return 0;
}

int dm_halt(void)
{
    if (dm_write(DM_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_HALTREQ) != 0) {
        return -1;
    }

    for (int i = 0; i < 100; i++) {
        uint32_t status;
        if (dm_read(DM_DMSTATUS, &status) != 0) return -1;
        if (status & DMSTATUS_ALLHALTED) {
            dm_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGE(TAG, "Halt timeout");
    return -1;
}

int dm_resume(void)
{
    if (dm_write(DM_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_RESUMEREQ) != 0) {
        return -1;
    }

    for (int i = 0; i < 100; i++) {
        uint32_t status;
        if (dm_read(DM_DMSTATUS, &status) != 0) return -1;
        if (status & DMSTATUS_ALLRUNNING) {
            dm_write(DM_DMCONTROL, DMCONTROL_DMACTIVE);
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGE(TAG, "Resume timeout");
    return -1;
}

bool dm_is_halted(void)
{
    uint32_t status;
    if (dm_read(DM_DMSTATUS, &status) != 0) return false;
    return (status & DMSTATUS_ALLHALTED) != 0;
}

static int wait_abstract_cmd(void)
{
    for (int i = 0; i < 100; i++) {
        uint32_t acs;
        if (dm_read(DM_ABSTRACTCS, &acs) != 0) return -1;
        if (!(acs & ABSTRACTCS_BUSY)) {
            if (acs & ABSTRACTCS_CMDERR_MASK) {
                ESP_LOGE(TAG, "Abstract cmd error: 0x%lx",
                         (unsigned long)((acs & ABSTRACTCS_CMDERR_MASK) >> 8));
                /* Clear cmderr by writing 1s */
                dm_write(DM_ABSTRACTCS, ABSTRACTCS_CMDERR_MASK);
                return -1;
            }
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGE(TAG, "Abstract cmd timeout");
    return -1;
}

int dm_read_gpr(int reg_num, uint32_t *value)
{
    uint32_t cmd = (AC_ACCESS_REGISTER << 24) |
                   AC_AARSIZE_32 | AC_TRANSFER |
                   AC_REGNO(AC_REG_GPR_BASE + reg_num);
    if (dm_write(DM_COMMAND, cmd) != 0) return -1;
    if (wait_abstract_cmd() != 0) return -1;
    return dm_read(DM_DATA0, value);
}

int dm_write_gpr(int reg_num, uint32_t value)
{
    if (dm_write(DM_DATA0, value) != 0) return -1;
    uint32_t cmd = (AC_ACCESS_REGISTER << 24) |
                   AC_AARSIZE_32 | AC_TRANSFER | AC_WRITE |
                   AC_REGNO(AC_REG_GPR_BASE + reg_num);
    if (dm_write(DM_COMMAND, cmd) != 0) return -1;
    return wait_abstract_cmd();
}

int dm_read_csr(int csr, uint32_t *value)
{
    uint32_t cmd = (AC_ACCESS_REGISTER << 24) |
                   AC_AARSIZE_32 | AC_TRANSFER |
                   AC_REGNO(csr);
    if (dm_write(DM_COMMAND, cmd) != 0) return -1;
    if (wait_abstract_cmd() != 0) return -1;
    return dm_read(DM_DATA0, value);
}

int dm_write_csr(int csr, uint32_t value)
{
    if (dm_write(DM_DATA0, value) != 0) return -1;
    uint32_t cmd = (AC_ACCESS_REGISTER << 24) |
                   AC_AARSIZE_32 | AC_TRANSFER | AC_WRITE |
                   AC_REGNO(csr);
    if (dm_write(DM_COMMAND, cmd) != 0) return -1;
    return wait_abstract_cmd();
}

int dm_read_memory(uint32_t addr, uint8_t *buf, size_t len)
{
    /*
     * Use program buffer to read memory word by word.
     * progbuf0: lw s0, 0(s0)
     * progbuf1: ebreak
     *
     * 1. Write addr to data0
     * 2. Execute: transfer s0 in + postexec
     * 3. Read data0 (contains mem[addr])
     * 4. Repeat with addr+4
     */
    uint32_t lw_s0_s0 = 0x00042403;  /* lw s0, 0(s0) */
    uint32_t ebreak   = 0x00100073;

    if (dm_write(DM_PROGBUF0, lw_s0_s0) != 0) return -1;
    if (dm_write(DM_PROGBUF1, ebreak) != 0) return -1;

    size_t offset = 0;
    while (offset < len) {
        size_t remaining = len - offset;
        uint32_t cur_addr = addr + offset;

        if (remaining >= 4 && (cur_addr & 3) == 0) {
            if (dm_write(DM_DATA0, cur_addr) != 0) return -1;
            uint32_t cmd = (AC_ACCESS_REGISTER << 24) |
                           AC_AARSIZE_32 | AC_TRANSFER | AC_WRITE |
                           AC_POSTEXEC |
                           AC_REGNO(AC_REG_GPR_BASE + 8); /* s0 */
            if (dm_write(DM_COMMAND, cmd) != 0) return -1;
            if (wait_abstract_cmd() != 0) return -1;

            /* s0 now contains mem[addr], read it back */
            uint32_t cmd2 = (AC_ACCESS_REGISTER << 24) |
                            AC_AARSIZE_32 | AC_TRANSFER |
                            AC_REGNO(AC_REG_GPR_BASE + 8);
            if (dm_write(DM_COMMAND, cmd2) != 0) return -1;
            if (wait_abstract_cmd() != 0) return -1;

            uint32_t word;
            if (dm_read(DM_DATA0, &word) != 0) return -1;
            memcpy(&buf[offset], &word, 4);
            offset += 4;
        } else {
            /* Byte-at-a-time for unaligned access: lb s0, 0(s0) */
            uint32_t lb_s0_s0 = 0x00040403; /* lb s0, 0(s0) */
            if (dm_write(DM_PROGBUF0, lb_s0_s0) != 0) return -1;

            if (dm_write(DM_DATA0, cur_addr) != 0) return -1;
            uint32_t cmd = (AC_ACCESS_REGISTER << 24) |
                           AC_AARSIZE_32 | AC_TRANSFER | AC_WRITE |
                           AC_POSTEXEC |
                           AC_REGNO(AC_REG_GPR_BASE + 8);
            if (dm_write(DM_COMMAND, cmd) != 0) return -1;
            if (wait_abstract_cmd() != 0) return -1;

            uint32_t cmd2 = (AC_ACCESS_REGISTER << 24) |
                            AC_AARSIZE_32 | AC_TRANSFER |
                            AC_REGNO(AC_REG_GPR_BASE + 8);
            if (dm_write(DM_COMMAND, cmd2) != 0) return -1;
            if (wait_abstract_cmd() != 0) return -1;

            uint32_t val;
            if (dm_read(DM_DATA0, &val) != 0) return -1;
            buf[offset] = (uint8_t)(val & 0xFF);
            offset++;

            /* Restore word-sized progbuf for next iteration */
            if (offset < len && remaining > 1) {
                if (dm_write(DM_PROGBUF0, lw_s0_s0) != 0) return -1;
            }
        }
    }

    return 0;
}

int dm_write_memory(uint32_t addr, const uint8_t *buf, size_t len)
{
    /*
     * Use program buffer to write memory word by word.
     * progbuf0: sw s1, 0(s0)
     * progbuf1: ebreak
     *
     * 1. Write addr to s0 via abstract cmd
     * 2. Write data to s1 via abstract cmd
     * 3. Execute progbuf (postexec on last transfer)
     */
    uint32_t sw_s1_s0 = 0x00942023; /* sw s1, 0(s0) */
    uint32_t ebreak   = 0x00100073;

    if (dm_write(DM_PROGBUF0, sw_s1_s0) != 0) return -1;
    if (dm_write(DM_PROGBUF1, ebreak) != 0) return -1;

    size_t offset = 0;
    while (offset < len) {
        size_t remaining = len - offset;
        uint32_t cur_addr = addr + offset;

        if (remaining >= 4 && (cur_addr & 3) == 0) {
            /* Load address into s0 */
            if (dm_write(DM_DATA0, cur_addr) != 0) return -1;
            uint32_t cmd_s0 = (AC_ACCESS_REGISTER << 24) |
                              AC_AARSIZE_32 | AC_TRANSFER | AC_WRITE |
                              AC_REGNO(AC_REG_GPR_BASE + 8);
            if (dm_write(DM_COMMAND, cmd_s0) != 0) return -1;
            if (wait_abstract_cmd() != 0) return -1;

            /* Load data into s1 + execute progbuf */
            uint32_t word;
            memcpy(&word, &buf[offset], 4);
            if (dm_write(DM_DATA0, word) != 0) return -1;
            uint32_t cmd_s1 = (AC_ACCESS_REGISTER << 24) |
                              AC_AARSIZE_32 | AC_TRANSFER | AC_WRITE |
                              AC_POSTEXEC |
                              AC_REGNO(AC_REG_GPR_BASE + 9); /* s1 */
            if (dm_write(DM_COMMAND, cmd_s1) != 0) return -1;
            if (wait_abstract_cmd() != 0) return -1;
            offset += 4;
        } else {
            /* Byte write: sb s1, 0(s0) */
            uint32_t sb_s1_s0 = 0x00940023;
            if (dm_write(DM_PROGBUF0, sb_s1_s0) != 0) return -1;

            if (dm_write(DM_DATA0, cur_addr) != 0) return -1;
            uint32_t cmd_s0 = (AC_ACCESS_REGISTER << 24) |
                              AC_AARSIZE_32 | AC_TRANSFER | AC_WRITE |
                              AC_REGNO(AC_REG_GPR_BASE + 8);
            if (dm_write(DM_COMMAND, cmd_s0) != 0) return -1;
            if (wait_abstract_cmd() != 0) return -1;

            if (dm_write(DM_DATA0, (uint32_t)buf[offset]) != 0) return -1;
            uint32_t cmd_s1 = (AC_ACCESS_REGISTER << 24) |
                              AC_AARSIZE_32 | AC_TRANSFER | AC_WRITE |
                              AC_POSTEXEC |
                              AC_REGNO(AC_REG_GPR_BASE + 9);
            if (dm_write(DM_COMMAND, cmd_s1) != 0) return -1;
            if (wait_abstract_cmd() != 0) return -1;
            offset++;

            if (offset < len && remaining > 1) {
                if (dm_write(DM_PROGBUF0, sw_s1_s0) != 0) return -1;
            }
        }
    }

    return 0;
}
