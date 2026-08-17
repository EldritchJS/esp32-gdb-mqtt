#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ECP5_IDCODE_LFE5U_25F  0x41111043
#define ECP5_IDCODE_LFE5U_45F  0x41112043
#define ECP5_IDCODE_LFE5U_85F  0x41113043

/* ECP5 JTAG IR instructions (8-bit IR) */
#define ECP5_IR_IDCODE         0xE0
#define ECP5_IR_ISC_ENABLE     0xC6
#define ECP5_IR_ISC_DISABLE    0x26
#define ECP5_IR_LSC_BITSTREAM_BURST 0x7A
#define ECP5_IR_LSC_CHECK_BUSY 0xF0
#define ECP5_IR_LSC_READ_STATUS 0x3C
#define ECP5_IR_ISC_ERASE      0x0E
#define ECP5_IR_LSC_INIT_ADDRESS 0x46
#define ECP5_IR_LSC_PROG_INCR_NV 0x70
#define ECP5_IR_REFRESH        0x79
#define ECP5_IR_BYPASS         0xFF

/* ECP5 JTAGG user instructions for tunneling to soft core */
#define ECP5_IR_ER1            0x32
#define ECP5_IR_ER2            0x38

/* ECP5 status register bits */
#define ECP5_STATUS_DONE       (1 << 8)
#define ECP5_STATUS_BUSY       (1 << 12)
#define ECP5_STATUS_FAIL       (1 << 13)

int ecp5_init(void);

uint32_t ecp5_read_idcode(void);

int ecp5_program_bitstream(const uint8_t *bitstream, size_t len);

int ecp5_program_begin(void);
int ecp5_program_data(const uint8_t *data, size_t len);
int ecp5_program_end(void);

bool ecp5_is_done(void);
uint32_t ecp5_read_status(void);

void ecp5_jtagg_shift_ir(const uint8_t *tdi, uint8_t *tdo, size_t bits);
void ecp5_jtagg_shift_dr(const uint8_t *tdi, uint8_t *tdo, size_t bits);
