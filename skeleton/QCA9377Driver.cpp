/*
 * QCA9377Driver.cpp - M1 skeleton implementation.
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k (drivers/net/wireless/ath/ath10k),
 * ISC-licensed: Copyright (c) Atheros Communications Inc.,
 * Copyright (c) Qualcomm Atheros, Inc. Adapted per ISC terms with attribution.
 *
 * Derived from Linux v7.2.3 ath10k sources, sha256-pinned:
 * 8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03
 *
 * M1: read-only. The card remains owned by Linux ath10k on the host side
 * of any dual-boot; this kext only maps BAR0 and reads diagnostics.
 * Never load it on a host where the Linux ath10k driver is also bound.
 */

#include "QCA9377Driver.hpp"
#include "FwData.h"
#include <IOKit/IORegistryEntry.h>

// IODT plane for the /options NVRAM mirror. Current SDK headers stopped
// declaring gIODTPlane as an extern global, but IORegistryEntry::getPlane()
// is exported by the KC (verified: __ZN15IORegistryEntry8getPlaneEPKc) and
// returns the same plane object by name — no fragile self-declared extern.

// Boot-arg parser (pexpert): the kernel exports _PE_parse_boot_argn
// (verified in the Boot KC symbol table). Self-declared with the exact xnu
// signature (pexpert.h:351, inside __BEGIN_DECLS = C linkage) instead of
// including <pexpert/pexpert.h>, whose KERNEL-guarded include set varies by
// build config. boolean_t == int on mach, so int is the same ABI type.
extern "C" int PE_parse_boot_argn(const char *arg_string,
                                  void        *arg_ptr,
                                  int          max_arg);

#include <cstdio>
#include <stdarg.h>
#include <libkern/OSDebug.h>
#include <libkern/OSKextLib.h>
#include <IOKit/IOLib.h>
#include <mach/kmod.h>

#define super IOService
OSDefineMetaClassAndStructors(com_bswork_QCA9377, IOService)

bool com_bswork_QCA9377::init(OSDictionary *dictionary)
{
    if (!super::init(dictionary))
        return false;
    qlog("QCA9377: init\n");
    return true;
}

void com_bswork_QCA9377::free(void)
{
    qlog("QCA9377: free\n");
    super::free();
}

