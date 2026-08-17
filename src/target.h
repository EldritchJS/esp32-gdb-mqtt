#pragma once

#include "target_iface.h"

void target_init(const target_ops_t *backend);
void target_set_backend(const target_ops_t *backend);
const char *target_backend_name(void);

int target_halt(void);
int target_continue(void);
bool target_is_halted(void);

int target_get_threads(target_thread_info_t *list, int max);
int target_read_registers(int thread_id, uint32_t *regs);
int target_write_register(int thread_id, int reg_num, uint32_t value);

int target_read_memory(uint32_t addr, uint8_t *buf, size_t len);
int target_write_memory(uint32_t addr, const uint8_t *buf, size_t len);

int target_set_breakpoint(uint32_t addr);
int target_clear_breakpoint(uint32_t addr);
void target_clear_all_breakpoints(void);
int target_check_breakpoints(int *hit_thread_id, uint32_t *hit_addr);
