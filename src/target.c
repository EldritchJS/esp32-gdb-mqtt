#include "esp_log.h"
#include "target.h"

static const char *TAG = "target";
static const target_ops_t *s_ops;

void target_init(const target_ops_t *backend)
{
    s_ops = backend;
    if (s_ops && s_ops->init) {
        s_ops->init();
    }
    ESP_LOGI(TAG, "Target backend: %s", s_ops ? s_ops->name : "none");
}

void target_set_backend(const target_ops_t *backend)
{
    if (s_ops && s_ops->is_halted && !s_ops->is_halted()) {
        if (s_ops->halt) s_ops->halt();
    }
    if (s_ops && s_ops->clear_all_breakpoints) {
        s_ops->clear_all_breakpoints();
    }

    s_ops = backend;
    if (s_ops && s_ops->init) {
        s_ops->init();
    }
    ESP_LOGI(TAG, "Switched to backend: %s", s_ops ? s_ops->name : "none");
}

const char *target_backend_name(void)
{
    return s_ops ? s_ops->name : "none";
}

int target_halt(void)
{
    return (s_ops && s_ops->halt) ? s_ops->halt() : -1;
}

int target_continue(void)
{
    return (s_ops && s_ops->resume) ? s_ops->resume() : -1;
}

bool target_is_halted(void)
{
    return (s_ops && s_ops->is_halted) ? s_ops->is_halted() : true;
}

int target_get_threads(target_thread_info_t *list, int max)
{
    return (s_ops && s_ops->get_threads) ? s_ops->get_threads(list, max) : 0;
}

int target_read_registers(int thread_id, uint32_t *regs)
{
    return (s_ops && s_ops->read_registers) ? s_ops->read_registers(thread_id, regs) : -1;
}

int target_write_register(int thread_id, int reg_num, uint32_t value)
{
    return (s_ops && s_ops->write_register) ? s_ops->write_register(thread_id, reg_num, value) : -1;
}

int target_read_memory(uint32_t addr, uint8_t *buf, size_t len)
{
    return (s_ops && s_ops->read_memory) ? s_ops->read_memory(addr, buf, len) : -1;
}

int target_write_memory(uint32_t addr, const uint8_t *buf, size_t len)
{
    return (s_ops && s_ops->write_memory) ? s_ops->write_memory(addr, buf, len) : -1;
}

int target_set_breakpoint(uint32_t addr)
{
    return (s_ops && s_ops->set_breakpoint) ? s_ops->set_breakpoint(addr) : -1;
}

int target_clear_breakpoint(uint32_t addr)
{
    return (s_ops && s_ops->clear_breakpoint) ? s_ops->clear_breakpoint(addr) : -1;
}

void target_clear_all_breakpoints(void)
{
    if (s_ops && s_ops->clear_all_breakpoints) {
        s_ops->clear_all_breakpoints();
    }
}

int target_check_breakpoints(int *hit_thread_id, uint32_t *hit_addr)
{
    return (s_ops && s_ops->check_breakpoints) ? s_ops->check_breakpoints(hit_thread_id, hit_addr) : 0;
}
