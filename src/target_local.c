#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "riscv/rvruntime-frames.h"
#include "target_local.h"

static const char *TAG = "target-local";

#define MAX_EXCLUDED 4
#define MAX_SUSPENDED 32

static TaskHandle_t s_excluded[MAX_EXCLUDED];
static int s_excluded_count;

static TaskHandle_t s_suspended[MAX_SUSPENDED];
static int s_suspended_count;
static bool s_halted;

static struct {
    uint32_t addr;
    bool active;
} s_breakpoints[TARGET_MAX_BREAKPOINTS];

static TaskHandle_t s_thread_handles[TARGET_MAX_THREADS];
static int s_thread_count;

void target_local_set_excluded_task(TaskHandle_t handle)
{
    if (s_excluded_count < MAX_EXCLUDED) {
        s_excluded[s_excluded_count++] = handle;
    }
}

static bool is_excluded(TaskHandle_t handle)
{
    for (int i = 0; i < s_excluded_count; i++) {
        if (s_excluded[i] == handle) {
            return true;
        }
    }
    return false;
}

static bool is_system_task(const char *name)
{
    return (strcmp(name, "IDLE") == 0 ||
            strcmp(name, "IDLE0") == 0 ||
            strcmp(name, "IDLE1") == 0 ||
            strcmp(name, "Tmr Svc") == 0 ||
            strcmp(name, "ipc0") == 0 ||
            strcmp(name, "ipc1") == 0 ||
            strcmp(name, "esp_timer") == 0 ||
            strcmp(name, "tiT") == 0 ||
            strcmp(name, "tcpip") == 0 ||
            strcmp(name, "wifi") == 0 ||
            strcmp(name, "sys_evt") == 0 ||
            strcmp(name, "mqtt_tx") == 0 ||
            strcmp(name, "mqtt_task") == 0 ||
            strcmp(name, "file_mgr") == 0 ||
            strcmp(name, "uart_relay") == 0 ||
            strcmp(name, "gdb_srv") == 0);
}

static int local_init(void)
{
    s_excluded_count = 0;
    s_suspended_count = 0;
    s_halted = false;
    s_thread_count = 0;
    memset(s_breakpoints, 0, sizeof(s_breakpoints));
    return 0;
}

static int local_halt(void)
{
    if (s_halted) {
        return 0;
    }

    TaskStatus_t tasks[32];
    UBaseType_t count = uxTaskGetSystemState(tasks, 32, NULL);

    s_suspended_count = 0;
    for (UBaseType_t i = 0; i < count; i++) {
        if (is_excluded(tasks[i].xHandle)) {
            continue;
        }
        if (is_system_task(tasks[i].pcTaskName)) {
            continue;
        }
        if (tasks[i].eCurrentState == eSuspended) {
            continue;
        }
        if (s_suspended_count < MAX_SUSPENDED) {
            ESP_LOGI(TAG, "  suspend: %s", tasks[i].pcTaskName);
            vTaskSuspend(tasks[i].xHandle);
            s_suspended[s_suspended_count++] = tasks[i].xHandle;
        }
    }

    s_halted = true;
    ESP_LOGI(TAG, "Halted %d tasks", s_suspended_count);
    return s_suspended_count;
}

static int local_resume(void)
{
    if (!s_halted) {
        return 0;
    }

    for (int i = 0; i < s_suspended_count; i++) {
        vTaskResume(s_suspended[i]);
    }

    int count = s_suspended_count;
    s_suspended_count = 0;
    s_halted = false;
    ESP_LOGI(TAG, "Resumed %d tasks", count);
    return count;
}

static bool local_is_halted(void)
{
    return s_halted;
}

static int local_get_threads(target_thread_info_t *list, int max)
{
    TaskStatus_t tasks[32];
    UBaseType_t count = uxTaskGetSystemState(tasks, 32, NULL);

    int n = 0;
    s_thread_count = 0;
    for (UBaseType_t i = 0; i < count && n < max && n < TARGET_MAX_THREADS; i++) {
        list[n].id = n + 1;
        strncpy(list[n].name, tasks[i].pcTaskName, TARGET_THREAD_NAME_LEN - 1);
        list[n].name[TARGET_THREAD_NAME_LEN - 1] = '\0';
        list[n].priority = tasks[i].uxCurrentPriority;

        switch (tasks[i].eCurrentState) {
        case eRunning:   list[n].state = TARGET_STATE_RUNNING; break;
        case eReady:     list[n].state = TARGET_STATE_RUNNING; break;
        case eSuspended: list[n].state = TARGET_STATE_HALTED; break;
        case eBlocked:   list[n].state = TARGET_STATE_BLOCKED; break;
        default:         list[n].state = TARGET_STATE_UNKNOWN; break;
        }

        s_thread_handles[n] = tasks[i].xHandle;
        n++;
    }
    s_thread_count = n;
    return n;
}