bool com_bswork_QCA9377::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    publishStage("M0-starting");

    // Staged bring-up cap (default 4 = run everything). PE_parse_boot_argn
    // returns false when absent, leaving the default untouched.
    if (!PE_parse_boot_argn("qca-maxstage", &fMaxStage, sizeof(fMaxStage)))
        fMaxStage = 4;
    if (fMaxStage < 1 || fMaxStage > 4)
        fMaxStage = 4;
    publishNum("maxstage", fMaxStage);
    if (fMaxStage < 4)
        qlog("QCA9377: stage cap %u - M%u and later will be skipped\n",
              fMaxStage, fMaxStage + 1);

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) {
        qlog("QCA9377: provider is not IOPCIDevice\n");
        publishStage("FAIL-provider");
        return false;
    }
    publishNum("pci-vendor", fPci->configRead16(0x00));
    publishNum("pci-device", fPci->configRead16(0x02));
    publishNum("pci-subvendor", fPci->configRead16(0x2C));

    // Enable PCI memory decodes + bus master BEFORE any BAR or config
    // probe. IOPCIFamily does not guarantee this on injected/prelinked
    // paths (unlike a hotplug probe): config reads can complete while the
    // memory BAR stays decode-disabled, so everything after would
    // consistently read 0xffffffff and the personality would look dead.
    fPci->setMemoryEnable(true);
    fPci->setBusMasterEnable(true);
    // ASPM off before any reset work (ath10k hif_power_up: L1 PM substates
    // on QCA61x4 are a known hang source). Saved for restore in stop().
    // Capability walk by hand — findPCICapability is NOT exported by the
    // Recovery KC (verified); configRead8/16 are proven on hardware.
    if (fPci->configRead16(0x06) & 0x0010) { // status: cap-list present
        uint8_t capOff = (uint8_t)(fPci->configRead8(0x34) & 0xFC);
        for (int hops = 0; capOff && hops < 48; hops++) {
            uint8_t capId = fPci->configRead8(capOff);
            if (capId == 0x10) { // PCI Express capability
                fAspmCapOff = capOff;
                fAspmCtl = fPci->configRead16(capOff + 0x10);
                if (fAspmCtl & 0x03) {
                    fPci->configWrite16(capOff + 0x10,
                                        fAspmCtl & ~(uint16_t)0x0003);
                    qlog("QCA9377: ASPM L0s/L1 disabled (was 0x%04x)\n",
                          fAspmCtl);
                }
                break;
            }
            capOff = (uint8_t)(fPci->configRead8(capOff + 0x01) & 0xFC);
        }
    }
    publishNum("aspm-cap-off", fAspmCapOff);
    publishNum("aspm-ctl", fAspmCtl);

    OSSynchronizeIO();

    qlog("QCA9377: start - vendor=0x%04x device=0x%04x rev=0x%02x sub-vendor=0x%04x sub-device=0x%04x\n",
          fPci->configRead16(0x00),
          fPci->configRead16(0x02),
          fPci->configRead8(0x08),
          fPci->configRead16(0x2C),
          fPci->configRead16(0x2E));
    fPciRev    = fPci->configRead8(0x08);
    fSubVendor = fPci->configRead16(0x2C);
    fSubDevice = fPci->configRead16(0x2E);
    publishNum("pci-rev", fPciRev);
    publishNum("pci-subdevice", fSubDevice);

    fBar0Mem = fPci->getDeviceMemoryWithIndex(0);
    if (!fBar0Mem) {
        qlog("QCA9377: no BAR0 memory descriptor\n");
        publishStage("FAIL-bar0-descriptor");
        return false;
    }
    fBar0Len = fBar0Mem->getLength();
    publishStage("M1-bar0");
    publishNum("bar0-len", (uint32_t)fBar0Len);
    qlog("QCA9377: BAR0 = 0x%llx, %llu bytes\n",
          (uint64_t)fBar0Mem->getPhysicalAddress(), (uint64_t)fBar0Len);

    fBar0 = (volatile uint32_t *)
        fBar0Mem->map()->getVirtualAddress();
    if (!fBar0) {
        qlog("QCA9377: BAR0 map failed\n");
        publishStage("FAIL-bar0-map");
        fBar0Mem = nullptr;
        return false;
    }

    // v0.9.3 NVRAM mirror: set properties on the IODTNVRAM *service* — its
    // setProperty override commits to EFI. (The old /options route died:
    // getPlane("IODT") returned null in Recovery, and registry-node
    // properties never reach efivars anyway.) Route uses only KC-verified
    // symbols: serviceMatching + waitForService (statics), setProperty
    // through the IORregistryEntry virtual chain. The Linux-side proof is
    // bswork-qca-test in efivars on next boot.
    {
        mach_timespec_t ts = { 3, 0 };
        OSDictionary *match = IOService::serviceMatching("IODTNVRAM");
        IOService *svc = match ? IOService::waitForService(match, &ts) : nullptr;
        if (svc) {
            fNvram = svc;
            publishNum("nvram-svc", 1);
            fNvram->setProperty("bswork-qca-test", "kernel-write-ok");
            qlog("QCA9377: IODTNVRAM service found - test var written\n");
        } else {
            publishNum("nvram-svc", 0);
            qlog("QCA9377: IODTNVRAM service NOT found - mirror disabled\n");
        }
    }

    if (!wakeTarget()) {
        qlog("QCA9377: target did not wake (timeout %u us)\n", kWakeTimeout_us);
        publishStage("FAIL-wake");
        fBar0 = nullptr;
        fBar0Mem = nullptr;
        return false;
    }
    qlog("QCA9377: target awake\n");
    publishStage("M1-awake");

    if (!probeRegisters()) {
        qlog("QCA9377: register probe failed\n");
        publishStage("FAIL-registers");
        fBar0 = nullptr;
        fBar0Mem = nullptr;
        return false;
    }
    publishStage("M1-probe");

    probeCopyEngines();
    logRevisionInfo();

    // M2 gate: bring the target to a known state before CE/BMI. After a warm
    // reboot from Linux (ath10k loaded, firmware running), the target is in
    // an undefined state (ath10k hif_power_up comment) — all SOC-domain
    // reads returned 0 in the 0923 boot. ath10k's medicine for exactly this:
    // qca6174_chip_reset = cold reset + wait-init (+ warm reset).
    if (!resetChip()) {
        qlog("QCA9377: chip reset failed - staying loaded for diagnostics\n");
        publishStage("FAIL-reset");
    } else if (!probeBmi()) {
        qlog("QCA9377: M2 BMI probe failed - staying loaded for diagnostics\n");
        publishStage("FAIL-m2-bmi");
    } else {
        publishStage("M2-bmi");
        publishNum("bmi-target", fBmi ? fBmi->targetVersion() : 0);
    }

    // M3: firmware boot (no interrupts yet; poll-only). Stage gate: cap=1
    // stops after M1, cap=2 after M2.
    bool m3ok = false;
    if (fMaxStage < 3) {
        qlog("QCA9377: M3 skipped by stage cap %u\n", fMaxStage);
        publishStage("M3-skip");
    } else if (fBmi && fBmi->targetVersion() != 0) {
        m3ok = bootFirmware();
    } else {
        qlog("QCA9377: M3 skipped - BMI probe did not succeed\n");
        publishStage("M3-skip");
    }

    // M4: HTC handshake + WMI-TLV SERVICE_READY/READY (poll-only). Verdict:
    // target's firmware is alive and talking on CE2/CE3. Gate: cap<=3 skips.
    bool m4ok = false;
    if (fMaxStage < 4) {
        qlog("QCA9377: M4 skipped by stage cap %u\n", fMaxStage);
        publishStage("M4-skip");
    } else if (m3ok) {
        m4ok = startHtcWmi();
    } else {
        qlog("QCA9377: M4 skipped - firmware not booted\n");
        publishStage("M4-skip");
    }

    publishNum("m3-ok", m3ok ? 1 : 0);
    publishNum("m4-ok", m4ok ? 1 : 0);
    publishStage("SUMMARY");
    qlog("QCA9377: SUMMARY ok=1 version=" QCA_DRIVER_VERSION " pciRev=0x%02x bmiTarget=0x%08x m3=%s m4=%s\n",
          fPciRev, fBmi ? fBmi->targetVersion() : 0,
          m3ok ? "BOOTED" : "no",
          m4ok ? "WMI_ONLINE" : "no");

    qlog("QCA9377: probe complete - staying passive (no MSI, no interrupts, no fw load)\n");
    registerService();
    return true;
}

void com_bswork_QCA9377::stop(IOService *provider)
{
    qlog("QCA9377: stop\n");
    teardownHardware();
    if (fNvram) {
        fNvram->release();
        fNvram = nullptr;
    }
    if (fPci) {
        // Release bus-master first so the target cannot issue new DMA while
        // we disarm rings, then drop memory decodes.
        fPci->setBusMasterEnable(false);
        fPci->setMemoryEnable(false);
        if (fAspmCapOff && fAspmCtl & 0x03)
            fPci->configWrite16(fAspmCapOff + 0x10, fAspmCtl);
        OSSynchronizeIO();
    }
    super::stop(provider);
}

