#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TARGET_NUM_REGS 33
#define TARGET_REG_PC   32
#define TARGET_MAX_BREAKPOINTS 8
#define TARGET_MAX_THREADS 32
#define TARGET_THREAD_NAME_LEN 32

typedef struct {
    int id;
    char name[TARGET_THREAD_NAME_LEN];
    uint32_t priority;
    enum { TARGET_STATE_RUNNING, TARGET_STATE_HALTED,
           TARGET_STATE_BLOCKED, TARGET_STATE_UNKNOWN } state;
} target_thread_info_t;

typedef struct target_ops {
    const char *name;

    int (*init)(void);
    int (*halt)(void);
    int (*resume)(void);
    bool (*is_halted)(void);

    int (*get_threads)(target_thread_info_t *list, int max);
    int (*read_registers)(int thread_id, uint32_t *regs);
    int (*write_register)(int thread_id, int reg_num, uint32_t value);

    int (*read_memory)(uint32_t addr, uint8_t *buf, size_t len);
    int (*write_memory)(uint32_t addr, const uint8_t *buf, size_t len);

    int (*set_breakpoint)(uint32_t addr);
    int (*clear_breakpoint)(uint32_t addr);
    void (*clear_all_breakpoints)(void);
    int (*check_breakpoints)(int *hit_thread_id, uint32_t *hit_addr);
} target_ops_t;
