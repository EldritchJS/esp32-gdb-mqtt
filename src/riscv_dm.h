#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * RISC-V Debug Module Interface (DMI) — Debug Spec 0.13
 *
 * Transport-agnostic: works via ECP5 JTAGG tunneling or direct
 * GPIO JTAG to a soft core's Debug Transport Module (DTM).
 *
 * DTM IR/DR:
 *   IR=0x01 → DTMCS (DTM control/status)
 *   IR=0x11 → DMI  (debug module interface, variable width)
 *
 * DMI DR format (for VexRiscv: 7-bit address, 32-bit data, 2-bit op):
 *   [1:0]  op      — 0=nop, 1=read, 2=write, 3=reserved
 *   [33:2] data    — 32-bit read/write data
 *   [40:34] address — 7-bit DMI register address
 *   Total: 41 bits
 */

/* Outer TAP IR (VexRiscv SMP tunneled JTAG) */
#define TAP_IR_LEN       6
#define TAP_IR_TUNNEL    0x23
#define TAP_IR_IDCODE    0x01

/* Inner tunnel instructions */
#define TUNNEL_IR_DTMCS  0x10
#define TUNNEL_IR_DMI    0x11
#define TUNNEL_HDR_BITS  14

/* DMI field widths (VexRiscv) */
#define DMI_OP_BITS   2
#define DMI_DATA_BITS 32
#define DMI_ADDR_BITS 7
#define DMI_TOTAL_BITS (DMI_OP_BITS + DMI_DATA_BITS + DMI_ADDR_BITS)

/* Tunnel pipeline: TDI delayed 9 clocks, TDO delayed 4 clocks */
#define TUNNEL_TDI_DELAY  9
#define TUNNEL_TDO_DELAY  4
#define TUNNEL_DR_BITS    (DMI_TOTAL_BITS + TUNNEL_TDI_DELAY)

/* DMI op codes */
#define DMI_OP_NOP    0
#define DMI_OP_READ   1
#define DMI_OP_WRITE  2

/* DMI status (in op field of response) */
#define DMI_STATUS_OK    0
#define DMI_STATUS_FAIL  2
#define DMI_STATUS_BUSY  3

/* Debug Module register addresses */
#define DM_DATA0          0x04
#define DM_DATA1          0x05
#define DM_DMCONTROL      0x10
#define DM_DMSTATUS       0x11
#define DM_HARTINFO       0x12
#define DM_ABSTRACTCS     0x16
#define DM_COMMAND        0x17
#define DM_ABSTRACTAUTO   0x18
#define DM_PROGBUF0       0x20
#define DM_PROGBUF1       0x21
#define DM_SBCS           0x38
#define DM_SBADDRESS0     0x39
#define DM_SBDATA0        0x3C

/* dmcontrol bits */
#define DMCONTROL_DMACTIVE    (1 << 0)
#define DMCONTROL_NDMRESET    (1 << 1)
#define DMCONTROL_HALTREQ     (1U << 31)
#define DMCONTROL_RESUMEREQ   (1 << 30)

/* dmstatus bits */
#define DMSTATUS_ALLHALTED    (1 << 9)
#define DMSTATUS_ANYHALTED    (1 << 8)
#define DMSTATUS_ALLRUNNING   (1 << 11)
#define DMSTATUS_ANYRUNNING   (1 << 10)

/* abstractcs bits */
#define ABSTRACTCS_BUSY       (1 << 12)
#define ABSTRACTCS_CMDERR_MASK (7 << 8)

/* Abstract command: Access Register */
#define AC_ACCESS_REGISTER    0
#define AC_TRANSFER           (1 << 17)
#define AC_WRITE              (1 << 16)
#define AC_AARSIZE_32         (2 << 20)
#define AC_POSTEXEC           (1 << 18)
#define AC_REGNO(n)           ((n) & 0xFFFF)

/* GPR register numbers in abstract command space */
#define AC_REG_GPR_BASE  0x1000
#define AC_REG_DPC       0x07B1
#define AC_REG_DCSR      0x07B0

/* Trigger registers */
#define CSR_TSELECT      0x7A0
#define CSR_TDATA1       0x7A1
#define CSR_TDATA2       0x7A2

/* tdata1 for mcontrol (type=2) */
#define DM_MCONTROL_TYPE    (2UL << 28)
#define DM_MCONTROL_EXECUTE (1 << 2)
#define DM_MCONTROL_M_MODE  (1 << 6)
#define DM_MCONTROL_ACTION_HALT (0 << 12)

int dm_init(void);
int dm_read(uint8_t addr, uint32_t *value);
int dm_write(uint8_t addr, uint32_t value);
int dm_halt(void);
int dm_resume(void);
bool dm_is_halted(void);
int dm_read_gpr(int reg_num, uint32_t *value);
int dm_write_gpr(int reg_num, uint32_t value);
int dm_read_csr(int csr, uint32_t *value);
int dm_write_csr(int csr, uint32_t value);
int dm_read_memory(uint32_t addr, uint8_t *buf, size_t len);
int dm_write_memory(uint32_t addr, const uint8_t *buf, size_t len);
