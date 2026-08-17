# esp32-gdb-mqtt

A wireless GDB debug server on an ESP32-C3 that communicates over MQTT. Debug a RISC-V soft core on an FPGA (or the ESP32's own FreeRTOS tasks) from any machine with network access -- no USB cables to the target.

## How It Works

```
┌──────────┐    TCP     ┌──────────────┐   MQTT    ┌──────────────┐   JTAG   ┌───────────┐
│   GDB    ├───────────►│ gdb_mqtt_    │◄─────────►│  ESP32-C3    ├─────────►│ FPGA soft │
│ (host)   │ :3333      │ bridge.py    │  broker    │  GDB server  │ GPIO     │ RISC-V    │
└──────────┘            └──────────────┘           └──────────────┘          └───────────┘
```

1. A Python bridge (`host/gdb_mqtt_bridge.py`) listens on a TCP port and relays GDB RSP packets to/from an MQTT broker.
2. The ESP32-C3 firmware subscribes to the same MQTT topics and implements the GDB Remote Serial Protocol.
3. The GDB server drives a JTAG TAP via GPIO bit-bang to access the RISC-V Debug Module (Debug Spec 0.13) on an FPGA soft core.

Standard GDB attaches with `target remote localhost:3333` and works normally -- breakpoints, register/memory inspection, single-step, continue. The MQTT transport is invisible to GDB.

## Features

- **Dual debug backends** -- switch at runtime between debugging the FPGA soft core (JTAG) and the ESP32's own FreeRTOS tasks (local self-debug)
- **Two JTAG transport modes** -- ECP5 JTAGG tunneling (for ULX3S) or direct GPIO-to-TAP (for Tang Primer 25K or any board exposing JTAG signals)
- **FPGA bitstream programming** -- stream a bitstream from an HTTP URL directly into an ECP5 via JTAG (no full-file RAM buffering)
- **Binary loading** -- load a bare-metal RISC-V binary into soft core memory and start execution, all from within GDB
- **RAM filesystem** -- upload files to the ESP32 over MQTT for later use (bitstreams, binaries)
- **UART console relay** -- bridge an FPGA UART to MQTT for remote serial console access
- **Interactive device configuration** -- WiFi credentials, MQTT broker, and device ID configurable at boot or via NVS

## Supported FPGA Targets

| Board | FPGA | Soft Core | JTAG Mode | Bitstream Programming |
|-------|------|-----------|-----------|----------------------|
| ULX3S | Lattice ECP5 (LFE5U-12F/25F/45F/85F) | VexRiscv (standard+debug) | ECP5 JTAGG tunneling | Yes (via JTAG) |
| Tang Primer 25K | Gowin GW5A-LV25MG121 | VexRiscv SMP (jtag_tap) | Direct GPIO | No (use openFPGALoader) |

Both SoC builds live in the companion [`test-bitstreams`](../fpga/test-bitstreams/) repository.

## Project Structure

```
esp32-gdb-mqtt/
├── src/
│   ├── main.c                 # Entry point, initialization sequence
│   ├── gdb_server.{c,h}      # GDB RSP protocol handler
│   ├── target_iface.h        # Debug backend vtable interface
│   ├── target.{c,h}          # Backend dispatcher and runtime switching
│   ├── target_local.{c,h}    # Local backend: ESP32 FreeRTOS self-debug
│   ├── target_jtag.{c,h}     # JTAG backend: external RISC-V soft core
│   ├── jtag_bitbang.{c,h}    # GPIO bit-bang JTAG with full TAP FSM
│   ├── riscv_dm.{c,h}        # RISC-V Debug Module (Spec 0.13) driver
│   ├── ecp5_jtag.{c,h}       # ECP5 JTAG: IDCODE, bitstream prog, JTAGG tunneling
│   ├── mqtt_transport.{c,h}  # MQTT connection, pub/sub, byte queues
│   ├── wifi.{c,h}            # WiFi station initialization
│   ├── device_config.{c,h}   # NVS-backed configuration with boot-time prompt
│   ├── file_manager.{c,h}    # MQTT-driven HTTP file download to ramfs
│   ├── ramfs.{c,h}           # In-memory filesystem (8 files, 64KB each)
│   ├── uart_relay.{c,h}      # UART-to-MQTT serial console bridge
│   ├── demo_task.{c,h}       # Two counter tasks for local-backend demo
│   ├── Kconfig.projbuild     # Menuconfig options
│   └── CMakeLists.txt         # ESP-IDF component registration
├── host/
│   └── gdb_mqtt_bridge.py    # Host-side TCP-to-MQTT bridge for GDB
├── docs/
│   ├── fpga-toolchain-setup.md    # ECP5/Gowin toolchain installation
│   └── tang-primer-25k-setup.md   # Tang Primer 25K wiring and setup
├── platformio.ini
└── sdkconfig.defaults
```

## Building

### Prerequisites

- [PlatformIO](https://platformio.org/) with ESP-IDF framework
- An MQTT broker (e.g. Mosquitto)
- Python 3 with `paho-mqtt` for the host bridge

### Configure

Run menuconfig to set WiFi credentials, MQTT broker, device ID, JTAG transport, and pin assignments:

```bash
pio run -t menuconfig
```

Key settings:

| Menu | Option | Default | Notes |
|------|--------|---------|-------|
| GDB over MQTT | WiFi SSID | `myssid` | |
| GDB over MQTT | WiFi Password | `mypassword` | |
| GDB over MQTT | MQTT Broker URI | `mqtt://192.168.1.1:1883` | |
| GDB over MQTT | Device ID | `esp32c3-001` | Used in all MQTT topic paths |
| JTAG transport | ECP5 / Direct | ECP5 | Direct for Tang Primer 25K |
| JTAG Bit-Bang | TCK/TMS/TDI/TDO pins | 2/3/4/5 | |
| UART Console Relay | Enable | off | Enable for FPGA serial console |
| UART Console Relay | TX/RX/Baud | 6/7/115200 | ESP32 UART1 pins |
| File Manager | Max file size | 64 KB | |

Or set defaults in `sdkconfig.defaults` and skip menuconfig.

### Build and Flash

```bash
pio run -t upload
```

## Usage

### 1. Start the Host Bridge

```bash
pip install paho-mqtt
python3 host/gdb_mqtt_bridge.py --broker <broker-ip> --device <device-id>
```

The bridge listens on TCP port 3333 (configurable with `--port`).

### 2. Connect GDB

```bash
riscv32-esp-elf-gdb
(gdb) target remote localhost:3333
```

### 3. GDB Monitor Commands

These are invoked from GDB with `monitor <command>`:

| Command | Description |
|---------|-------------|
| `target local` | Switch to local backend (debug ESP32 FreeRTOS tasks) |
| `target jtag` | Switch to JTAG backend (debug FPGA soft core) |
| `target` | Print the active backend name |
| `fpga_idcode` | Read ECP5 JTAG IDCODE (ECP5 transport only) |
| `fpga_status` | Read ECP5 status register (ECP5 transport only) |
| `fpga_stream <url>` | Stream a bitstream from HTTP into the ECP5 (ECP5 transport only) |
| `fpga_program <file>` | Program ECP5 from a file in ramfs (ECP5 transport only) |
| `riscv_load <file> [addr]` | Load a binary into soft core memory (default 0x10000000), set PC, resume |
| `files` | List files in ramfs |

### 4. Typical JTAG Workflow

```
(gdb) monitor target jtag          # switch to JTAG backend
(gdb) monitor files                 # see what's in ramfs
(gdb) monitor riscv_load hello.bin  # load and run a binary
(gdb) info registers                # inspect CPU state
(gdb) x/16x 0x10000000             # examine SRAM contents
```

### 5. Upload Files via MQTT

Send a JSON command to `device/<id>/file/cmd`:

```bash
# Download a file from HTTP into the ESP32's ramfs
mosquitto_pub -h <broker> -t "device/<id>/file/cmd" \
  -m '{"action":"download","url":"http://server/hello.bin","name":"hello.bin"}'

# List files
mosquitto_pub -h <broker> -t "device/<id>/file/cmd" \
  -m '{"action":"list"}'

# Delete a file
mosquitto_pub -h <broker> -t "device/<id>/file/cmd" \
  -m '{"action":"delete","name":"hello.bin"}'
```

Responses arrive on `device/<id>/file/resp`.

### 6. UART Console Relay

When enabled, the ESP32 bridges UART1 to MQTT for remote serial console access (e.g. a LiteX BIOS prompt):

```bash
# Read console output
mosquitto_sub -h <broker> -t "device/<id>/console/out"

# Send keystrokes
mosquitto_pub -h <broker> -t "device/<id>/console/in" -m "help"
```

## MQTT Topic Map

All topics are prefixed with `device/{device_id}/`:

| Topic | Direction | Content |
|-------|-----------|---------|
| `gdb/cmd` | host -> device | GDB RSP packets |
| `gdb/resp` | device -> host | GDB RSP responses |
| `file/cmd` | host -> device | JSON file commands |
| `file/resp` | device -> host | JSON file responses |
| `console/out` | device -> host | UART data from FPGA |
| `console/in` | host -> device | UART data to FPGA |

## Architecture

### Debug Backend Abstraction

The GDB server is target-agnostic. All target operations go through a vtable (`target_ops_t`) with two implementations:

**Local backend** (`target_local.c`): Debugs the ESP32-C3 itself. Suspends/resumes FreeRTOS tasks, reads RISC-V registers from exception frames in the TCB, accesses memory directly. Useful for debugging application firmware running on the ESP32.

**JTAG backend** (`target_jtag.c`): Debugs an external RISC-V soft core through the Debug Module. Halt/resume via DM registers, GPR access via abstract commands, memory access via program buffer, hardware trigger breakpoints. Single-hart operation.

Switch backends at runtime with `monitor target local` or `monitor target jtag`.

### JTAG Transport Layer

The RISC-V Debug Module driver (`riscv_dm.c`) uses a compile-time selected transport:

**ECP5 JTAGG tunneling** (`ecp5_jtag.c`): The VexRiscv debug TAP sits behind the ECP5's JTAGG primitive. Each bit of the inner TAP is clocked by shifting 2-bit {tms,tdi} pairs through the ECP5's ER1/ER2 registers. This also enables bitstream programming via the ECP5 JTAG port.

**Direct JTAG** (`jtag_bitbang.c` only): The soft core's JTAG TAP is directly wired to ESP32 GPIO pins. The 5-bit DTM IR (DTMCS, DMI) and variable-length DR (41-bit DMI frames) are shifted directly. Simpler and faster, but no FPGA programming capability.

Select with `CONFIG_JTAG_TRANSPORT_ECP5` or `CONFIG_JTAG_TRANSPORT_DIRECT` in menuconfig.

### GPIO Bit-Bang JTAG

`jtag_bitbang.c` drives TCK/TMS/TDI/TDO via GPIO with a full 16-state TAP FSM. Navigates to Shift-IR or Shift-DR, shifts data LSB-first, and manages state transitions. Clock delay is configurable in microseconds.

## Hardware

### ESP32-C3-DevKitM-1

- RISC-V single-core @ 160 MHz
- WiFi 802.11 b/g/n
- 400 KB SRAM, 4 MB flash
- GPIOs 2-5: JTAG (TCK, TMS, TDI, TDO)
- GPIO 6-7: UART1 (console relay TX/RX)

### Pin Assignment Summary

| GPIO | Function |
|------|----------|
| 2 | JTAG TCK |
| 3 | JTAG TMS |
| 4 | JTAG TDI |
| 5 | JTAG TDO |
| 6 | UART relay TX (ESP32 -> FPGA RX) |
| 7 | UART relay RX (FPGA TX -> ESP32) |

All signals are 3.3V. No level shifting needed with either the ULX3S or Tang Primer 25K.

## Documentation

- [FPGA Toolchain Setup](docs/fpga-toolchain-setup.md) -- installing yosys, nextpnr, LiteX, and related tools
- [Tang Primer 25K Setup](docs/tang-primer-25k-setup.md) -- wiring, flashing, and testing with the Gowin GW5A board
