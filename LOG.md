# Project log (public)

This is the working log of the qca9377-macos project, sanitized of any
machine-specific identifiers. It exists to show the methodology: every
decision recorded, every check shown, nothing asserted from memory when a
pinned source could be consulted.

## Method rules the project runs under

1. Log everything — decisions, commands, outputs.
2. Check after every change; a change without a recorded check is not done.
3. Hardware is read-only while the Linux driver owns it; ground truth is
   captured by observation only (lspci/sysfs/dmesg/ethtool/iw).
4. No artifact without a hash — sources and firmware are sha256-pinned.
5. License hygiene — ath10k is ISC (adaptable); GPL sources are studied
   but never copied.
6. Expectation honesty — this is a weeks-to-months project; sessions are
   scoped modestly.

## Session 1 — scaffold

- Project repo created with `ref/ firmware/ skeleton/ docs/ logs/`.
- Rules above written down and agreed before any work.

## Session 2 — ground truth, pins, M1 skeleton

- Tool inventory; one privileged, audited, strictly read-only dump
  captured the full hardware picture (dmesg probe trace, PCI
  capabilities, MSI, wireless caps). See `docs/REFERENCE-TRACE.md`.
- Firmware gap found and fixed from evidence: the probe trace shows
  **api 6** firmware and **board api 2** — so the bundle needed
  `firmware-6.bin` + `board-2.bin`, not just `firmware-5.bin`. The
  firmware's embedded version string was verified byte-identical to the
  trace line.
- References pinned: linux-7.2.3 tarball (sha256 in `ref/`), extracted
  ath10k sources verified 64/64 files ISC; itlwm pinned for study only
  (GPLv2 — nothing copied).
- Register constants for the skeleton extracted from the pinned
  `qca6174_regs` table (QCA9377 reuses it per `core.c`) with file:line
  citations in the driver header, then **machine-verified** by a checker
  that re-parses both sides. The checker itself initially grabbed the
  wrong register table (qca988x) and failed loudly — exactly what
  cross-checks are for.
- Wake timeout corrected to 30 ms (`pci.h:201`) after initially assuming
  20 ms.
- M1 skeleton written: match `pci168c,42`, map BAR0, ath10k wake
  protocol, read chip_id / fw_indicator / PCIE_BAR_REG / CE0 word.
  Read-only except the single `PCIE_SOC_WAKE` write — the same write
  ath10k performs before any hardware init.
- A log-hygiene incident: sequential edits to the private log scrambled
  its section order; fixed by full rewrite. Lesson: append-heavy
  documents get rewrites, not repeated patches.

## Session 3 — build pipeline prep

- The "free CI" plan was evaluated claim-by-claim against the actual
  development machine rather than trusted: GH Actions macOS runners
  adopted; VFIO passthrough rejected for this host (no IOMMU groups, and
  the host network rides the card under test); Xcode-in-VM rejected on
  RAM/disk. Honest caveat recorded: a VM without passthrough cannot run
  the probe (no `pci168c,42` present), so the M1 verdict requires the
  real machine booted into macOS.
- License narrative corrected from an external plan: this port is
  ISC-derived, not GPL-obligated. Publishing is a choice — and keeping
  GPL code out is what preserves that choice.
- xcodegen verified to have no kernel-extension product type (checked in
  the PBXProductType enum of the xcodeproj library it uses); the spec
  uses bundle + `WRAPPER_EXTENSION=kext` + `-fapple-kext` + `-lkmod
  -Wl,-kext`.
- CI workflow fixed before first run: an `|| true` was swallowing build
  failures; `-target` replaced `-scheme`; contradictory signing settings
  removed.
- Publishing gate defined: machine identifiers are hard-excluded, a
  privacy grep must pass with zero hits before anything leaves the
  machine, and the staging tree carries no git history.

## Next

- Boot test of v0.5.2: the recovery boot log's SUMMARY line should read
  `m3=BOOTED m4=WMI_ONLINE` with the target's MAC address and RF chain
  count — hardware confirmation that the firmware speaks.
- M4 (HTT data path) planning: service connection for the data pipe,
  HTT sync, ring setup above the existing CE/HTC layers.