// ---- milestone telemetry (v0.6.0/v0.7.0) ----------------------------------
// "qca-*" properties on our own registry node: ioreg keeps them for the
// node's whole life (dmesg rotates), and the diag's NVRAM report captures
// the node verbatim. Every M2+ failure path publishes BEFORE teardown on
// purpose: start() stays true there, so the node survives with the verdict.
//
// v0.9.3: the stage is mirrored into real NVRAM by setting the property on
// the IODTNVRAM service itself (fNvram). If start() panics or the diag never
// runs, Linux still reads "bswork-qca-stage" straight from efivars.
void com_bswork_QCA9377::nvramStage(const char *stage)
{
    if (!fNvram)
        return;
    fNvram->setProperty("bswork-qca-stage", stage);
}

void com_bswork_QCA9377::publishStage(const char *stage)
{
    setProperty("qca-stage", stage);
    nvramStage(stage);
}

void com_bswork_QCA9377::publishNum(const char *key, uint32_t v)
{
    char full[48];
    snprintf(full, sizeof(full), "qca-%s", key);
    setProperty(full, v, 32);
}

// ---- chip reset (v0.8.0) — ported from ath10k pci.c -----------------------
// qca6174_chip_reset (pci.c:2751): "QCA6174 requires cold + warm reset to
// work"; cold reset (pci.c:3341) + wait_for_target_init (pci.c:3284) +
// warm reset (pci.c:2623). The warm tail needs CE pipes (via init_pipes),
// so the probe path runs the cold+wait prefix here and the warm tail is
// re-evaluated after CE init — matching ath10k's own skip when pipes are
// down.
bool com_bswork_QCA9377::resetChip(void)
{
    qlog("QCA9377: chip reset begin\n");
    // ath10k wraps every reg access in wake/sleep; after cold reset the
    // target may auto-sleep, so assert wake before and after the reset.
    if (!wakeTarget()) {
        publishNum("reset-fail", 1);
        return false;
    }
    if (!coldReset()) {
        publishNum("reset-fail", 2);
        return false;
    }
    if (!wakeTarget()) {
        publishNum("reset-fail", 3);
        return false;
    }
    if (!waitForTargetInit()) {
        // v0.9.5 RESCUE: the cold leg revived the SOC but the ROM stayed
        // silent (both boots since v0.8.0). ath10k's documented medicine
        // for exactly this: the warm leg — CPU warm reset forces the ROM
        // to re-run, with CE pipes up around it.
        qlog("QCA9377: cold-reset announce failed - trying warm rescue leg\n");
        publishNum("postreset-chip-id", read32(kSOC_ChipID_Offset));
        publishNum("postreset-bar-reg", read32(kPCIe_BARReg_Offset));
        publishNum("postreset-rtc-state",
                   read32(kPCIe_LocalBaseAddress) & kRTCStateMaskSt);
        if (warmReset()) {
            publishStage("M2-reset");
            return true;
        }
        publishNum("reset-fail", 4);
        return false;
    }
    publishStage("M2-reset");
    return true;
}

bool com_bswork_QCA9377::coldReset(void)
{
    // SOC_GLOBAL_RESET is a reg-domain address: PCIE_LOCAL_BASE + 0x8
    // (ath10k reg_read32, pci.c:701). Read-modify-write bit0.
    uint32_t val = read32(kPCIe_LocalBaseAddress + kSocGlobalResetOffset);
    qlog("QCA9377: cold reset: GLOBAL_RESET=0x%08x\n", val);
    val |= 1;
    write32(kPCIe_LocalBaseAddress + kSocGlobalResetOffset, val);
    // PCIe may not be stable immediately after the reset write (ath10k
    // pci.c:3350 comment) — mandatory delay before further access.
    IOSleep(kColdResetDelay_ms);
    val &= ~(uint32_t)1;
    write32(kPCIe_LocalBaseAddress + kSocGlobalResetOffset, val);
    IOSleep(kColdResetDelay_ms);
    qlog("QCA9377: cold reset complete\n");
    return true;
}

bool com_bswork_QCA9377::waitForTargetInit(void)
{
    // Port of ath10k_pci_wait_for_target_init (pci.c:3284), INTX branch:
    // poll FW_INDICATOR (SOC_CORE_BASE + 0x28) until FW_IND_INITIALIZED.
    // v0.9.4: 10 s window; every second publishes fw-ind-sN + rtc-sN
    // (RTC power state, PCIe-local domain). Diagnoses: ROM announces late
    // (samples show it), ROM dead (all zero), SOC auto-sleep (rtc != 3),
    // decode death (0xffffffff).
    uint32_t elapsed     = 0;
    uint32_t nextSample  = 0;
    uint32_t last        = 0xffffffff;
    while (elapsed < kTargetInitTimeout_ms) {
        uint32_t val = read32(kFWIndicatorAddress);
        last = val;
        if (val != 0xffffffff) {
            if (val & kFWIndInitialized) {
                publishNum("fw-indicator", val);
                qlog("QCA9377: target initialised after %u ms (fw_ind=0x%08x)\n",
                      elapsed, val);
                return true;
            }
            if (val & kFWIndEventPending)
                break; // device crashed during init
        }
        if (elapsed >= nextSample) {
            char key[16];
            snprintf(key, sizeof(key), "fw-ind-s%u", nextSample / 1000);
            publishNum(key, val);
            snprintf(key, sizeof(key), "rtc-s%u", nextSample / 1000);
            publishNum(key, read32(kPCIe_LocalBaseAddress) & kRTCStateMaskSt);
            nextSample += kTargetInitSample_ms;
        }
        IOSleep(kTargetInitStep_ms);
        elapsed += kTargetInitStep_ms;
    }
    // timeout telemetry (0xffffffff=decode-dead, 0=never started,
    // EVENT_PENDING=crashed mid-init)
    publishNum("fw-ind-last", last);
    qlog("QCA9377: target init wait timed out (last fw_ind=0x%08x)\n", last);
    return false;
}

