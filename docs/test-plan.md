# ESP32 GDB-MQTT Test Plan

Hardware required:
- ESP32-C3-DevKitM-1
- Sipeed Tang Primer 25K Dock
- 7 jumper wires (male-to-female)
- Micro-USB cable (ESP32-C3)
- USB-C cable (Tang Primer 25K)
- MQTT broker (mosquitto) reachable by the ESP32 over WiFi

## Phase 1: ESP32-C3 Standalone

No FPGA needed. Tests the ESP32 firmware, WiFi, MQTT, and local debug backend.

### 1.1 Build and Flash

- [ ] `pio run` completes without errors
- [ ] `pio run -t upload` flashes the ESP32-C3 successfully
- [ ] Serial monitor shows boot log (`pio device monitor`)

### 1.2 Device Configuration

- [ ] Boot prompt appears on serial for 3 seconds
- [ ] Can enter WiFi SSID/password/MQTT URI via serial prompt
- [ ] Config persists across reboot (stored in NVS)
- [ ] Default Kconfig values work if prompt is skipped

### 1.3 WiFi Connection

- [ ] ESP32 connects to WiFi and obtains IP
- [ ] Boot log shows IP address
- [ ] Reconnection behavior: power cycle the ESP32, verify it reconnects

### 1.4 MQTT Connection

- [ ] ESP32 connects to MQTT broker
- [ ] `mosquitto_sub -t "device/+/#"` shows the device subscribing to its topics
- [ ] Verify topic structure: `device/<device-id>/gdb`, `device/<device-id>/gdb/resp`, `device/<device-id>/file/cmd`, etc.

### 1.5 Local Debug Backend (GDB over MQTT)

Start the host-side bridge:
```bash
python3 host/gdb_mqtt_bridge.py --broker <broker-ip> --device <device-id>
```

- [ ] Bridge connects and listens on localhost:3333
- [ ] GDB connects: `riscv32-esp-elf-gdb -ex "target remote localhost:3333"`
- [ ] `info threads` lists FreeRTOS tasks (counter_a, counter_b, etc.)
- [ ] `info registers` returns register values for current thread
- [ ] `x/10x <address>` reads memory
- [ ] `thread <n>` switches threads
- [ ] `monitor help` returns command list
- [ ] `monitor target local` confirms local backend active
- [ ] Ctrl-C halts (suspends tasks)
- [ ] `continue` resumes

### 1.6 Demo Tasks

- [ ] counter_a and counter_b tasks appear in thread list
- [ ] Reading counter globals shows incrementing values
- [ ] Setting a breakpoint on a counter task address works (note: polling-based, may miss)

### 1.7 File Manager

- [ ] `mosquitto_pub -t "device/<id>/file/cmd" -m '{"cmd":"list"}'` returns file list on `device/<id>/file/resp`
- [ ] Download a small test file: `{"cmd":"download","url":"http://<host>/test.bin","name":"test.bin"}`
- [ ] Verify file appears in ramfs: `{"cmd":"list"}`
- [ ] Delete file: `{"cmd":"delete","name":"test.bin"}`
- [ ] Verify file removed: `{"cmd":"list"}`

### 1.8 UART Relay (if enabled)

Requires rebuilding with `CONFIG_UART_RELAY_ENABLE=y`. Skip until Phase 3.

## Phase 2: Tang Primer 25K FPGA Setup

No ESP32 wiring yet. Verify the FPGA board works standalone.

### 2.1 Board Detection

- [x] Connect Tang Primer 25K via USB-C
- [x] `openFPGALoader --detect` recognizes the board (Gowin GW5A-25, idcode 0x1281b)
- [x] Note: board has no user LEDs — LED chaser output (E8, D7) not connected on this dock variant. POWER/READY/DONE are status LEDs only.

### 2.2 Bitstream Flash

- [x] Flash to SRAM: `openFPGALoader -b tangprimer25k ~/fpga/build/tang-primer-25k-soc/gateware/sipeed_tang_primer_25k.fs`
- [x] DONE LED solid green after programming (bitstream loaded into fabric)
- [x] Flash to persistent storage: built openFPGALoader from source with XTX XT25F64B (0x0b4017) added to `spiFlashdb.hpp`. Binary at `~/fpga/openFPGALoader/build/openFPGALoader`.
- [x] Power cycle persistence: DONE LED solid after power cycle, READY blinking (LiteX SoC running).

### 2.3 UART Verify (Optional)

If the Tang Primer exposes a USB-serial port:
- [ ] Connect to it at 115200 baud
- [ ] LiteX BIOS banner and `litex>` prompt appear
- [ ] `help` command works

## Phase 3: ESP32 + FPGA Integration

