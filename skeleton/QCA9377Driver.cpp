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
#include <libkern/OSDebug.h>
#include <libkern/OSKextLib.h>
#include <IOKit/IOLib.h>
#include <mach/kmod.h>

#define super IOService
OSDefineMetaClassAndStructors(com_bswork_QCA9377, IOService)

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool com_bswork_QCA9377::init(OSDictionary *dictionary)
{
    if (!super::init(dictionary))
        return false;
    IOLog("QCA9377: init\n");
    return true;
}

void com_bswork_QCA9377::free(void)
{
    IOLog("QCA9377: free\n");
    super::free();
}

bool com_bswork_QCA9377::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) {
        IOLog("QCA9377: provider is not IOPCIDevice\n");
        return false;
    }

    IOLog("QCA9377: start - vendor=0x%04x device=0x%04x rev=0x%02x sub-vendor=0x%04x sub-device=0x%04x\n",
          fPci->configRead16(0x00),          // vendor
          fPci->configRead16(0x02),          // device
          fPci->configRead8(0x08),           // revision
          fPci->configRead16(0x2C),          // subsystem vendor
          fPci->configRead16(0x2E));         // subsystem device
    fPciRev = fPci->configRead8(0x08);     // saved for logRevisionInfo

    // BAR0: 64-bit, non-prefetchable MMIO at config offset 0x10.
    // Ground truth (docs/hardware-ground-truth.txt): 0x51000000, 2 MiB.
    fBar0Mem = fPci->getDeviceMemoryWithIndex(0);
    if (!fBar0Mem) {
        IOLog("QCA9377: no BAR0 memory descriptor\n");
        return false;
    }
    fBar0Len = fBar0Mem->getLength();
    IOLog("QCA9377: BAR0 = 0x%llx, %llu bytes\n",
          (uint64_t)fBar0Mem->getPhysicalAddress(), (uint64_t)fBar0Len);

    fBar0 = (volatile uint32_t *)
        fBar0Mem->map()->getVirtualAddress();
    if (!fBar0) {
        IOLog("QCA9377: BAR0 map failed\n");
        return false;
    }

    if (!wakeTarget()) {
        IOLog("QCA9377: target did not wake (timeout %u us)\n", kWakeTimeout_us);
        return false;
    }
    IOLog("QCA9377: target awake\n");

    if (!probeRegisters()) {
        IOLog("QCA9377: register probe failed\n");
        return false;
    }

    probeCopyEngines();
    logRevisionInfo();

    // One-line machine-friendly verdict for the sos capture to highlight.
    IOLog("QCA9377: SUMMARY ok=1 version=0.2.0 pciRev=0x%02x\n", fPciRev);

    IOLog("QCA9377: M1 probe complete - staying passive (no MSI, no CE, no fw load)\n");
    registerService();
    return true;
}

void com_bswork_QCA9377::stop(IOService *provider)
{
    IOLog("QCA9377: stop\n");
    super::stop(provider);
}

// ---------------------------------------------------------------------------
// MMIO helpers - mirror ath10k_bus_pci_read32 (pci.c:652-671) for this
// kernel: direct 32-bit LE access, no windowing.
// ---------------------------------------------------------------------------

uint32_t com_bswork_QCA9377::read32(uint32_t offset)
{
    return OSReadLittleInt32(fBar0, offset);
}

void com_bswork_QCA9377::write32(uint32_t offset, uint32_t value)
{
    // Used only for the wake register, which is safe to write in ath10k
    // before any hardware init (pci.c:433 area). Everything else is read-only.
    OSWriteLittleInt32(fBar0, offset, value);
}

// ---------------------------------------------------------------------------
// Wake protocol - ath10k/pci.c:468-488 (ath10k_pci_wake_wait) pattern:
// write 1 to PCIE_SOC_WAKE, poll RTC_STATE until the V field reads ON,
// 5us steps growing to 50us. The awake check reads the RTC power state
// register at BAR0 + PCIE_LOCAL_BASE + 0x0000 (hw.h:864 RTC_STATE_ADDRESS).
// ---------------------------------------------------------------------------

