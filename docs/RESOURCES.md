# Primary sources & verified facts — qca9377-macos

Everything here was verified against the primary source during sessions 26–27.
Cite the pin, don't re-derive from memory.

## Kernel-level declarations (current xnu, apple-oss-distributions mirror)

| Fact | Source | Consequence |
|---|---|---|
| `gIODTPlane` is NOT declared in any SDK header anymore | xnu `iokit/IOKit/IORegistryEntry.h` (no mention), `IOService.h:122-123` externs only `gIOServicePlane`/`gIOPowerPlane` | Driver self-declares `extern const IORegistryPlane * gIODTPlane;` (historical xnu form). Kernel still exports `_gIODTPlane` — verified in Boot KC symtab |
| `_PE_parse_boot_argn` signature | xnu `pexpert/pexpert/pexpert.h:351`, body inside `__BEGIN_DECLS` (line 42) = C linkage | Self-declared `extern "C" int PE_parse_boot_argn(const char*, void*, int)`. `boolean_t` == `int` on mach → identical ABI |
| Kernel builds use `-nostdinc++` | CI failure 35818803291: `<cstring>` breaks libc++ header search | Use `<stdarg.h>`, `bcopy`/`bcmp` (libkern), never `<c*>` headers in kext TUs |
| `fromPath` mangled symbol | Boot KC: `__ZN15IORegistryEntry8fromPathEPKcPK15IORegistryPlanePcPiPS_` | Take/return conventions already in stub |
| Which KC fileset defines what | `ref/kc/check-deps.py` (`kc_filesets()` importable) | `_PE_parse_boot_argn` = `com.apple.kernel` fileset; `_gIODTPlane` = kpi pool. Battery MUST pass before every deploy |

## board-2.bin — structure and our card's entry (session 27, byte-verified)

File: `ref` = embedded copy in `skeleton/FwData.h` (extracted to /tmp during analysis),
upstream `firmware/QCA9377-hw1.0/board-2.bin` (304,308 B, kvalo QCA9377/hw1.0).

- Container: magic `"QCA-ATH10K-BOARD\0"` = **17 bytes incl. NUL**, padded to 20 — then
  outer IEs (id=0 `ATH10K_BD_IE_BOARD`, len) whose payload is a nested IE list:
  id 0 = board **name** (ASCII), id 1 = board **data** (BDF).
- Parser ground truth: pinned ath10k `core.c:1338` (`parse_bd_ie_board`),
  `core.c:1428` (`search_bd`), `core.c:1508` (`magic_len = strlen(ATH10K_BOARD_MAGIC) + 1`),
  `core.c:1574-1600` (`create_board_name`), type enums `hw.h:217-223`.
- Our embedded copy contains **37 board entries, all `vendor=168c,device=0042`**.
- **Our exact card entry EXISTS: `bus=pci,vendor=168c,device=0042,subsystem-vendor=11ad,subsystem-device=08a6`**
  (Lite-On, subsystem read from real Recovery boot evidence). Other 11ad entries: 0806, 1806, 0507.
- Conclusion: M3 `selectBoard()` exact-match path will succeed for this machine; board.bin
  fallback stays as safety net only.

## WMI-TLV constants for M5 (scan/vdev) — from pinned `ref/linux-ath10k/ath10k/wmi-tlv.h`

- ID composition: `WMI_TLV_CMD(grp) = (grp << 12) | 0x1`, same for `WMI_TLV_EV`
  (wmi-tlv.h:14-15). Groups start at `WMI_TLV_GRP_START = 0x3` (wmi-tlv.h:20-21).
- Groups: SCAN=0x3, PDEV=0x4, VDEV=0x5, PEER=0x6, MGMT=0x7 (enum order, wmi-tlv.h:21-27).
- Cmd IDs: `START_SCAN = 0x3001`, `STOP_SCAN = 0x3002`, `SCAN_CHAN_LIST = 0x3003`;
  `PDEV_SET_PARAM = 0x4003`, `PDEV_SET_BASE_MACADDR = 0x4013`;
  `VDEV_CREATE = 0x5001`, `VDEV_DELETE = 0x5002`, `VDEV_START_REQUEST = 0x5003`,
  `VDEV_UP = 0x5005`, `VDEV_DOWN = 0x5007`, `VDEV_SET_PARAM = 0x5008`.
- Events: `SCAN_EVENTID = 0x3001` (wmi-tlv.h:300), `VDEV_START_RESP_EVENTID = 0x5001`.
- Scan frame layout (wmi-tlv.c:1978 `gen_start_scan`): TLV header is 4B (u16 tag, u16 len).
  Frame = `TAG_STRUCT_START_SCAN_CMD`(fixed struct) + `TAG_ARRAY_UINT32`(channels) +
  `TAG_ARRAY_STRUCT`(ssids) + `TAG_ARRAY_STRUCT`(bssids) + `TAG_ARRAY_FIXED_STRUCT`(ie).
  Tag values: `ARRAY_UINT32=589`, `ARRAY_STRUCT=591`, `ARRAY_FIXED_STRUCT=592`,
  `STRUCT_START_SCAN_CMD=640` (wmi-tlv.h:589-640).
- M4 INIT generator constants (44-slot resource_config etc.) are already checker-verified
  (`tools/check-init-constants.py` flow documented in HARDENING.md).

## Logging / telemetry channels (all proven or coded)

| Channel | Path | Survives |
|---|---|---|
| ioreg props `qca-*` | our node (`com_bswork_QCA9377`) | until node goes away; diag NVRAM-reports them |
| NVRAM stage | `/options` → `bswork-qca-stage` | any hang, crash, no-diag boot — read from Linux efivars |
| NVRAM log tail | `/options` → `bswork-qca-logtail` (v0.7.2, 1536B scrolling) | last ~20 log lines, flushed every line |
| dmesg | `msgbuf=1048576` boot-arg (in stick config) | ~1MB of early boot, no eviction at t+6min |
| diag report | `kits/diag/qca-diag.sh` → chunked b64 NVRAM vars → `tools/read-nvram-report.sh` | everything the script captures |

## Recovery OS facts (observed on this machine, sessions 24-26)

- Recovery cannot mount external FAT volumes (msdosfs kext present but mounts fail);
  all evidence egress must be NVRAM (`/options` writes or diag chunked vars).
- `/Volumes` shows only Recovery's own volume (`Untitled`).
- NVRAM round-trip works: GUID `4D424153-542D-4449-4147-000000000001`, prefix `bswork`.
- `b64encode` + `nvram` exist in Recovery rootfs; `gzip`/`base64` do not (diag uses pure-perl b64).
- OC DEBUG 1.0.7 on stick: Target=67, DisplayLevel=0x80000042, logs land as
  `opencore-*.txt` on the EFI FAT volume.

## Pipeline invariants (do not break)

1. Injector golden DMG goes via `--golden` (positional = kit mode → BTree abort).
2. `CFBundleExecutable` must exist and equal the binary name (the four-boot lesson).
3. OSBundleLibraries symbols MUST resolve from declared filesets — battery gate.
4. Software-only unmount (`udisksctl unmount -b /dev/sdX3`), never power-off.
5. Public repo = subtree of private `skeleton/` + shared `tools/`/`project.yml`; sync = file copy + commit (footer-free).