Wire the ESP32-C3 to the Tang Primer 25K per the pin table in `docs/tang-primer-25k-setup.md`.

### 3.1 Wiring

| ESP32-C3 GPIO | Signal         | PMOD j4 Pin | FPGA Pin |
|---------------|----------------|-------------|----------|
| GPIO2         | TCK            | j4:0        | G11      |
| GPIO3         | TMS            | j4:3        | C11      |
| GPIO4         | TDI            | j4:1        | D11      |
| GPIO5         | TDO            | j4:2        | B11      |
| GPIO6         | TX (ESP->FPGA) | j4:5        | D10      |
| GPIO7         | RX (FPGA->ESP) | j4:4        | G10      |
| GND           | GND            | GND         | GND      |

### 3.2 Build for Direct JTAG

Rebuild ESP32 firmware with direct JTAG transport:
```bash
# Either via menuconfig:
pio run -t menuconfig
# Set JTAG transport -> "Direct JTAG (GPIO to TAP)"
# Enable UART Console Relay

# Or add to sdkconfig.defaults:
# CONFIG_JTAG_TRANSPORT_DIRECT=y
# CONFIG_UART_RELAY_ENABLE=y
```
- [ ] `pio run` builds with direct JTAG transport
- [ ] `pio run -t upload` flashes successfully

### 3.3 JTAG Connection

Start the bridge in one terminal, subscribe to console output in another:
```bash
# Terminal 1: bridge
python3 host/gdb_mqtt_bridge.py --broker <broker-ip> --device <device-id>

# Terminal 2: UART console (keep open for all subsequent tests)
mosquitto_sub -h <broker> -t "device/<device-id>/console/out"
```

Connect GDB and switch to the JTAG backend:
```
riscv32-esp-elf-gdb
(gdb) target remote localhost:3333
(gdb) monitor target jtag
(gdb) flushregs
```

Note: `flushregs` is required after `monitor target jtag` — GDB caches registers from the previous backend (ESP32 local) and `monitor` commands don't invalidate the cache. Without it, `info registers` shows stale ESP32 values (0x3fc... addresses).

- [ ] `monitor target jtag` — ESP32 serial log shows "Debug Module active, dmstatus=0x004c0c82" (or similar, version 0.13, authenticated)
- [ ] No JTAG errors in ESP32 serial log
- [ ] `info registers` — shows VexRiscv values (sp in 0x10000000 range, not 0x3fc...)
- [ ] `x/4x 0x00000000` — reads ROM region (non-zero: LiteX BIOS code)
- [ ] `x/4x 0x10000000` — reads SRAM region

### 3.4 Halt and Resume

```
(gdb) continue
(gdb) [Ctrl-C]
(gdb) info registers
(gdb) continue
```

- [ ] Ctrl-C halts the VexRiscv (ESP32 serial log shows "Hart halted")
- [ ] `info registers` works while halted
- [ ] `continue` resumes (ESP32 serial log shows "Hart resumed")

### 3.5 Memory Write Verification

This must pass before attempting `riscv_load`. Memory writes use the Debug Module program buffer (`sw s1, 0(s0)`) which has not been tested through the tunnel protocol.

```
(gdb) [Ctrl-C if not already halted]
(gdb) set *0x10000000 = 0xdeadbeef
(gdb) x/x 0x10000000
```

Expected: `0x10000000: 0xdeadbeef`

```
(gdb) set *0x10000004 = 0xcafef00d
(gdb) x/2x 0x10000000
```

Expected: `0x10000000: 0xdeadbeef 0xcafef00d`

- [ ] Single word write + readback matches
- [ ] Second word write at adjacent address works (tests sequential writes)
- [ ] CSR write: `set *0xf0001800 = 0x3` then `x/x 0xf0001800` (no visible LEDs, but register should read back)

If writes fail (readback shows old data or zeros), check ESP32 serial log for DMI errors — likely a tunnel timing issue with the program buffer POSTEXEC sequence.

### 3.6 UART Console Relay

Verify the LiteX BIOS is reachable before loading test binaries (console/out is how we observe binary output).

```bash
# Terminal 2 should already be running mosquitto_sub

# Terminal 3: send a command to the BIOS
mosquitto_pub -h <broker> -t "device/<device-id>/console/in" -m "help"
```

- [ ] LiteX BIOS banner visible on console/out after FPGA power-on
- [ ] `help` command returns BIOS help text on console/out
- [ ] `ident` command returns board identifier

### 3.7 Load Test Binaries

Requires: memory writes working (3.5) and UART relay working (3.6).

Note: Tang Primer 25K dock has no user LEDs. Test binaries use UART output.

