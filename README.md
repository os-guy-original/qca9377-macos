# qca9377-macos

A macOS driver for the Qualcomm Atheros **QCA9377** Wi-Fi adapter
(PCI `168c:0042`), built by adapting the Linux `ath10k` driver to Apple's
IOKit/XNU kernel.

**Status: v0.5.3 — the full boot chain through M4 is implemented (probe,
Copy Engines + BMI, firmware load/boot, HTC + WMI-TLV). v0.5.3 fixes the
root cause of every prior boot rejection (class symbols were hidden by
`-fvisibility=hidden`; OpenCore's prelinker can only bind exported
symbols). The next boot test shows whether the firmware answers
(`WMI_READY_EVENT`); no Wi-Fi yet.** See
the [roadmap](#roadmap) for what works and what is still ahead.

## Why this is possible

- **ath10k is ISC-licensed** (verified per-file in the pinned sources) — a
  permissive license that legally allows adaptation into a macOS kext with
  attribution. Most Linux Wi-Fi drivers are GPL and are not adaptable this
  way; ath10k is the rare exception, and this project exists because of it.
- The **hardware model matches**: QCA9377 is a PCIe device whose firmware
  is uploaded from the host on every boot (BMI), then driven over a
  command/event protocol (WMI) and a ring-based DMA data path (HTT/Copy
  Engines). The ath10k sources contain the complete, exact truth for all
  of it.
- The **802.11 MAC layer** — the hardest part of any Wi-Fi driver — is not
  in scope to invent; a later milestone reuses the proven
  userspace/hybrid approach popularized by itlwm (studied as reference
  only; no GPL code is copied into this project).

## Build

CI builds the kext on every push (GitHub Actions macOS runner, free for
public repos). Grab the `QCA9377-kext` artifact from the latest green run,
or the rolling `kext-latest` release asset (public download, no auth).

Local build on any Mac with Xcode:

```
brew install xcodegen
xcodegen generate
xcodebuild -project QCA9377.xcodeproj -target QCA9377 -configuration Release build
```

The result is a **probe driver only**: it matches `pci168c,42`, maps BAR0,
performs the ath10k wake protocol, and logs diagnostic registers. It
touches nothing else. The single pass/fail line from its first run:

```
QCA9377: SOC chip_id = 0x003821ff
```

That one match proves BAR0 mapping, the wake protocol, and the whole
register-map translation in one shot.

## Firmware

`firmware/QCA9377-hw1.0/` contains the QCA firmware images the card loads
into its own RAM on every boot, with their license notices
(`notice_ath10k_firmware-*.txt`). The firmware is redistributed under the
terms in those notices. `firmware/FETCH.md` describes the upstream source
and how to fetch it yourself.

## Roadmap

| Milestone | Scope | Status |
|---|---|---|
| M1 | Ground truth, pinned references, probe skeleton, CI | **done** |
| M2 | Copy Engine rings + BMI handshake (port of `ce.c` / `pci.c` diag window) | **done** |
| M3 | Firmware load; `WMI_READY_EVENT` observed | **implemented — boot test pending** |
| M4 | HTT data path | — |
| M5 | 802.11 MAC (scan/assoc), networks visible | — |

The `docs/REFERENCE-TRACE.md` file is the Linux `ath10k` probe trace this
hardware produces — the acceptance target every milestone is judged
against.

## License

- Driver code: **ISC** (see `LICENSE`), derived from Linux `ath10k`
  (SPDX ISC, © Atheros Communications / Qualcomm Atheros) with attribution.
- `ref/` fetches the pinned ath10k sources from kernel.org; nothing from
  GPL projects (e.g. itlwm) is copied into this codebase.
- Firmware files are Qualcomm's, redistributed with their notices.