// v0.9.5: init_pipes equivalent — create or recreate the CE pair manager.
// The warm leg resets the CE block mid-sequence, so rings must be
// reprogrammed; ath10k calls init_pipes twice inside warm_reset.
bool com_bswork_QCA9377::ensureCE(void)
{
    if (fCe && fCe->init())
        return true;
    if (fCe) {
        fCe->destroy();
        fCe = nullptr;
    }
    fCe = qca::CEManager::create(fBar0);
    if (!fCe)
        return false;
    if (!fCe->init()) {
        fCe->destroy();
        fCe = nullptr;
        return false;
    }
    return true;
}

// v0.9.5: full port of ath10k_pci_warm_reset (pci.c:2623) — the medicine
// for exactly our observed failure: cold reset revives the SOC (rtc=3,
// chip-id correct) but the ROM never announces. ath10k's comment:
// "QCA6174 requires cold + warm reset to work." Faithful structure:
//   si0 → cpu-warm-reset(FW_IND=0) → init_pipes → wait-init
//   → clear LF timer → CE reset → cpu-warm-reset → init_pipes → wait-init
bool com_bswork_QCA9377::warmReset(void)
{
    uint32_t val;

    // SI0: set/clear SI0_RST (mask 0 on qca6174 — RMW kept for faithfulness)
    val = read32(kRTC_SOC_BaseAddress + kSocResetControlOffset);
    write32(kRTC_SOC_BaseAddress + kSocResetControlOffset, val);
    IOSleep(kWarmResetStep_ms);
    publishStage("warm-si0");

    // CPU warm reset: FW_INDICATOR=0 then CPU_WARM_RST — forces the target
    // CPU back into the mask ROM
    write32(kFWIndicatorAddress, 0);
    val = read32(kRTC_SOC_BaseAddress + kSocResetControlOffset);
    write32(kRTC_SOC_BaseAddress + kSocResetControlOffset,
            val | kSocResetCpuWarmRstMask);
    publishStage("warm-cpu");

    // interlude 1: init_pipes + wait_for_target_init (ath10k does both here)
    if (!ensureCE()) {
        publishNum("warm-fail", 1);
        qlog("QCA9377: warm reset: CE init failed (interlude 1)\n");
        return false;
    }
    (void)waitForTargetInit(); // non-fatal here; the final wait decides
    publishStage("warm-wait1");

    // LF timer disable
    val = read32(kRTC_SOC_BaseAddress + kSocLfTimerControl0Offset);
    write32(kRTC_SOC_BaseAddress + kSocLfTimerControl0Offset,
            val & ~kSocLfTimerEnableMask);
    publishStage("warm-lf");

    // CE reset set/clear
    val = read32(kRTC_SOC_BaseAddress + kSocResetControlOffset);
    write32(kRTC_SOC_BaseAddress + kSocResetControlOffset, val | kSocResetCeRstMask);
    IOSleep(kWarmResetStep_ms);
    write32(kRTC_SOC_BaseAddress + kSocResetControlOffset, val & ~kSocResetCeRstMask);
    publishStage("warm-ce");

    // second CPU warm reset (ath10k warm_reset_cpu #2)
    write32(kFWIndicatorAddress, 0);
    val = read32(kRTC_SOC_BaseAddress + kSocResetControlOffset);
    write32(kRTC_SOC_BaseAddress + kSocResetControlOffset,
            val | kSocResetCpuWarmRstMask);
    publishStage("warm-cpu2");

    // interlude 2: init_pipes + the decisive wait_for_target_init
    if (!ensureCE()) {
        publishNum("warm-fail", 2);
        qlog("QCA9377: warm reset: CE init failed (interlude 2)\n");
        return false;
    }
    if (!waitForTargetInit()) {
        publishNum("warm-fail", 3);
        qlog("QCA9377: warm reset: target never announced after warm leg\n");
        return false;
    }

    qlog("QCA9377: warm reset complete - target announced\n");
    return true;
}

// ath10k wake_target_cpu (pci.c): set CPU_INTR_MASK in CORE_CTRL so the
// target CPU wakes and services the CE rings. SOC_CORE_BASE + offset form.
void com_bswork_QCA9377::wakeTargetCpu(void)
{
    uint32_t val = read32(kSOC_CoreBaseAddress + kSocCoreCtrlOffset);
    write32(kSOC_CoreBaseAddress + kSocCoreCtrlOffset,
            val | kCoreCtrlCpuIntrMask);
    qlog("QCA9377: target CPU doorbell (CORE_CTRL 0x%08x -> 0x%08x)\n",
          val, val | kCoreCtrlCpuIntrMask);
}

