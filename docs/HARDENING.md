# Hardening & DR — qca9377-macos

Rules and procedures that keep boot tests cheap and mistakes reversible.
Every item here was earned the hard way (see WORKLOG sessions 14–24).

## Root cause #2 — undeclared IOPCIFamily (2026-09-21, session 24)

**The dependency bug.** OC's prelinker resolves undefined symbols from the
kext's own externals plus the KC filesets matching its DECLARED
OSBundleLibraries only (Link.c `InternalOcGetSymbolName` →
`Kext->Dependencies[]`). We referenced `IOPCIDevice` without declaring
`com.apple.iokit.IOPCIFamily`, so `metaClass` /
`extendedConfigRead8/16` silently stayed value-0 → LOAD_ERROR →
"Invalid Parameter". Named in the first DEBUG-OC boot log:
`OCAK: Symbol __ZN11IOPCIDevice9metaClassE has 0-value`.

**Fix:** declare `com.apple.iokit.IOPCIFamily: 2.9` (v0.5.4). Matches the
working reference (VoodooI2C declares exactly this).

**Gate v3** (`bswork/ref/kc/check-deps.py`): parses every boot-KC fileset
(LC_FILESET_ENTRY name is a lc_str at +24; inner LC_SYMTAB offsets are
FILE-ABSOLUTE in KC layout — both learned by byte-level debugging) and
requires each undefined symbol to be own-external / `__realmain`-glue /
`com.apple.kernel`-exported / declared-family-exported. Matrix:
v0.5.3+old-plist FAILs on exactly the 3 log-named symbols;
v0.5.3+new-plist resolves 315/315.

**Rules:**
1. Any new class/function import → run gate v3 BEFORE push. It prints the
   exact `add to OSBundleLibraries:` line.
2. DEBUG OC stays the boot arbiter: `OCAK: Symbol ... has 0-value` lines
   enumerate unsolved symbols with zero guessing.
3. `com.apple.kpi.*` versions are fine as-is; only *family* bundles need
   explicit declaration when referenced.

## Root cause #1 — the visibility bug (2026-09-21, session 23)

**The visibility bug.** `-fvisibility=hidden` in OTHER_CFLAGS demoted every
class symbol (`metaClass`, `gMetaClass`, `superClass`, both vtables) to
LOCAL linkage while cross-TU references stayed external-undefined
(N_PEXT|UNDEF). Only ld64's auto-kept kmod glue was exported (5 symbols
total, vs VoodooPS2Controller's 169). OpenCore's prelinker links injected
kexts against **external definitions only**, so binding `metaClass` failed
→ `InternalPrelinkKext` returned LOAD_ERROR → remapped by the caller to
`EFI_INVALID_PARAMETER` → the one-line log
`OC: Prelinked injection QCA9377.kext (...) - Invalid Parameter`
on **every version we ever injected** (v0.1.0 through v0.5.2 — there was
never a working baseline to bisect from).

Why it hid for so long:
- RELEASE OpenCore collapses all linker failures into one line; the named
  diagnostics (`OCAK: Symbol %a was unresolved`) are DEBUG_INFO prints.
- The original import gate matched symbol *strings* in the KC, not
  exportability, and never checked that our own symbols were exported.
- `KextFindKmodAddress` passes with a LOCAL `kmod_info` (only its address
  is read), so the earliest in-binary probe missed it.

Fix (v0.5.3): `-fvisibility=hidden` removed from OTHER_CFLAGS; explicit
`GCC_SYMBOLS_PRIVATE_EXTERN: NO` + `GCC_INLINES_ARE_PRIVATE_EXTERN: NO`
(the Xcode kext template's own values).

## The gate v2 — run before EVERY boot test

```
python3 /home/sd-v/bswork/ref/kc/check-imports.py <kext-binary>
```

Exit 0 = safe to boot-test. It now performs four checks:

1. **Export census** — fails if any symbol is referenced
   external-undefined while a same-named LOCAL definition exists in the
   binary (the exact v0.5.2 pathology; mangling-agnostic, so it also
   passes namespaced kexts like VoodooPS2). `__realmain`/`__antimain`
   are allowlisted (ld64 -kext carries them private even in known-good
   kexts; OC tolerates them).
2. **True link pool** — imports must resolve against defined exports of
   `BootKernelExtensions.kc` (kernel + KPIs; `com.apple.kernel` exports
   memcpy/operator-new/IODMACommand/IOMallocContiguous) UNION
   `BaseSystemKernelExtensions.kc` (aux KC, 118 embedded kexts). The old
   aux-only oracle produced false "unresolvable" verdicts for standard
   KPI symbols.
3. **Linker-provided** symbols (`__realmain`, `__antimain`) allowed.
4. **Self-provided** — an UNDEF is fine if the binary itself exports it.

Validation matrix (2026-09-21): v0.5.2 FAIL census (5 shadowed) ✓,
v0.1.0 FAIL census (5 shadowed) ✓, VoodooPS2Controller PASS ✓.

## Structural/identity battery (still mandatory)

```
python3 -c "import plistlib; plistlib.load(open('<kext>/Contents/Info.plist','rb'))"
grep -a CFBundleVersion <kext>/Contents/Info.plist   # must match intended version
grep -a CFBundleIdentifier ...                       # gate uses it for census
```

**Rule: no boot test until the gate passes.** v0.5.0 burned two boot
cycles the gate would have caught; v0.1.0–v0.5.2 burned seven more on the
visibility bug that gate v2 catches instantly.

## Adding a new kernel import — mandatory drill

1. Get the exact mangled symbol from the built binary (undef list).
2. `grep -xF '<symbol>' /home/sd-v/bswork/ref/kc/kc-pool-boot-plus-aux.txt`
   — must be present (pool regenerated from BOTH Recovery KCs; script
   `ref/kc/check-imports.py` re-derives it from the KCs directly).
3. If absent: do not import it (refactor), regardless of what any
   string-scan claims.

## CI expectations

- Build must be warning-free from our sources (cosmetic fixes get same-day
  commits — warnings hide real regressions).
- `gh run watch` before pulling the artifact; artifact battery on every pull.
- **Post-build symbol sanity**: the built binary must export its class
  symbols (gate v2 census on the artifact — catches any future build-flag
  regression immediately).
- Commits and both logs stay footer-free (no co-author lines) — user rule.

## Known limitations (accepted, documented)

- 32-bit DMA mask is enforced at allocation time via
  `inTaskWithPhysicalMask(…, 0xFFFFFFFF)`; kext does not fall back above
  4 GB (QCA9377 cannot DMA above it anyway).
- M4 verdict = WMI READY with MAC + RF chain count. Data path (M5) not started.
- `IOPCIDevice` deprecation warnings ×3 — accepted: PCIDriverKit does not
  apply to classic kexts; revisit only if DriverKit becomes viable here.

## Deferred hygiene — resolved in v0.5.3

- [x] `HTC.cpp` dead `uint8_t raw[2048]` in `waitTarget()` — removed
- [x] `HTC.cpp` unused `static inline wr32()` — removed
- [x] version bump 0.5.2 → 0.5.3 in the same change set (no drift)
