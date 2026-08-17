#include <string.h>
#include "esp_log.h"
#include "riscv_dm.h"
#include "target_jtag.h"

#ifdef CONFIG_JTAG_TRANSPORT_ECP5
#include "ecp5_jtag.h"
#else
#include "jtag_bitbang.h"
#endif

static const char *TAG = "target-jtag";

static bool s_halted;
static bool s_initialized;

static struct {
    uint32_t addr;
    bool active;
} s_breakpoints[TARGET_MAX_BREAKPOINTS];

/* Saved s0/s1 before debug operations clobber them */
static uint32_t s_saved_s0, s_saved_s1;
static bool s_regs_saved;

static int save_scratch_regs(void)
{
    if (s_regs_saved) return 0;
    if (dm_read_gpr(8, &s_saved_s0) != 0) return -1;
    if (dm_read_gpr(9, &s_saved_s1) != 0) return -1;
    s_regs_saved = true;
    return 0;
}

static int restore_scratch_regs(void)
{
    if (!s_regs_saved) return 0;
    if (dm_write_gpr(8, s_saved_s0) != 0) return -1;
    if (dm_write_gpr(9, s_saved_s1) != 0) return -1;
    s_regs_saved = false;
    return 0;
}

static int jtag_backend_init(void)
{
    s_halted = false;
    s_initialized = false;
    s_regs_saved = false;
    memset(s_breakpoints, 0, sizeof(s_breakpoints));

#ifdef CONFIG_JTAG_TRANSPORT_ECP5
    if (ecp5_init() != 0) {
        ESP_LOGE(TAG, "ECP5 not detected on JTAG");
        return -1;
    }

    if (!ecp5_is_done()) {
        ESP_LOGW(TAG, "ECP5 has no bitstream loaded");
        return -1;
    }
#else
    jtag_reset();
#endif

    if (dm_init() != 0) {
        ESP_LOGE(TAG, "Debug Module init failed");
        return -1;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "JTAG backend initialized — connected to soft core");
    return 0;
}

static int jtag_halt(void)
{
    if (!s_initialized) return -1;
    if (s_halted) return 0;

    if (dm_halt() != 0) return -1;
    s_halted = true;

    if (save_scratch_regs() != 0) {
        ESP_LOGW(TAG, "Could not save scratch regs");
    }

    /* Apply hardware breakpoints to trigger registers */
    for (int i = 0; i < TARGET_MAX_BREAKPOINTS; i++) {
        if (!s_breakpoints[i].active) continue;
        dm_write_csr(CSR_TSELECT, i);
        dm_write_csr(CSR_TDATA1, DM_MCONTROL_TYPE | DM_MCONTROL_EXECUTE |
                                  DM_MCONTROL_M_MODE | DM_MCONTROL_ACTION_HALT);
        dm_write_csr(CSR_TDATA2, s_breakpoints[i].addr);
    }

    ESP_LOGI(TAG, "Hart halted");
    return 0;
}

static int jtag_resume(void)
{
    if (!s_initialized) return -1;
    if (!s_halted) return 0;

    restore_scratch_regs();

    if (dm_resume() != 0) return -1;
    s_halted = false;
    s_regs_saved = false;
    ESP_LOGI(TAG, "Hart resumed");
    return 0;
}

static bool jtag_is_halted(void)
{
    if (!s_initialized) return false;
    return dm_is_halted();
}

static int jtag_get_threads(target_thread_info_t *list, int max)
{
    if (max < 1) return 0;
    list[0].id = 1;
    strncpy(list[0].name, "hart0", TARGET_THREAD_NAME_LEN);
    list[0].priority = 0;
    list[0].state = s_halted ? TARGET_STATE_HALTED : TARGET_STATE_RUNNING;
    return 1;
}

static int jtag_read_registers(int thread_id, uint32_t *regs)
{
    if (!s_initialized || !s_halted) {
        memset(regs, 0, TARGET_NUM_REGS * sizeof(uint32_t));
        return -1;
    }
    (void)thread_id;

    regs[0] = 0; /* x0 is always zero */
    for (int i = 1; i < 32; i++) {
        if (i == 8) {
            regs[i] = s_saved_s0;
        } else if (i == 9) {
            regs[i] = s_saved_s1;
        } else if (dm_read_gpr(i, &regs[i]) != 0) {
            regs[i] = 0;
        }
    }

    /* PC = dpc CSR */
    if (dm_read_csr(AC_REG_DPC, &regs[TARGET_REG_PC]) != 0) {
        regs[TARGET_REG_PC] = 0;
    }

    return 0;
}