// ---- target CE configuration (v0.9.2) — port of ath10k_pci_init_config ----
//
// All values below are byte-verified against pinned ath10k:
//   ce_pipe_config            ce.h (6x u32, packed)
//   pci_target_ce_config_wlan pci.c (CE0..CE9; target receives 7 entries —
//                             qca6174_values.num_target_ce_config_wlan)
//   pci_target_service_to_ce_map_wlan pci.c (service_id = group<<8|idx,
//                             htc.h:261/270)
//   struct pcie_state         pci.h:42
//   HI offsets                targaddrs.h (hi_option_flag2 0xcc,
//                             hi_early_alloc 0x100)
//   banks                     QCA9377_1_0_DEVICE_ID -> 9 (pci.c:2308)
struct CePipeConfig {
    uint32_t pipenum;
    uint32_t pipedir;
    uint32_t nentries;
    uint32_t nbytesMax;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

struct CeSvcToPipe {
    uint32_t serviceId;
    uint32_t pipeDir;
    uint32_t pipeNum;
} __attribute__((packed));

static const uint32_t kPipeDirIn  = 1;
static const uint32_t kPipeDirOut = 2;
static const uint32_t kPipeDirInOut = 3;
static const uint32_t kCeAttrDisIntr = 0x8;

static const uint32_t kSvcWmiControl = 0x100;
static const uint32_t kSvcWmiDataBe  = 0x101;
static const uint32_t kSvcWmiDataBk  = 0x102;
static const uint32_t kSvcWmiDataVi  = 0x103;
static const uint32_t kSvcWmiDataVo  = 0x104;
static const uint32_t kSvcRsvdCtrl   = 0x001;
static const uint32_t kSvcTestRaw    = 0xFE00;
static const uint32_t kSvcHttDataMsg = 0x300;

static const CePipeConfig kTargetCeConfigWlan[] = {
    { 0, kPipeDirOut,  32,  256, 0,                0 }, // CE0 host->t HTC
    { 1, kPipeDirIn,   32, 2048, 0,                0 }, // CE1 t->host HTT+HTC
    { 2, kPipeDirIn,   64, 2048, 0,                0 }, // CE2 t->host WMI
    { 3, kPipeDirOut,  32, 2048, 0,                0 }, // CE3 host->t WMI
    { 4, kPipeDirOut, 256,  256, 0,                0 }, // CE4 host->t HTT
    { 5, kPipeDirIn,   32,  512, 0,                0 }, // CE5 t->host HTT
    { 6, kPipeDirInOut,32, 4096, 0,                0 }, // CE6 autonomous
    { 7, kPipeDirInOut, 0,    0, 0,                0 }, // CE7 host-only diag
    { 8, kPipeDirIn,   64, 2048, kCeAttrDisIntr,   0 }, // CE8 pktlog
    { 9, kPipeDirInOut,32, 2048, kCeAttrDisIntr,   0 }, // CE9 qcache
};
static const uint32_t kNumTargetCeConfig = 7;      // qca6174: send CE0..CE6

static const CeSvcToPipe kTargetSvcToPipeMap[] = {
    { kSvcWmiDataVo,  kPipeDirOut, 3 },
    { kSvcWmiDataVo,  kPipeDirIn,  2 },
    { kSvcWmiDataBk,  kPipeDirOut, 3 },
    { kSvcWmiDataBk,  kPipeDirIn,  2 },
    { kSvcWmiDataBe,  kPipeDirOut, 3 },
    { kSvcWmiDataBe,  kPipeDirIn,  2 },
    { kSvcWmiDataVi,  kPipeDirOut, 3 },
    { kSvcWmiDataVi,  kPipeDirIn,  2 },
    { kSvcWmiControl, kPipeDirOut, 3 },
    { kSvcWmiControl, kPipeDirIn,  2 },
    { kSvcRsvdCtrl,   kPipeDirOut, 0 },
    { kSvcRsvdCtrl,   kPipeDirIn,  1 },
    { kSvcTestRaw,    kPipeDirOut, 0 },
    { kSvcTestRaw,    kPipeDirIn,  1 },
    { kSvcHttDataMsg, kPipeDirOut, 4 },
    { kSvcHttDataMsg, kPipeDirIn,  5 },
    { 0, 0, 0 },                                       // terminator
};
static const uint32_t kNumSvcToPipe = 17;

// pcie_state field offsets (pci.h:42): pipe_cfg_addr, svc_to_pipe_map,
// msi_requested, msi_granted, msi_addr, msi_data, msi_fw_intr_data,
// power_mgmt_method, config_flags
static const uint32_t kPcieStatePipeCfgOff   = 0x00;
static const uint32_t kPcieStateSvcMapOff    = 0x04;
static const uint32_t kPcieStateConfigFlgOff = 0x20;
static const uint32_t kPcieCfgFlagEnableL1   = 0x00000001;

static const uint32_t kHiOptionFlag2Offset   = 0xcc;   // targaddrs.h:148
static const uint32_t kHiEarlyAllocOffset    = 0x100;  // targaddrs.h:184
static const uint32_t kHiOptionEarlyCfgDone  = 0x10;
static const uint32_t kHiEarlyAllocMagic     = 0x6d8a;
static const uint32_t kHiEarlyAllocMagicMask = 0xffff0000;
static const uint32_t kHiEarlyAllocMagicShift = 16;
static const uint32_t kHiEarlyAllocBanksMask = 0x0000000f;
static const uint32_t kHiEarlyAllocBanksShift = 0;
static const uint32_t kQca9377NumBanks       = 9;       // pci.c:2308

bool com_bswork_QCA9377::initConfig(void)
{
    // --- walk the interconnect: pcie_state -> pipe cfg + svc map areas ---
    uint32_t pcieStateAddr = 0;
    if (!fCe->diagRead32(kHiBaseAddress + kHiInterconnectStateOffset,
                          &pcieStateAddr) || pcieStateAddr == 0) {
        qlog("QCA9377: initConfig: pcie_state addr read failed (0x%08x)\n",
              pcieStateAddr);
        return false;
    }
    qlog("QCA9377: initConfig: pcie_state @ 0x%08x\n", pcieStateAddr);

    uint32_t pipeCfgAddr = 0, svcMapAddr = 0, cfgFlags = 0;
    if (!fCe->diagRead32(pcieStateAddr + kPcieStatePipeCfgOff, &pipeCfgAddr)
        || pipeCfgAddr == 0) {
        qlog("QCA9377: initConfig: pipe_cfg addr invalid\n");
        return false;
    }
    if (!fCe->diagRead32(pcieStateAddr + kPcieStateSvcMapOff, &svcMapAddr)
        || svcMapAddr == 0) {
        qlog("QCA9377: initConfig: svc_map addr invalid\n");
        return false;
    }
    // config_flags: clear L1 (target-side ASPM; host side already off)
    if (!fCe->diagRead32(pcieStateAddr + kPcieStateConfigFlgOff, &cfgFlags))
        return false;
    cfgFlags &= ~kPcieCfgFlagEnableL1;
    if (!fCe->diagWrite32(pcieStateAddr + kPcieStateConfigFlgOff, cfgFlags))
        return false;

    // --- download pipe config + service map ---
    if (!fCe->diagWriteMem(pipeCfgAddr, kTargetCeConfigWlan,
                            kNumTargetCeConfig * sizeof(CePipeConfig))) {
        qlog("QCA9377: initConfig: pipe cfg download failed\n");
        return false;
    }
    if (!fCe->diagWriteMem(svcMapAddr, kTargetSvcToPipeMap,
                            kNumSvcToPipe * sizeof(CeSvcToPipe))) {
        qlog("QCA9377: initConfig: svc map download failed\n");
        return false;
    }
    publishNum("initcfg-pipeaddr", pipeCfgAddr);
    publishNum("initcfg-svcaddr", svcMapAddr);

    // --- early allocation: 9 IRAM banks, magic in the top half ---
    uint32_t ealloc = 0;
    if (!fCe->diagRead32(kHiBaseAddress + kHiEarlyAllocOffset, &ealloc))
        return false;
    ealloc |= ((kHiEarlyAllocMagic << kHiEarlyAllocMagicShift)
               & kHiEarlyAllocMagicMask);
    ealloc |= ((kQca9377NumBanks << kHiEarlyAllocBanksShift)
               & kHiEarlyAllocBanksMask);
    if (!fCe->diagWrite32(kHiBaseAddress + kHiEarlyAllocOffset, ealloc))
        return false;

    // --- tell the target early configuration is done ---
    uint32_t flag2 = 0;
    if (!fCe->diagRead32(kHiBaseAddress + kHiOptionFlag2Offset, &flag2))
        return false;
    flag2 |= kHiOptionEarlyCfgDone;
    if (!fCe->diagWrite32(kHiBaseAddress + kHiOptionFlag2Offset, flag2))
        return false;

    qlog("QCA9377: initConfig complete (cfg@0x%08x map@0x%08x ealloc=0x%08x flag2=0x%08x)\n",
          pipeCfgAddr, svcMapAddr, ealloc, flag2);
    return true;
}

// ---- log-tail mirror (v0.7.2) ---------------------------------------------
// qlog() replaces IOLog in this TU: same dmesg line, plus the text is kept
// in a scrolling buffer that is mirrored to /options ("bswork-qca-logtail")
// after every append. If start() wedges mid-M3, NVRAM still carries the
// last ~20 lines — no stage boundary required.
void com_bswork_QCA9377::logTailFlush(void)
{
    if (fNvram)
        fNvram->setProperty("bswork-qca-logtail", fLogTail);
}

void com_bswork_QCA9377::qlog(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    // ret = full length the string WOULD have had (may exceed the buffer on
    // truncation); the bytes actually present are capped by sizeof(line)-1.
    const int ret = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (ret <= 0)
        return;

    IOLog("%s", line);

    const uint32_t len = (uint32_t)ret;
    const uint32_t space = sizeof(fLogTail) - 1 - fLogTailUsed;
    if (len + 1 > space) {
        // drop oldest half, keep it line-aligned (bcopy: src, dst order)
        uint32_t cut = fLogTailUsed / 2;
        while (cut < fLogTailUsed && fLogTail[cut] != '\n')
            cut++;
        if (cut < fLogTailUsed)
            cut++;
        fLogTailUsed -= cut;
        bcopy(fLogTail + cut, fLogTail, fLogTailUsed);
    }
    const uint32_t present = (len < sizeof(line)) ? len : (uint32_t)sizeof(line) - 1;
    const uint32_t space2 = sizeof(fLogTail) - 1 - fLogTailUsed;
    const uint32_t n = (present < space2) ? present : space2;
    bcopy(line, fLogTail + fLogTailUsed, n);
    fLogTailUsed += n;
    fLogTail[fLogTailUsed] = '\0';
    logTailFlush();
}

void com_bswork_QCA9377::publishMac(void)
{
    if (!fWmi)
        return;
    uint8_t mac[6];
    fWmi->macAddress(mac);
    char text[18];
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    setProperty("qca-mac", text);
}
uint32_t com_bswork_QCA9377::read32(uint32_t offset)
{
    return OSReadLittleInt32(fBar0, offset);
}

void com_bswork_QCA9377::write32(uint32_t offset, uint32_t value)
{

    OSWriteLittleInt32(fBar0, offset, value);
}

bool com_bswork_QCA9377::isAwake(void)
{

    uint32_t state = read32(kPCIe_LocalBaseAddress + 0x00000000);
    return ((state & 0x00000007) == kRTCStateValOn);
}

bool com_bswork_QCA9377::wakeTarget(void)
{
    uint32_t elapsed = 0;
    uint32_t step    = kWakeStepStart_us;

    write32(kPCIe_SOCWake_Offset, 0x00000001);

    while (elapsed < kWakeTimeout_us) {
        if (isAwake())
            return true;
        IODelay(step);
        elapsed += step;
        if (step < kWakeStepMax_us)
            step += 5;
    }
    return false;
}

bool com_bswork_QCA9377::probeRegisters(void)
{

    uint32_t chipId = read32(kSOC_ChipID_Offset);
    publishNum("chip-id", chipId);
    qlog("QCA9377: SOC chip_id = 0x%08x (Linux dmesg: 0x003821ff)\n", chipId);

    //    0 = firmware never started (cold) - the ideal M1 condition.

    uint32_t fwInd = read32(kFWIndicatorAddress);
    publishNum("fw-indicator", fwInd);
    qlog("QCA9377: fw_indicator = 0x%08x [%s%s] (0 = cold target)\n",
          fwInd,
          (fwInd & kFWIndEventPending)  ? " EVENT_PENDING" : "",
          (fwInd & kFWIndInitialized) ? " INITIALIZED" : "");

    uint32_t barReg = read32(kPCIe_BARReg_Offset);
    publishNum("pcie-bar-reg", barReg);
    qlog("QCA9377: PCIE_BAR_REG = 0x%08x\n", barReg);

    return true;
}

uint32_t com_bswork_QCA9377::ceBase(uint32_t ceId)
{
    return kCE0Base + kCEStride * ceId;
}

void com_bswork_QCA9377::probeCopyEngines(void)
{
    for (uint32_t ce = 0; ce < kCECount; ce++) {
        uint32_t base = ceBase(ce);
        uint32_t srBase = read32(base + kCESRBaseLo);
        uint32_t srSize = read32(base + kCESRSize);
        uint32_t drBase = read32(base + kCEDRBaseLo);
        uint32_t drSize = read32(base + kCEDRSize);
        uint32_t srIdx  = read32(base + kCESRWrIndex);
        uint32_t drIdx  = read32(base + kCEDSTWrIndex);
        uint32_t srri   = read32(base + kCECurrentSRRI);
        uint32_t drri   = read32(base + kCECurrentDRRI);
        qlog("QCA9377: CE%u srBase=0x%08x srNent=%u drBase=0x%08x drNent=%u "
              "srW=0x%x drW=0x%x SRRI=0x%x DRRI=0x%x\n",
              ce, srBase, srSize, drBase, drSize, srIdx, drIdx, srri, drri);
    }
    uint32_t ceSum = read32(kCEWrapperBaseAddress + 0x0000);
    qlog("QCA9377: CE wrapper intr summary = 0x%08x\n", ceSum);
}

void com_bswork_QCA9377::logRevisionInfo(void)
{

    uint32_t chipId  = read32(kSOC_ChipID_Offset);
    uint32_t socRev  = (chipId & kChipIdRev_Mask) >> kChipIdRev_LSB;
    const char *name = "unknown";
    if      (socRev == 0) name = "hw1.0";
    else if (socRev == 1) name = "hw1.1";
    else if (socRev == 2) name = "hw1.3";
    qlog("QCA9377: SoC revision %u (%s), pci rev-id 0x%02x\n",
          socRev, name, fPciRev);
}

bool com_bswork_QCA9377::probeBmi(void)
{
    // v0.9.5: ensureCE (not create) — if the warm rescue leg already built
    // the CE manager, reuse it; create here would leak it and double-program
    // the CE0/CE1 rings.
    if (!ensureCE()) {
        qlog("QCA9377: CE create/init failed\n");
        return false;
    }

    // CE7 diag window BEFORE config: init_config programs the target's CE
    // routing through it (ath10k hif_power_up order: chip reset -> init_pipes
    // -> init_config -> wake_target_cpu -> BMI).
    if (!fCe->initDiag()) {
        publishNum("diag-ready", 0);
        qlog("QCA9377: CE7 diag window init failed\n");
        teardownHardware();
        return false;
    }
    publishNum("diag-ready", 1);

    // Interconnect proof of life BEFORE configuring: read the target's
    // fw_indicator through CE7 and cross-check against direct MMIO.
    uint32_t hi = 0, mmio = 0;
    bool diagOk = fCe->diagRead32(kHiBaseAddress + 0x28, &hi);
    mmio = read32(kSOC_CoreBaseAddress + 0x28);
    publishNum("diag-fw-ind", hi);
    if (diagOk) {
        publishNum("diag-mmio-match", hi == mmio ? 1 : 0);
        qlog("QCA9377: CE7 diag FW_IND=0x%08x mmio=0x%08x [%s]\n",
              hi, mmio, hi == mmio ? "MATCH" : "MISMATCH");
    } else {
        publishNum("diag-mmio-match", 0);
        qlog("QCA9377: CE7 diag read of HI fw_ind failed (hi=0x%08x mmio=0x%08x)\n",
              hi, mmio);
    }

    // Pre-config HI snapshot (forensics; initConfig rewrites parts of it).
    uint32_t hiDump[8] = {0};
    if (fCe->diagReadMem(kHiBaseAddress, hiDump, sizeof(hiDump))) {
        for (int w = 0; w < 8; w++)
            qlog("QCA9377: HI[0x%02x] = 0x%08x\n", w * 4, hiDump[w]);
    }

    if (!initConfig()) {
        qlog("QCA9377: initConfig failed - target CE config not programmed\n");
        publishStage("FAIL-initcfg");
        teardownHardware();
        return false;
    }
    publishStage("M2-initcfg");

    wakeTargetCpu();
    publishStage("M2-cpu");

    // Re-probe the registers that read 0 pre-reset (boot 0923): if the cold
    // reset did its job, chip_id must now be real (0xffffffff would mean the
    // device is gone; 0 means the SOC window is still dead).
    uint32_t chipId = read32(kSOC_ChipID_Offset);
    publishNum("chip-id-postreset", chipId);

    fBmi = new qca::Bmi(fCe);
    if (!fBmi || !fBmi->getTargetInfo()) {
        qlog("QCA9377: BMI GET_TARGET_INFO failed\n");
        if (fBmi)
            setProperty("qca-bmi-stage", fBmi->lastStage());
        teardownHardware();          // release CE DMA region, not leak it
        return false;
    }

    return true;
}

// Single teardown path: rings parked + DMA released in reverse order of
// acquisition. Called from stop() and from every failure path so a failed
// probe never leaves a live DMA region behind.
void com_bswork_QCA9377::teardownHardware(void)
{
    if (fWmi) { delete fWmi; fWmi = nullptr; }
    if (fHtc) { delete fHtc; fHtc = nullptr; }
    if (fBmi) { delete fBmi; fBmi = nullptr; }
    if (fCe)  { fCe->destroy(); fCe = nullptr; }
}

bool com_bswork_QCA9377::bootFirmware(void)
{

    qca::FwImage img;
    if (!qca::Fw::parseFirmware(&img)) {
        qlog("QCA9377: M3 fw parse failed\n");
        publishStage("M3-FAIL-fw-parse");
        teardownHardware();
        return false;
    }
    publishStage("M3-fw-parsed");

    const uint8_t *boardData = nullptr;
    uint32_t boardLen = 0;
    bool haveBoard = qca::Fw::selectBoard(qca9377_board2_bin, qca9377_board2_len,
                                          fSubVendor, fSubDevice,
                                          &boardData, &boardLen);
    if (!haveBoard) {

        boardData = qca9377_board_bin;
        boardLen  = qca9377_board_len;
        qlog("QCA9377: M3 falling back to board.bin (%uB)\n", boardLen);
    }

    if (!qca::Fw::configureTarget(fBmi)) {
        qlog("QCA9377: M3 configureTarget failed\n");
        publishStage("M3-FAIL-configure");
        teardownHardware();
        return false;
    }
    publishStage("M3-configured");

    if (!qca::Fw::downloadBoardData(fBmi, boardData, boardLen)) {
        qlog("QCA9377: M3 board data failed\n");
        publishStage("M3-FAIL-board-data");
        teardownHardware();
        return false;
    }
    publishStage("M3-board-ok");

    uint32_t boardId = 0, chipId = 0;
    if (img.otp && img.otpLen) {
        if (!qca::Fw::runOtp(fBmi, img.otp, img.otpLen, &boardId, &chipId)) {
            qlog("QCA9377: M3 OTP failed (continuing, cal may be wrong)\n");
        }
    } else {
        qlog("QCA9377: M3 no OTP image in fw6 (cal may be wrong)\n");
    }

    if (!qca::Fw::downloadFirmware(fBmi, img.firmware, img.firmwareLen)) {
        qlog("QCA9377: M3 firmware download failed\n");
        publishStage("M3-FAIL-fw-download");
        teardownHardware();
        return false;
    }
    publishStage("M3-fw-downloaded");

    if (!qca::Fw::doneAndWaitTargetInit(fBmi, fCe, fBar0)) {
        qlog("QCA9377: M3 target init wait failed\n");
        publishStage("M3-FAIL-init-wait");
        teardownHardware();
        return false;
    }

    publishStage("M3-booted");
    qlog("QCA9377: M3 firmware booted (board=%u chip=%u)\n",
          boardId, chipId);
    return true;
}

// M4: HTC over CE0/1, then WMI-TLV events over CE3/2 (poll-only).
bool com_bswork_QCA9377::startHtcWmi(void)
{
    if (!fCe->initWmi()) {
        qlog("QCA9377: M4 WMI CE pair init failed\n");
        publishStage("M4-FAIL-ce-init");
        teardownHardware();
        return false;
    }
    publishStage("M4-ce-ok");

    fHtc = new qca::Htc(fCe);
    if (!fHtc->waitTarget(10000)) {
        qlog("QCA9377: M4 HTC_READY not seen\n");
        publishStage("M4-FAIL-htc-ready");
        teardownHardware();
        return false;
    }
    publishStage("M4-htc-ready");

    uint8_t eid = 0xFF;
    uint16_t maxMsg = 0;
    if (!fHtc->connectService(qca::kHtcSvcWmiControl, &eid, &maxMsg)) {
        qlog("QCA9377: M4 WMI service connect failed\n");
        publishStage("M4-FAIL-connect");
        teardownHardware();
        return false;
    }
    publishNum("htc-eid", eid);
    publishStage("M4-connected");
    if (!fHtc->setupComplete()) {
        qlog("QCA9377: M4 SETUP_COMPLETE failed\n");
        publishStage("M4-FAIL-setup");
        teardownHardware();
        return false;
    }
    publishStage("M4-setup-ok");

    fWmi = new qca::Wmi(fHtc, fCe);
    if (!fWmi->waitServiceAndReady(10000)) {
        qlog("QCA9377: M4 WMI SERVICE_READY/READY failed\n");
        publishStage("M4-FAIL-wmi-ready");
        teardownHardware();
        return false;
    }
    publishStage("M4-wmi-online");
    publishNum("fw-build", fWmi->fwBuild());
    publishNum("fw-abi0", fWmi->abiVersion0());
    publishNum("fw-memreqs", fWmi->numMemReqs());
    publishNum("fw-phy-cap", fWmi->phyCapability());
    publishNum("fw-rf-chains", fWmi->numRfChains());
    publishMac();

    uint8_t mac[6];
    fWmi->macAddress(mac);
    qlog("QCA9377: M4 WMI online - mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

// kmod linkage - the classic command-line kext recipe. libkmod does NOT
// define kmod_info; every kext must define it (XNU osfmk/mach/kmod.h).
// OpenCore's prelinker locates this symbol to wire _PrelinkKmodInfo; a
// kext without a defined kmod_info is rejected at injection with
// "Prelinked injection ... Invalid Parameter" (boot test 2026-09-18).

extern "C" {

__attribute__((visibility("default")))
kern_return_t qca9377_kmod_start(kmod_info_t *, void *)
{
    return KERN_SUCCESS;
}

__attribute__((visibility("default")))
kern_return_t qca9377_kmod_stop(kmod_info_t *, void *)
{
    return KERN_SUCCESS;
}

__attribute__((visibility("default")))
kmod_start_func_t *_realmain = qca9377_kmod_start;

__attribute__((visibility("default")))
kmod_stop_func_t *_antimain = qca9377_kmod_stop;

__attribute__((visibility("default")))
kmod_info_t kmod_info = {
    0,
    KMOD_INFO_VERSION,
    0,
    "com.bswork.QCA9377",
    QCA_DRIVER_VERSION,
    -1,
    0, 0, 0, 0,
    qca9377_kmod_start,
    qca9377_kmod_stop
};

}
