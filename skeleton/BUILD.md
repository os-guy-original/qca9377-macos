# BUILD.md - M1 skeleton kext

## What this is

`QCA9377Probe` personality matching `pci168c,42`. On `start()` it:

1. logs the PCI identity (vendor/device/rev/subsystem) from config space,
2. maps BAR0 and logs its address/length (ground truth: `0x51000000`, 2 MiB),
3. performs the ath10k wake protocol (write `PCIE_SOC_WAKE`, poll RTC state
   until `V==3`, 5->50us steps, 30 ms timeout per `pci.h:201`),
4. reads and logs four diagnostic registers:
   - SOC chip_id (`BAR0+0x8f0`) - expect `0x003821ff` per Linux dmesg
   - fw_indicator (`BAR0+0x3a028`)
   - PCIE_BAR_REG (`BAR0+0x40030`)
   - CE0 base control word (`BAR0+0x34400`)

It then stays passive. No MSI, no copy engines, no BMI, no firmware load,
no network stack attachment. **M2 is where writing starts; M1 is a probe.**

## Safety rules (non-negotiable)

- **Never load this on the Linux side** - it is macOS-only; on Linux the
  running ath10k owns the card.
- **First load on the target Mac must be in verbose mode** (`-v` boot-arg)
  with the panic screenshot routine known (recovery boot + `nvram
  boot-args=""`). If the kext panics at BAR0 map or wake, the panic log
  (`/Library/Logs/DiagnosticReports/Kernel-*.panic`) names the faulting PC.
- The kext only ever *writes one register*: `PCIE_SOC_WAKE` (offset
  `0x80004`). This write is exactly what ath10k itself performs before
  any hardware init (ref `linux-ath10k/ath10k/pci.c` wake path), and it is
  the documented wake mechanism for this chip family. Everything else is
  read-only.

## Building (needs a macOS machine with Xcode)

This repo is authored on Linux; kexts must be compiled with Apple's
toolchain. Two paths:

### Path A - full Xcode (recommended)

1. Copy `skeleton/` to the Mac.
2. `File > New > Project > macOS > Kernel Extension` (or use
   `kextlib`-style template). Name bundle id `com.bswork.QCA9377`,
   class name `com_bswork_QCA9377`.
3. Replace the generated `QCA9377Driver.hpp/.cpp` and `Info.plist` with
   these files.
4. Deployment target macOS 14.0 (Sonoma) - matches the
   `com.apple.kpi.*: 23.0` versions in Info.plist.
5. Build. The product is `QCA9377.kext`.

### Path B - command line only

```
mkdir -p QCA9377.kext/Contents/MacOS
cp Info.plist QCA9377.kext/Contents/
clang++ -arch x86_64 -arch arm64e \
  -stdlib=libc++ -fmodules -fcaret-diagnostics \
  -DKEXT_BUILD \
  -I/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/Kernel.framework/Headers \
  -fapple-kext -mkernel -nostdlib -lkmod \
  -c QCA9377Driver.cpp -o QCA9377.kext/Contents/MacOS/QCA9377
```

(The exact link step varies by Xcode version - if it fights you, Path A
always works. Do not guess link flags; let a template generate them.)

## Expected first-load log (kprintf / `log show --predicate`)

```
QCA9377: init
QCA9377: start - vendor=0x168c device=0x0042 rev=0x31 sub-vendor=0x11ad sub-device=0x08a6
QCA9377: BAR0 = 0x51000000, 2097152 bytes
QCA9377: target awake
QCA9377: SOC chip_id = 0x003821ff (Linux dmesg: 0x003821ff)
QCA9377: fw_indicator = 0x...
QCA9377: PCIE_BAR_REG = 0x...
QCA9377: CE0 base reg = 0x...
QCA9377: M1 probe complete - staying passive (no MSI, no CE, no fw load)
```

**The pass/fail line is chip_id == 0x003821ff.** That single match proves:
BAR0 mapped correctly, the wake protocol worked, MMIO is functional, and
the register map translation from the pinned ath10k source is correct.
Every later milestone builds on that one fact.

## Open questions carried into M2

- The ath10k dmesg reports target `0x05020001` while `hw.h` defines
  `QCA9377_HW_1_0_DEV_VERSION 0x05020000` - the extra `|1` bit is set
  somewhere in the PCIe local regs at boot. pci.c reads
  `PCIE_BAR_REG_ADDRESS` (0x40030) around this; resolve with M2 readouts.
- `firmware-6.bin` container has an unexplained `0x77` byte at offset 11;
  resolve by reading the pinned ath10k firmware parser (`firmware.c`)
  before ever writing a loader.