static int jtag_write_register(int thread_id, int reg_num, uint32_t value)
{
    if (!s_initialized || !s_halted) return -1;
    (void)thread_id;

    if (reg_num == 0) return 0;

    if (reg_num == TARGET_REG_PC) {
        return dm_write_csr(AC_REG_DPC, value);
    }

    if (reg_num == 8) { s_saved_s0 = value; return 0; }
    if (reg_num == 9) { s_saved_s1 = value; return 0; }

    if (reg_num >= 1 && reg_num < 32) {
        return dm_write_gpr(reg_num, value);
    }

    return -1;
}

static int jtag_read_memory(uint32_t addr, uint8_t *buf, size_t len)
{
    if (!s_initialized || !s_halted) {
        memset(buf, 0, len);
        return -1;
    }

    return dm_read_memory(addr, buf, len);
}

static int jtag_write_memory(uint32_t addr, const uint8_t *buf, size_t len)
{
    if (!s_initialized || !s_halted) return -1;

    return dm_write_memory(addr, buf, len);
}

static int jtag_set_breakpoint(uint32_t addr)
{
    for (int i = 0; i < TARGET_MAX_BREAKPOINTS; i++) {
        if (s_breakpoints[i].active && s_breakpoints[i].addr == addr) {
            return 0;
        }
    }
    for (int i = 0; i < TARGET_MAX_BREAKPOINTS; i++) {
        if (!s_breakpoints[i].active) {
            s_breakpoints[i].addr = addr;
            s_breakpoints[i].active = true;

            if (s_initialized && s_halted) {
                dm_write_csr(CSR_TSELECT, i);
                dm_write_csr(CSR_TDATA1, DM_MCONTROL_TYPE | DM_MCONTROL_EXECUTE |
                                          DM_MCONTROL_M_MODE | DM_MCONTROL_ACTION_HALT);
                dm_write_csr(CSR_TDATA2, addr);
            }

            ESP_LOGI(TAG, "Breakpoint %d set at 0x%08lx", i, (unsigned long)addr);
            return 0;
        }
    }
    return -1;
}

static int jtag_clear_breakpoint(uint32_t addr)
{
    for (int i = 0; i < TARGET_MAX_BREAKPOINTS; i++) {
        if (s_breakpoints[i].active && s_breakpoints[i].addr == addr) {
            s_breakpoints[i].active = false;

            if (s_initialized && s_halted) {
                dm_write_csr(CSR_TSELECT, i);
                dm_write_csr(CSR_TDATA1, 0);
                dm_write_csr(CSR_TDATA2, 0);
            }

            ESP_LOGI(TAG, "Breakpoint %d cleared at 0x%08lx", i, (unsigned long)addr);
            return 0;
        }
    }
    return -1;
}

static void jtag_clear_all_breakpoints(void)
{
    for (int i = 0; i < TARGET_MAX_BREAKPOINTS; i++) {
        if (s_breakpoints[i].active && s_initialized && s_halted) {
            dm_write_csr(CSR_TSELECT, i);
            dm_write_csr(CSR_TDATA1, 0);
            dm_write_csr(CSR_TDATA2, 0);
        }
    }
    memset(s_breakpoints, 0, sizeof(s_breakpoints));
}

static int jtag_check_breakpoints(int *hit_thread_id, uint32_t *hit_addr)
{
    if (!s_initialized) return 0;
    if (s_halted) return 0;

    if (!dm_is_halted()) return 0;

    /* Hart halted spontaneously — likely hit a trigger */
    s_halted = true;
    save_scratch_regs();

    uint32_t dcsr;
    if (dm_read_csr(AC_REG_DCSR, &dcsr) == 0) {
        uint32_t cause = (dcsr >> 6) & 0x7;
        if (cause == 2) { /* trigger */
            uint32_t dpc;
            if (dm_read_csr(AC_REG_DPC, &dpc) == 0) {
                if (hit_thread_id) *hit_thread_id = 1;
                if (hit_addr) *hit_addr = dpc;
                return 1;
            }
        }
    }

    /* Halted for other reason (ebreak, step, etc.) */
    if (hit_thread_id) *hit_thread_id = 1;
    uint32_t dpc;
    if (dm_read_csr(AC_REG_DPC, &dpc) == 0) {
        if (hit_addr) *hit_addr = dpc;
    }
    return 1;
}

const target_ops_t target_jtag_ops = {
    .name = "jtag",
    .init = jtag_backend_init,
    .halt = jtag_halt,
    .resume = jtag_resume,
    .is_halted = jtag_is_halted,
    .get_threads = jtag_get_threads,
    .read_registers = jtag_read_registers,
    .write_register = jtag_write_register,
    .read_memory = jtag_read_memory,
    .write_memory = jtag_write_memory,
    .set_breakpoint = jtag_set_breakpoint,
    .clear_breakpoint = jtag_clear_breakpoint,
    .clear_all_breakpoints = jtag_clear_all_breakpoints,
    .check_breakpoints = jtag_check_breakpoints,
};