Start a local HTTP server to serve the test binaries to the ESP32. The ESP32's file manager downloads files over HTTP, so this must be reachable from the ESP32's WiFi network:

```bash
# Terminal 4: serve test binaries (run from the riscv/ directory)
cd /path/to/esp32-gdb-mqtt-test-bitstreams/riscv
python3 -m http.server 8080
```

Verify the server is reachable by checking your machine's IP on the same network as the ESP32 (e.g. `ifconfig en0` or `ip addr`). The ESP32 will fetch from `http://<your-ip>:8080/<filename>`.

Upload binaries to ESP32 ramfs:
```bash
mosquitto_pub -h <broker> -t "device/<device-id>/file/cmd" \
  -m '{"action":"download","url":"http://<your-ip>:8080/hello.bin","name":"hello.bin"}'
mosquitto_pub -h <broker> -t "device/<device-id>/file/cmd" \
  -m '{"action":"download","url":"http://<your-ip>:8080/count.bin","name":"count.bin"}'
```

Load and verify hello.bin:
```
(gdb) monitor riscv_load hello.bin
```

- [ ] GDB prints "Loaded hello.bin (87 bytes) at 0x10000000, running"
- [ ] "Hello from VexRiscv!" appears on console/out (Terminal 2)

Load and verify count.bin:
```
(gdb) monitor riscv_load count.bin
```

- [ ] "0x00000000", "0x00000001", ... appears on console/out, incrementing every ~0.5s
- [ ] Ctrl-C halts — counter stops
- [ ] `info registers` — s0 holds the current count value
- [ ] `continue` — counter resumes from where it left off (not from 0)

### 3.8 Breakpoints

Requires: FPGA bitstream built with `hardware_breakpoints = 2` in `build_soc_tang.py`.
The bitstream build script is in `esp32-gdb-mqtt-test-bitstreams/riscv/build_soc_tang.py`.

#### 3.8.1 Entry Breakpoint (hello.bin)

```
(gdb) monitor riscv_load hello.bin
(gdb) break *0x10000000
(gdb) continue
```

- [ ] Execution halts at 0x10000000 (before printing anything)
- [ ] `info breakpoints` shows the breakpoint
- [ ] `info registers` — PC is 0x10000000
- [ ] `continue` — "Hello from VexRiscv!" appears on console/out
- [ ] `delete 1` clears the breakpoint

#### 3.8.2 Loop Breakpoint and Continue (count.bin)

count.bin is a tight loop: `_start` at 0x10000000 (executes once), `loop` at 0x10000004 (repeats). This tests that GDB's step-past-breakpoint dance works (remove bp → single step → re-insert bp → continue).

```
(gdb) monitor riscv_load count.bin
(gdb) break *0x10000000
(gdb) continue
```

- [ ] "Thread 1 hit Breakpoint 1, 0x10000000" — halts at entry before any output
- [ ] `info registers` — PC is 0x10000000
- [ ] `continue` — counter output starts on console/out (0x00000000, 0x00000001, ...)
- [ ] Note: breakpoint at 0x10000000 only fires once because the loop jumps to 0x10000004

```
(gdb) delete 1
(gdb) break *0x10000004
(gdb) continue
```

- [ ] Breakpoint fires at 0x10000004 (inside the loop)
- [ ] `continue` again — breakpoint fires again (step-past-breakpoint works)
- [ ] `info registers` — fp (s0/x8) holds the current count value, incrementing between hits
- [ ] `delete` all breakpoints, `continue` — counter runs freely on console/out

### 3.9 Backend Switching

```
(gdb) monitor target local
(gdb) flushregs
(gdb) info threads
(gdb) monitor target jtag
(gdb) flushregs
(gdb) x/4x 0x10000000
```

- [ ] `monitor target local` — `info threads` shows FreeRTOS tasks
- [ ] `monitor target jtag` — JTAG operations still work after round-trip
- [ ] Memory contents from previous JTAG session are preserved

## Phase 4: Edge Cases

### 4.1 Disconnect Recovery

- [ ] Kill the MQTT bridge while GDB is connected, restart bridge, reconnect GDB — ESP32 recovers
- [ ] Disconnect ESP32 WiFi (power cycle router briefly) — ESP32 reconnects and GDB session can resume
- [ ] Unplug FPGA USB while JTAG is active — JTAG operations fail gracefully (error, no crash)

### 4.2 Large Transfers

- [ ] Download a 64KB file via file manager (max default size)
- [ ] Load it with riscv_load — verify it fails gracefully (8KB SRAM limit)
- [ ] Read 512 bytes of memory in one GDB `x` command

### 4.3 Multiple GDB Sessions

- [ ] Connect two GDB instances to the bridge — verify behavior (expect second to fail or queue)