bool com_bswork_QCA9377::isAwake(void)
{
    // ath10k checks RTC_STATE_V_GET(read32(RTC_STATE_ADDRESS)) against
    // rtc_state_val_on (=3 for qca6174, hw.c:153).
    uint32_t state = read32(kPCIe_LocalBaseAddress + 0x00000000);
    return ((state & 0x00000007) == kRTCStateValOn); // RTC_STATE_V_MASK=0x7
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

// ---------------------------------------------------------------------------
// M1 diagnostics
// ---------------------------------------------------------------------------

bool com_bswork_QCA9377::probeRegisters(void)
{
    // 1. SOC chip id: ath10k reads BAR0 + RTC_SOC_BASE + 0xf0
    //    (pci.c:691-694 + hw.c:60). Linux dmesg ground truth on this card:
    //    "qca9377 hw1.1 target 0x05020001 chip_id 0x003821ff sub 11ad:08a6".
    uint32_t chipId = read32(kSOC_ChipID_Offset);
    IOLog("QCA9377: SOC chip_id = 0x%08x (Linux dmesg: 0x003821ff)\n", chipId);

    // 2. Firmware indicator scratch: SCRATCH_3 (SOC_CORE_BASE + 0x28,
    //    hw.c:59 -> fw_indicator 0x3a028). Bits: hw.h:986-987.
    //    0 = firmware never started (cold) - the ideal M1 condition.
    //    PENDING/INITIALIZED = warm boot, another OS's ath10k left state.
    uint32_t fwInd = read32(kFWIndicatorAddress);
    IOLog("QCA9377: fw_indicator = 0x%08x [%s%s] (0 = cold target)\n",
          fwInd,
          (fwInd & kFWIndEventPending)  ? " EVENT_PENDING" : "",
          (fwInd & kFWIndInitialized) ? " INITIALIZED" : "");

    // 3. PCIE_BAR_REG (hw.h:996, read raw per pci.c:883).
    uint32_t barReg = read32(kPCIe_BARReg_Offset);
    IOLog("QCA9377: PCIE_BAR_REG = 0x%08x\n", barReg);

    return true;
}

// ---------------------------------------------------------------------------
// Copy engine diagnostics - READ-ONLY sweep. Per-CE base from ce.h:341
// (CE0_BASE + stride*id); ring register offsets from qcax_ce_regs
// (hw.c:462-476). Reads prove mapping sanity and capture ring state a
// previous OS's ath10k may have left behind - M2 ground truth.
// ---------------------------------------------------------------------------

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
        IOLog("QCA9377: CE%u srBase=0x%08x srNent=%u drBase=0x%08x drNent=%u "
              "srW=0x%x drW=0x%x SRRI=0x%x DRRI=0x%x\n",
              ce, srBase, srSize, drBase, drSize, srIdx, drIdx, srri, drri);
    }
    uint32_t ceSum = read32(kCEWrapperBaseAddress + 0x0000); // CE_WRAPPER_INTERRUPT_SUMMARY (ce.h:374)
    IOLog("QCA9377: CE wrapper intr summary = 0x%08x\n", ceSum);
}

void com_bswork_QCA9377::logRevisionInfo(void)
{
    // SOC_CHIP_ID_REV field: bits 11:8 (hw.h:916-917). QCA6174 map
    // (hw.h:70-72): 0=hw1.0, 1=hw1.1, 2=hw1.3. Cross-check against the
    // PCI config-space revision (Linux reported rev 31 = 0x1f for the
    // sub-version; the SoC rev here is the firmware-architecture one).
    uint32_t chipId  = read32(kSOC_ChipID_Offset);
    uint32_t socRev  = (chipId & kChipIdRev_Mask) >> kChipIdRev_LSB;
    const char *name = "unknown";
    if      (socRev == 0) name = "hw1.0";
    else if (socRev == 1) name = "hw1.1";
    else if (socRev == 2) name = "hw1.3";
    IOLog("QCA9377: SoC revision %u (%s), pci rev-id 0x%02x\n",
          socRev, name, fPciRev);
}

// ---------------------------------------------------------------------------
// kmod linkage - the classic command-line kext recipe. libkmod does NOT
// define kmod_info; every kext must define it (XNU osfmk/mach/kmod.h).
// OpenCore's prelinker locates this symbol to wire _PrelinkKmodInfo; a
// kext without a defined kmod_info is rejected at injection with
// "Prelinked injection ... Invalid Parameter" (boot test 2026-09-18).
// ---------------------------------------------------------------------------

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
    0,                     // next (the kernel chains modules)
    KMOD_INFO_VERSION,     // struct format version
    0,                     // id (assigned by the kernel)
    "com.bswork.QCA9377",  // matches Info.plist CFBundleIdentifier
    "0.2.0",               // matches CFBundleShortVersionString
    -1,                    // reference count (kernel-managed)
    0, 0, 0, 0,            // referenceList, address, size, hdrSize
    qca9377_kmod_start,
    qca9377_kmod_stop
};

} // extern "C"
