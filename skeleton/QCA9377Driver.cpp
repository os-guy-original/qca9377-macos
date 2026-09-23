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

// gIODTPlane: the /options NVRAM mirror needs the IODT plane handle. The
// kernel exports it (_gIODTPlane, verified in the Boot KC symbol table) but
// current SDK headers no longer declare it (xnu only externs gIOServicePlane/
// gIOPowerPlane now); declare it exactly as historical xnu did.
class IORegistryPlane;
extern const IORegistryPlane * gIODTPlane;

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

    // /options is the NVRAM-backed registry entry (IODT plane). Grabbing it
    // once here; nvramStage() mirrors every stage change into it.
    fOptions = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!fOptions)
        qlog("QCA9377: /options not available - NVRAM stage mirror disabled\n");

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

    if (!probeBmi()) {
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
    if (fOptions) {
        fOptions->release();
        fOptions = nullptr;
    }
    if (fPci) {
        // Release bus-master first so the target cannot issue new DMA while
        // we disarm rings, then drop memory decodes.
        fPci->setBusMasterEnable(false);
        fPci->setMemoryEnable(false);
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
// v0.7.0: the stage is also mirrored into real NVRAM via the /options entry
// (IODT plane, NVRAM-backed). If start() panics or the diag never runs,
// Linux still reads "bswork-qca-stage" straight from efivars.
void com_bswork_QCA9377::nvramStage(const char *stage)
{
    if (!fOptions)
        return;
    fOptions->setProperty("bswork-qca-stage", stage);
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

// ---- log-tail mirror (v0.7.2) ---------------------------------------------
// qlog() replaces IOLog in this TU: same dmesg line, plus the text is kept
// in a scrolling buffer that is mirrored to /options ("bswork-qca-logtail")
// after every append. If start() wedges mid-M3, NVRAM still carries the
// last ~20 lines — no stage boundary required.
void com_bswork_QCA9377::logTailFlush(void)
{
    if (fOptions)
        fOptions->setProperty("bswork-qca-logtail", fLogTail);
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
    fCe = qca::CEManager::create(fBar0);
    if (!fCe || !fCe->init()) {
        qlog("QCA9377: CE init failed\n");
        if (fCe) { fCe->destroy(); fCe = nullptr; }
        return false;
    }

    fBmi = new qca::Bmi(fCe);
    if (!fBmi || !fBmi->getTargetInfo()) {
        qlog("QCA9377: BMI GET_TARGET_INFO failed\n");
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
