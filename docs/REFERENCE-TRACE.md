# Reference probe trace — QCA9377 under Linux ath10k

This is the exact line sequence the Linux `ath10k` driver produces when it
probes a healthy QCA9377 (`168c:0042`, rev 31) on PCIe. It is the
acceptance target for this project: M2/M3 are judged by reproducing the
equivalent progress with our own driver on macOS.

Source: Linux 7.2.3 ath10k_pci on Acer Aspire hardware, captured read-only
via `dmesg`.

## The trace

```
[    4.122563] ath10k_pci 0000:04:00.0: enabling device (0000 -> 0002)
[    4.123907] ath10k_pci 0000:04:00.0: pci irq msi oper_irq_mode 2 irq_mode 0 reset_mode 0
[    4.322427] ath10k_pci 0000:04:00.0: qca9377 hw1.1 target 0x05020001 chip_id 0x003821ff sub 11ad:08a6
[    4.322431] ath10k_pci 0000:04:00.0: kconfig debug 1 debugfs 1 tracing 1 dfs 0 testmode 0
[    4.322476] ath10k_pci 0000:04:00.0: firmware ver WLAN.TF.2.1-00021-QCARMSWP-1 api 6 features wowlan,ignore-otp crc32 42e41877
[    4.386346] ath10k_pci 0000:04:00.0: board_file api 2 bmi_id N/A crc32 8aedfa4a
[    4.567905] ath10k_pci 0000:04:00.0: htt-ver 3.56 wmi-op 4 htt-op 3 cal otp max-sta 32 raw 0 hwcrypto 1
```

(Offsets are boot-time seconds on the capture machine and carry no meaning
for us.)

## Decoding — what each line proves

| Line | Meaning | Project milestone |
|---|---|---|
| `enabling device` | PCI command register: memory space + bus master enabled | M1 |
| `pci irq msi oper_irq_mode 2` | single MSI vector negotiated | M2 |
| `chip_id 0x003821ff` | read from `BAR0 + RTC_SOC_BASE(0x800) + 0xf0`; **the M1 pass/fail value** | M1 |
| `target 0x05020001` | target firmware version register (note the `|1` bit vs `QCA9377_HW_1_0_DEV_VERSION 0x05020000` — to be explained from `pci.c` reads) | M2 |
| `firmware ver … api 6` | `firmware-6.bin` loaded over BMI; API 6 | M3 |
| `board_file api 2` | `board-2.bin` format | M3 |
| `htt-ver 3.56 wmi-op 4 htt-op 3` | WMI/HTT protocol negotiation complete | M3 |

## v0.2.0 additions — what each new line proves

| Line | Meaning | Source of constants |
|---|---|---|
| `fw_indicator = 0x… [EVENT_PENDING/INITIALIZED]` (or `(0 = cold target)`) | SCRATCH_3 state left by whatever ran the card last; `0` = firmware never started (ideal M1 condition) | `hw.h:984-987` (bits 1/2) |
| `CE<n> srBase=… srNent=… drBase=… drNent=… srW=… drW=… SRRI=… DRRI=…` ×8 | per-CE ring state as the kext sees it; non-zero bases/indexes = a previous OS configured the rings (warm boot) | `ce.h:341` (base), `hw.c:462-476` (qcax_ce_regs offsets) |
| `CE wrapper intr summary = 0x…` | wrapper interrupt summary register (ce.h:374); non-zero = pending CE interrupts at boot | same |
| `SoC revision <n> (hw1.x), pci rev-id 0x…` | chip-id revision field decode (bits 11:8) cross-checked with PCI config rev | `hw.h:70-72,916-917` |
| `SUMMARY ok=1 version=0.2.0 pciRev=…` | single machine-friendly verdict line for capture tooling | — |

## Generic PCI facts of this card family

- BAR0: 2 MiB, 64-bit, non-prefetchable memory
- PCIe x1, 2.5 GT/s; ASPM L0s/L1 capable; L1 enabled on the capture host
- MSI: up to 8 vectors maskable; ath10k uses 1
- Subsystem on the capture card: Lite-On `11ad:08a6` (varies by OEM; not
  part of our match logic — we match `pci168c,42` only)
