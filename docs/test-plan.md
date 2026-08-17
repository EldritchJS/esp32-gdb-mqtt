# ESP32 GDB-MQTT Test Plan

Hardware required:
- ESP32-C3-DevKitM-1
- Sipeed Tang Primer 25K Dock
- 7 jumper wires (male-to-female)
- Micro-USB cable (ESP32-C3)
- USB-C cable (Tang Primer 25K)
- Mac with MQTT broker (mosquitto)

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

| ESP32-C3 GPIO | Signal | PMOD j4 Pin |
|---------------|--------|-------------|
| GPIO2         | TCK    | j4:0        |
| GPIO3         | TMS    | j4:3        |
| GPIO4         | TDI    | j4:1        |
| GPIO5         | TDO    | j4:2        |
| GPIO6         | TX     | serial_rx   |
| GPIO7         | RX     | serial_tx   |
| GND           | GND    | GND         |

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

Start bridge and GDB:
```bash
python3 host/gdb_mqtt_bridge.py --broker <broker-ip> --device <device-id>
riscv32-esp-elf-gdb -ex "target remote localhost:3333"
```

- [ ] `monitor target jtag` switches to JTAG backend
- [ ] No JTAG errors in ESP32 serial log
- [ ] `info registers` returns VexRiscv register values (not ESP32 registers)
- [ ] `x/4x 0x00000000` reads ROM region
- [ ] `x/4x 0x10000000` reads SRAM region

### 3.4 Halt and Resume

- [ ] Ctrl-C halts the VexRiscv (ESP32 log confirms halt)
- [ ] `info registers` works while halted
- [ ] `continue` resumes the VexRiscv
- [ ] LEDs resume chasing after continue

### 3.5 Load Test Binaries

- [ ] Upload blink0.bin to ramfs via file manager
- [ ] `monitor riscv_load blink0.bin` — LED 0 slow blinks
- [ ] Upload blink1.bin, `monitor riscv_load blink1.bin` — LED 1 double blinks
- [ ] Upload hello.bin, `monitor riscv_load hello.bin` — UART output + LED cycle

### 3.6 UART Console Relay

- [ ] `mosquitto_sub -t "device/<id>/console/out"` shows LiteX BIOS output
- [ ] `mosquitto_pub -t "device/<id>/console/in" -m "help"` sends keystroke to BIOS
- [ ] BIOS responds with help text on console/out
- [ ] After loading hello.bin, UART output appears on console/out

### 3.7 Memory Read/Write

While halted:
- [ ] `x/4x 0xf0001800` reads LED CSR register
- [ ] `set *0x10000000 = 0xdeadbeef` writes SRAM
- [ ] `x/x 0x10000000` confirms the write
- [ ] `set *0xf0001800 = 0x3` sets both LEDs on via CSR

### 3.8 Breakpoints

- [ ] `break *0x10000000` sets hardware breakpoint at SRAM base
- [ ] Load blink0.bin, execution halts at 0x10000000
- [ ] `info breakpoints` shows the breakpoint
- [ ] `delete 1` clears it
- [ ] `continue` resumes execution

### 3.9 Backend Switching

- [ ] `monitor target local` switches back to local ESP32 debug
- [ ] `info threads` shows FreeRTOS tasks again
- [ ] `monitor target jtag` switches back to FPGA
- [ ] JTAG operations still work after round-trip

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

### 4.4 macOS CDC

- [ ] ESP32-C3 appears as `/dev/cu.usbmodem*` after plugging in
- [ ] PIO can flash without manual driver intervention
- [ ] Serial monitor works alongside GDB (different USB endpoints)
