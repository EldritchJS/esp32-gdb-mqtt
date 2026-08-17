# FPGA Toolchain Setup for ULX3S ECP5 + ESP32-C3 Remote Debug

This guide covers everything you need to install on macOS before the ULX3S
and ESP32-C3-DevKitM-1 hardware arrives.

## Hardware to Acquire

| Item | Purpose | Source |
|------|---------|--------|
| ULX3S 85F | ECP5 FPGA dev board (84K LUTs, 56 GPIO, JTAG header) | [Crowd Supply](https://www.crowdsupply.com/radiona/ulx3s), [Mouser](https://www.mouser.com/ProductDetail/Radiona/CS-ULX3S-03), eBay |
| ESP32-C3-DevKitM-1 | RISC-V MCU with WiFi (runs GDB-MQTT firmware) | Espressif, Amazon, Mouser |
| Female-female jumper wires | 4 for JTAG (TCK, TMS, TDI, TDO) + 2 for GND/3V3 | Any electronics supplier |

Both boards run at 3.3V logic — no level shifting needed between them.

## Wiring (C3 to ULX3S J4 JTAG Header)

The ULX3S J4 header pinout is: 3V3, GND, TCK, TDI, TDO, TMS.

```
ESP32-C3-DevKitM-1          ULX3S J4 Header
┌──────────────────┐        ┌────────────────┐
│ GPIO 2  (TCK) ───┼───────►│ TCK            │
│ GPIO 3  (TMS) ───┼───────►│ TMS            │
│ GPIO 4  (TDI) ───┼───────►│ TDI            │
│ GPIO 5  (TDO) ◄──┼────────│ TDO            │
│ GND ─────────────┼────────│ GND            │
│ 3V3 ─────────────┼────────│ 3V3 (optional) │
└──────────────────┘        └────────────────┘
```

The same 4 JTAG wires serve dual purpose:
1. Programming the ECP5 bitstream (loading the soft RISC-V into the FPGA)
2. Debugging the soft RISC-V core via the ECP5 JTAGG primitive

Do NOT connect the 3V3 line unless needed — the ULX3S's 3V3 rail is 2A capable
and can damage things if miswired. Both boards have their own USB power.

## 1. ECP5 Open-Source Toolchain (Synthesis + Place & Route + Bitstream)

These tools turn Verilog/VHDL into a bitstream you can load onto the ECP5.

### Recommended: OSS CAD Suite (pre-built bundle from YosysHQ)

The Homebrew taps for ECP5 tools (ktemkin/oss-fpga, kost/homebrew-ulx3s) are
stale — broken python deps and outdated branch names. The reliable method is
YosysHQ's official nightly build which bundles everything pre-compiled.

1. Go to https://github.com/YosysHQ/oss-cad-suite-build/releases
2. Download the latest release for your platform:
   - **Apple Silicon (M1/M2/M3/M4):** `oss-cad-suite-darwin-arm64-YYYYMMDD.tgz`
   - **Intel Mac:** `oss-cad-suite-darwin-x64-YYYYMMDD.tgz` (being phased out)

```bash
# Download (adjust URL for latest nightly and your architecture)
cd ~/fpga
curl -LO https://github.com/YosysHQ/oss-cad-suite-build/releases/download/2026-08-01/oss-cad-suite-darwin-arm64-20260801.tgz

# Extract
tar -xzf oss-cad-suite-darwin-arm64-*.tgz
```

3. Add to your shell profile (~/.zshrc):

```bash
# OSS CAD Suite — ECP5 FPGA toolchain
source ~/fpga/oss-cad-suite/environment
```

Then reload: `source ~/.zshrc`

This gives you all of the following in one shot:
- **yosys** — RTL synthesis (Verilog → netlist)
- **nextpnr-ecp5** — place and route (netlist → physical layout)
- **ecppack** — bitstream packing (layout → .bit file)
- **openFPGALoader** — bitstream upload to FPGA boards
- **openocd** — JTAG debugging (useful for reference/testing)

### Alternative: YoWASP (if you prefer pip)

YoWASP provides the same tools as WebAssembly binaries via pip:

```bash
pip3 install yowasp-yosys yowasp-nextpnr-ecp5
```

Executables are prefixed with `yowasp-` (e.g., `yowasp-yosys`, `yowasp-nextpnr-ecp5`).
Actively maintained — latest release March 2026.

### Verify installation

```bash
yosys --version
nextpnr-ecp5 --version
ecppack --version
```

## 2. FPGA Loader (for testing via onboard FTDI)

openFPGALoader uploads bitstreams to the ULX3S via its onboard FTDI USB chip.
Useful for testing before the C3 takes over programming duties.

Already included in OSS CAD Suite. If you used YoWASP instead, install separately:

```bash
brew install openfpgaloader
```

### Usage

```bash
# Upload bitstream to SRAM (volatile, lost on power cycle)
openFPGALoader --board=ulx3s bitstream.bit

# Write bitstream to SPI flash (persistent)
openFPGALoader --board=ulx3s -f bitstream.bit
```

## 3. LiteX SoC Builder + VexRiscv

LiteX builds the entire SoC — VexRiscv CPU, UART, SDRAM controller, debug
module — and outputs Verilog for yosys. It has a built-in ULX3S target.

```bash
# Create a working directory
mkdir -p ~/fpga && cd ~/fpga

# Download and run the LiteX setup script
curl -o litex_setup.py https://raw.githubusercontent.com/enjoy-digital/litex/master/litex_setup.py
python3 litex_setup.py --init --install

# Download pre-built RISC-V GCC cross-compiler
python3 litex_setup.py --gcc=riscv
```

### Set up your PATH

Add this to your shell profile (~/.zshrc):

```bash
# RISC-V toolchain (adjust path based on where litex_setup.py installed it)
export PATH="$PATH:$HOME/fpga/riscv64-elf-gcc/bin"
export LITEX_ENV_CC_TRIPLE=riscv64-elf
```

Then reload: `source ~/.zshrc`

### Verify installation

```bash
python3 -c "import litex; print('LiteX OK')"
riscv64-elf-gcc --version
```

### Build a test SoC for ULX3S (after hardware arrives)

```bash
cd ~/fpga/litex-boards/litex_boards/targets
python3 radiona_ulx3s.py --build --cpu-type=vexriscv
```

This generates a bitstream with VexRiscv + UART + SDRAM ready to load.

## 4. SpinalHDL (Optional — only if customizing VexRiscv)

LiteX ships pre-built VexRiscv Verilog configurations. You only need SpinalHDL
if you want to customize the CPU pipeline, add/remove ISA extensions, or
modify the debug module.

```bash
brew install openjdk sbt
```

### Verify

```bash
java --version
sbt --version
```

## 5. Simulation (Optional — for testing without hardware)

```bash
brew install verilator
```

LiteX can simulate the entire SoC in software:

```bash
cd ~/fpga
litex_sim --cpu-type=vexriscv
```

This runs VexRiscv in simulation with a virtual UART — useful for testing
soft core firmware before the board arrives.

## 6. ESP32-C3 Toolchain (Already Installed)

You already have PlatformIO and ESP-IDF for the C3 side (the esp32-gdb-mqtt
project). The ESP-IDF toolchain includes `riscv32-esp-elf-gdb` which can also
be used to debug the soft RISC-V core via the MQTT bridge.

## Summary Checklist

```
[ ] Download and extract OSS CAD Suite from GitHub releases
[ ] Add "source ~/fpga/oss-cad-suite/environment" to ~/.zshrc
[ ] Verify: yosys --version && nextpnr-ecp5 --version && ecppack --version
[ ] pip3 install meson ninja
[ ] python3 litex_setup.py --init --install
[ ] python3 litex_setup.py --gcc=riscv
[ ] Add RISC-V toolchain to PATH in ~/.zshrc
[ ] Verify: riscv64-elf-gcc --version
[ ] (Optional) brew install openjdk sbt
[ ] (Optional) brew install verilator
```

## Workflow Once Hardware Arrives

1. **Build SoC bitstream**: LiteX generates VexRiscv + peripherals + debug TAP → yosys → nextpnr → ecppack → .bit file
2. **Test via FTDI**: `openFPGALoader --board=ulx3s bitstream.bit` to verify the SoC works
3. **Wire C3 to J4**: Connect 4 JTAG lines + GND
4. **Flash C3 firmware**: `pio run -t upload` (the esp32-gdb-mqtt project)
5. **Program FPGA from C3**: `monitor fpga_program` via GDB, or auto-program on boot
6. **Debug soft core**: `monitor target jtag` switches the GDB server to JTAG backend, C3 tunnels through JTAGG to VexRiscv debug module

## References

- [ULX3S Manual & Pinout](https://github.com/emard/ulx3s/blob/master/doc/MANUAL.md)
- [ULX3S on Crowd Supply](https://www.crowdsupply.com/radiona/ulx3s)
- [LiteX Installation Wiki](https://github.com/enjoy-digital/litex/wiki/Installation)
- [LiteX on PyPI](https://pypi.org/project/litex/)
- [Linux on LiteX-VexRiscv](https://github.com/litex-hub/linux-on-litex-vexriscv)
- [NEORV32 JTAG Debug Discussion](https://github.com/stnolting/neorv32/discussions/28)
- [VexRiscv OpenOCD and Traps](https://tomverbeure.github.io/2021/07/18/VexRiscv-OpenOCD-and-Traps.html)
- [OSS CAD Suite Releases](https://github.com/YosysHQ/oss-cad-suite-build/releases)
- [YoWASP (pip-based toolchain)](https://yowasp.org/)
- [Project Trellis](https://github.com/YosysHQ/prjtrellis)