## Sessions 4-8 (2026-09-18) — published, first boot test, kmod_info fix

- Repo published, CI taken green through four documented iterations
  (runner/SDK mismatch, userspace libc++ linkage, -rpath rejection,
  SYMROOT product placement). Kext artifact verified end-to-end:
  plutil OK, `Mach-O 64-bit x86_64 kext bundle`, fully resolved.
- First real-hardware boot test: Recovery booted, but the kext never
  loaded. OpenCore's log held the answer (the only DEBUG_WARN line that
  survives a RELEASE build): `Prelinked injection QCA9377.kext -
  Invalid Parameter`.
- Root cause (verified against OpenCore 1.0.7 source): the binary has
  no `_kmod_info` symbol. `-nostdlib` only pulls archive members to
  resolve undefined symbols, and the skeleton references nothing from
  libkmod — so libkmod's kmod_info definition was never linked. OC's
  prelinker finds `VirtualKmod == 0` and rejects the kext. Reference
  kexts built the standard way (identical Mach-O shape) all carry
  `_kmod_info` and inject fine.
- Fix: `-Wl,-u,_kmod_info` forces the libkmod member in; the CI sanity
  step now fails the build if `_kmod_info` is absent.
- Operational lesson: capture tooling that writes to the recovery RAM
  disk must be delivered to external storage before rebooting, or the
  data is lost.

## Sessions 9-17 (2026-09-19/20) — the kernelcache oracle, v0.5.x arc

- **Firmware IE parsing caught a silent bug**: the first parse of the
  firmware container "showed" no image inside `firmware-6.bin`. The parse
  had died silently on an odd-length OTP IE (missing 4-byte alignment).
  Fixed parser proves both firmware files complete; the target's
  `WMI_OP_VERSION = 4` also identified the firmware as **WMI-TLV** —
  all M4 protocol work pulled from `wmi-tlv.c`, not the main WMI.
- **Boot rejection #2, root-caused from ground truth**: v0.5.0 was
  rejected at prelink (`Invalid Parameter`) despite a structurally
  identical plist. Instead of guessing, the actual recovery
  `BaseSystemKernelExtensions.kc` was pulled from the injected DMG and
  used as the export oracle: `_IOMallocContiguous`/`_IOFreeContiguous`
  are **absent** from the recovery kernelcache, and one own-class method
  (`CECopyPair::teardown`) was declared+called but never defined. Either
  alone poisons the link (unresolved → LOAD_ERROR → surfaced as
  Invalid Parameter).
- **Fix (v0.5.1)**: DMA allocation moved to
  `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` +
  `IODMACommand::withSpecification(kIODMACommandOutputHost64, 32 bits,
  …)` — the exact symbols the KC exports (cross-checked against xnu
  headers and a reference open-source driver's usage). The 32-bit mask
  is now enforced at allocation, not checked after the fact.
  `teardown()` defined: parks ring registers before freeing DMA memory.
- **Permanent payoff — the pre-boot gate**: a 76k-symbol oracle extracted
  from the real KC plus a checker script. Validated both directions:
  v0.5.0 fails with exactly the three killer symbols; v0.5.1 passes.
  New rule: no boot test until the gate passes (see `docs/HARDENING.md`).
- **Full audit against the pinned ath10k sources (v0.5.2)**: two
  boot-blockers found and fixed — the CE `recvWait` consumer never
  recovered from the nbytes==0 hardware race (every later exchange read
  the wrong slot; now polls the descriptor to landing like
  `completed_recv_next`), and the HTC WMI service ID was 0x0400 instead
  of 0x0100 (`ATH10K_HTC_SVC_GRP_WMI = 1`) — M4 could never have
  connected. Plus: post-recv-before-send ordering, 244-byte BMI chunk
  cap (256−12 header), credit-report records parsed as 4-byte entries,
  IE-loop pad-underflow guards, `hi_hci_uart_pwr_mgmt_params_ext` write
  (required for this chip family per `core.c`), version-label sync.
- CI taken green after three documented fix cycles; artifact battery
  (plist, Mach-O type, NOUNDEFS, `_kmod_info`, KC import gate) runs on
  every pulled artifact before deployment.