static TaskHandle_t thread_id_to_handle(int thread_id)
{
    if (thread_id < 1 || thread_id > s_thread_count) {
        return NULL;
    }
    return s_thread_handles[thread_id - 1];
}

static int local_read_registers(int thread_id, uint32_t *regs)
{
    TaskHandle_t task = thread_id_to_handle(thread_id);
    if (!task) {
        memset(regs, 0, TARGET_NUM_REGS * sizeof(uint32_t));
        return -1;
    }

    StaticTask_t *tcb = (StaticTask_t *)task;
    const RvExcFrame *frame = (const RvExcFrame *)tcb->pxDummy1;

    if (!frame) {
        memset(regs, 0, TARGET_NUM_REGS * sizeof(uint32_t));
        return -1;
    }

    regs[0] = 0;
    memcpy(&regs[1], &frame->ra, 31 * sizeof(uint32_t));
    regs[TARGET_REG_PC] = frame->mepc;
    return 0;
}

static int local_write_register(int thread_id, int reg_num, uint32_t value)
{
    if (reg_num == 0) {
        return 0;
    }

    TaskHandle_t task = thread_id_to_handle(thread_id);
    if (!task) {
        return -1;
    }

    StaticTask_t *tcb = (StaticTask_t *)task;
    RvExcFrame *frame = (RvExcFrame *)tcb->pxDummy1;
    if (!frame) {
        return -1;
    }

    if (reg_num < 32) {
        (&frame->mepc)[reg_num] = value;
    } else if (reg_num == TARGET_REG_PC) {
        frame->mepc = value;
    } else {
        return -1;
    }
    return 0;
}

static int local_read_memory(uint32_t addr, uint8_t *buf, size_t len)
{
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)addr;
    for (size_t i = 0; i < len; i++) {
        buf[i] = src[i];
    }
    return 0;
}

static int local_write_memory(uint32_t addr, const uint8_t *buf, size_t len)
{
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)addr;
    for (size_t i = 0; i < len; i++) {
        dst[i] = buf[i];
    }
    return 0;
}

static int local_set_breakpoint(uint32_t addr)
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
            ESP_LOGI(TAG, "Breakpoint %d set at 0x%08lx", i, (unsigned long)addr);
            return 0;
        }
    }
    return -1;
}

static int local_clear_breakpoint(uint32_t addr)
{
    for (int i = 0; i < TARGET_MAX_BREAKPOINTS; i++) {
        if (s_breakpoints[i].active && s_breakpoints[i].addr == addr) {
            s_breakpoints[i].active = false;
            ESP_LOGI(TAG, "Breakpoint %d cleared at 0x%08lx", i, (unsigned long)addr);
            return 0;
        }
    }
    return -1;
}

static void local_clear_all_breakpoints(void)
{
    memset(s_breakpoints, 0, sizeof(s_breakpoints));
}

static int local_check_breakpoints(int *hit_thread_id, uint32_t *hit_addr)
{
    TaskStatus_t tasks[32];
    UBaseType_t count = uxTaskGetSystemState(tasks, 32, NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        if (is_excluded(tasks[i].xHandle) || is_system_task(tasks[i].pcTaskName)) {
            continue;
        }
        if (tasks[i].eCurrentState == eSuspended) {
            continue;
        }

        StaticTask_t *tcb = (StaticTask_t *)tasks[i].xHandle;
        const RvExcFrame *frame = (const RvExcFrame *)tcb->pxDummy1;
        if (!frame) continue;

        uint32_t pc = frame->mepc;
        for (int bp = 0; bp < TARGET_MAX_BREAKPOINTS; bp++) {
            if (s_breakpoints[bp].active && s_breakpoints[bp].addr == pc) {
                if (hit_thread_id) {
                    for (int t = 0; t < s_thread_count; t++) {
                        if (s_thread_handles[t] == tasks[i].xHandle) {
                            *hit_thread_id = t + 1;
                            break;
                        }
                    }
                }
                if (hit_addr) *hit_addr = pc;
                return 1;
            }
        }
    }
    return 0;
}

const target_ops_t target_local_ops = {
    .name = "local",
    .init = local_init,
    .halt = local_halt,
    .resume = local_resume,
    .is_halted = local_is_halted,
    .get_threads = local_get_threads,
    .read_registers = local_read_registers,
    .write_register = local_write_register,
    .read_memory = local_read_memory,
    .write_memory = local_write_memory,
    .set_breakpoint = local_set_breakpoint,
    .clear_breakpoint = local_clear_breakpoint,
    .clear_all_breakpoints = local_clear_all_breakpoints,
    .check_breakpoints = local_check_breakpoints,
};
