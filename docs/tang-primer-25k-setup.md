# Tang Primer 25K Setup Guide

Remote GDB debugging of a VexRiscv SMP soft core on a Sipeed Tang Primer 25K (Gowin GW5A-LV25MG121), using an ESP32-C3 as a wireless JTAG bridge over MQTT.

## Prerequisites

- Sipeed Tang Primer 25K board
- ESP32-C3 dev board (e.g. DevKitM-1)
- 7 male-to-female jumper wires
- MQTT broker reachable by the ESP32
- openFPGALoader installed (`brew install openfpgaloader` or from oss-cad-suite)

## 1. Flash the FPGA Bitstream

The bitstream contains a LiteX SoC with:
- VexRiscv SMP @ 50 MHz, RISC-V JTAG TAP on PMOD j4
- 128 KB boot ROM (LiteX BIOS), 8 KB SRAM
- UART (115200 baud), LED chaser, timer

```bash
openFPGALoader -b tangprimer25k ~/fpga/build/tang-primer-25k-soc/gateware/sipeed_tang_primer_25k.fs
```

To write to flash (persists across power cycles):

```bash
openFPGALoader -b tangprimer25k -f ~/fpga/build/tang-primer-25k-soc/gateware/sipeed_tang_primer_25k.fs
```

## 2. Wire the ESP32-C3 to the Tang Primer 25K

### JTAG (PMOD j4, top row)

| ESP32-C3 GPIO | Signal | PMOD j4 Pin | FPGA Pin |
|---------------|--------|-------------|----------|
| GPIO2         | TCK    | j4:0        | G11      |
| GPIO3         | TMS    | j4:3        | C11      |
| GPIO4         | TDI    | j4:1        | D11      |
| GPIO5         | TDO    | j4:2        | B11      |

### UART Console Relay (PMOD j4, bottom row)

| ESP32-C3 GPIO | Signal         | PMOD j4 Pin | FPGA Pin |
|---------------|----------------|-------------|----------|
| GPIO6         | TX (ESP->FPGA) | j4:5        | D10      |
| GPIO7         | RX (FPGA->ESP) | j4:4        | G10      |

### Ground

| ESP32-C3 | Tang Primer 25K |
|----------|-----------------|
| GND      | GND (PMOD j4 or any GND) |

Both boards are 3.3V logic -- no level shifting needed.

## 3. Configure and Flash the ESP32

Switch from ECP5 tunneling to direct JTAG transport and enable the UART relay. Either run menuconfig:

```bash
pio run -t menuconfig
```

And set:
- JTAG transport -> "Direct JTAG (GPIO to TAP)"
- UART Console Relay -> Enable
- Verify JTAG pin assignments match the table above (defaults are correct)
- Verify UART relay pins: TX=6, RX=7, Baud=115200

Or add to `sdkconfig.defaults`:

```
CONFIG_JTAG_TRANSPORT_DIRECT=y
CONFIG_UART_RELAY_ENABLE=y
CONFIG_UART_RELAY_TX_PIN=6
CONFIG_UART_RELAY_RX_PIN=7
CONFIG_UART_RELAY_BAUD=115200
```

Then build and flash:

```bash
pio run -t upload
```

## 4. Verify the Connection

### UART Console

Subscribe to the LiteX BIOS console output:

```bash
mosquitto_sub -h <broker> -t "device/<device-id>/console/out"
```

You should see the LiteX BIOS banner and `litex>` prompt after the FPGA is powered on. Send keystrokes with:

```bash
mosquitto_pub -h <broker> -t "device/<device-id>/console/in" -m "help"
```

### JTAG

Connect GDB over MQTT and check the debug module responds:

```
(gdb) target remote localhost:3333
(gdb) monitor target jtag
```

## 5. Load and Run Test Binaries

Three test binaries are pre-built in `~/fpga/test-bitstreams/`:

| Binary | Size | Behavior |
|--------|------|----------|
| `riscv-blink0/blink0.bin` | ~60 B | Slow blink on LED 0 |
| `riscv-blink1/blink1.bin` | ~80 B | Double-blink on LED 1 |
| `riscv-hello/hello.bin` | 226 B | Prints to UART, cycles LEDs |

Upload a binary to the ESP32's ramfs, then load it:

```
(gdb) monitor riscv_load blink0.bin
```

This halts the CPU, writes the binary to SRAM at 0x10000000, sets the PC, and resumes. Load `blink1.bin` next to confirm the cycle works -- you should see the LED pattern change.

To rebuild the test binaries:

```bash
cd ~/fpga/test-bitstreams
export PATH="$HOME/.platformio/packages/toolchain-riscv32-esp/bin:$PATH"
make riscv
```

## SoC Memory Map

| Region | Base       | Size   |
|--------|------------|--------|
| ROM    | 0x00000000 | 128 KB |
| SRAM   | 0x10000000 | 8 KB   |
| CSR    | 0xf0000000 | 64 KB  |
| CLINT  | 0xf0010000 | 64 KB  |
| PLIC   | 0xf0c00000 | 4 MB   |

### CSR Registers

| Register          | Address    | Description |
|-------------------|------------|-------------|
| led_chaser_out    | 0xf0001800 | LED output (2 bits) |
| uart_rxtx         | 0xf0002800 | UART data (R/W) |
| uart_txfull       | 0xf0002804 | UART TX full flag |
| uart_rxempty      | 0xf0002808 | UART RX empty flag |
| timer0_load       | 0xf0002000 | Timer load value |
| timer0_en         | 0xf0002008 | Timer enable |
| timer0_value      | 0xf0002010 | Timer current value |

## Rebuilding the Bitstream

If you need to regenerate the FPGA bitstream (e.g. after changing the SoC configuration):

```bash
source ~/fpga/oss-cad-suite/environment
cd ~/fpga/test-bitstreams
python3 build_soc_tang.py
```

This runs the full flow: LiteX SoC generation -> yosys synthesis -> nextpnr place-and-route -> gowin_pack bitstream. Output lands in `~/fpga/build/tang-primer-25k-soc/gateware/sipeed_tang_primer_25k.fs`.

## Troubleshooting

**No UART output from BIOS**: Check that serial_tx on the Tang Primer is wired to GPIO7 (RX) on the ESP32, not the other way around. Verify baud rate is 115200 on both sides.

**JTAG not responding**: Verify `monitor target jtag` was issued (defaults to local backend). Check TCK/TMS/TDI/TDO wiring matches the pin table. The JTAG TAP runs in its own clock domain -- no frequency matching needed.

**riscv_load hangs**: The debug module may not have halted the CPU. Try `monitor target jtag` again to reinitialize. Check that the FPGA bitstream was flashed successfully (DONE LED should be solid green, READY blinking).

**openFPGALoader doesn't detect the board**: Try `openFPGALoader --detect` to list connected devices. The Tang Primer 25K uses a BL616 USB-JTAG bridge.
