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
          fPci->configRead16(0x00),
          fPci->configRead16(0x02),
          fPci->configRead8(0x08),
          fPci->configRead16(0x2C),
          fPci->configRead16(0x2E));
    fPciRev = fPci->configRead8(0x08);
    fSubVendor = fPci->configRead16(0x2C);
    fSubDevice = fPci->configRead16(0x2E);

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

    if (!probeBmi()) {
        IOLog("QCA9377: M2 BMI probe failed - staying loaded for diagnostics\n");

    }

    // M3: firmware boot (no interrupts yet; poll-only). Only attempted

    bool m3ok = false;
    if (fBmi && fBmi->targetVersion() != 0) {
        m3ok = bootFirmware();
    } else {
        IOLog("QCA9377: M3 skipped - BMI probe did not succeed\n");
    }

    // M4: HTC handshake + WMI-TLV SERVICE_READY/READY (poll-only). Verdict:
    // target's firmware is alive and talking on CE2/CE3.
    bool m4ok = false;
    if (m3ok) {
        m4ok = startHtcWmi();
    } else {
        IOLog("QCA9377: M4 skipped - firmware not booted\n");
    }

    IOLog("QCA9377: SUMMARY ok=1 version=0.5.3 pciRev=0x%02x bmiTarget=0x%08x m3=%s m4=%s\n",
          fPciRev, fBmi ? fBmi->targetVersion() : 0,
          m3ok ? "BOOTED" : "no",
          m4ok ? "WMI_ONLINE" : "no");

    IOLog("QCA9377: probe complete - staying passive (no MSI, no interrupts, no fw load)\n");
    registerService();
    return true;
}

void com_bswork_QCA9377::stop(IOService *provider)
{
    IOLog("QCA9377: stop\n");
    if (fWmi) { delete fWmi; fWmi = nullptr; }
    if (fHtc) { delete fHtc; fHtc = nullptr; }
    if (fBmi) { delete fBmi; fBmi = nullptr; }
    if (fCe)  { fCe->destroy(); fCe = nullptr; }
    super::stop(provider);
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
    IOLog("QCA9377: SOC chip_id = 0x%08x (Linux dmesg: 0x003821ff)\n", chipId);

    //    0 = firmware never started (cold) - the ideal M1 condition.

    uint32_t fwInd = read32(kFWIndicatorAddress);
    IOLog("QCA9377: fw_indicator = 0x%08x [%s%s] (0 = cold target)\n",
          fwInd,
          (fwInd & kFWIndEventPending)  ? " EVENT_PENDING" : "",
          (fwInd & kFWIndInitialized) ? " INITIALIZED" : "");

    uint32_t barReg = read32(kPCIe_BARReg_Offset);
    IOLog("QCA9377: PCIE_BAR_REG = 0x%08x\n", barReg);

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
        IOLog("QCA9377: CE%u srBase=0x%08x srNent=%u drBase=0x%08x drNent=%u "
              "srW=0x%x drW=0x%x SRRI=0x%x DRRI=0x%x\n",
              ce, srBase, srSize, drBase, drSize, srIdx, drIdx, srri, drri);
    }
    uint32_t ceSum = read32(kCEWrapperBaseAddress + 0x0000);
    IOLog("QCA9377: CE wrapper intr summary = 0x%08x\n", ceSum);
}

void com_bswork_QCA9377::logRevisionInfo(void)
{

    uint32_t chipId  = read32(kSOC_ChipID_Offset);
    uint32_t socRev  = (chipId & kChipIdRev_Mask) >> kChipIdRev_LSB;
    const char *name = "unknown";
    if      (socRev == 0) name = "hw1.0";
    else if (socRev == 1) name = "hw1.1";
    else if (socRev == 2) name = "hw1.3";
    IOLog("QCA9377: SoC revision %u (%s), pci rev-id 0x%02x\n",
          socRev, name, fPciRev);
}

bool com_bswork_QCA9377::probeBmi(void)
{
    fCe = qca::CEManager::create(fBar0);
    if (!fCe || !fCe->init()) {
        IOLog("QCA9377: CE init failed\n");
        if (fCe) { fCe->destroy(); fCe = nullptr; }
        return false;
    }

    fBmi = new qca::Bmi(fCe);
    if (!fBmi || !fBmi->getTargetInfo()) {
        IOLog("QCA9377: BMI GET_TARGET_INFO failed\n");
        return false;
    }
    return true;
}

bool com_bswork_QCA9377::bootFirmware(void)
{

    qca::FwImage img;
    if (!qca::Fw::parseFirmware(&img)) {
        IOLog("QCA9377: M3 fw parse failed\n");
        return false;
    }

    const uint8_t *boardData = nullptr;
    uint32_t boardLen = 0;
    bool haveBoard = qca::Fw::selectBoard(qca9377_board2_bin, qca9377_board2_len,
                                          fSubVendor, fSubDevice,
                                          &boardData, &boardLen);
    if (!haveBoard) {

        boardData = qca9377_board_bin;
        boardLen  = qca9377_board_len;
        IOLog("QCA9377: M3 falling back to board.bin (%uB)\n", boardLen);
    }

    if (!qca::Fw::configureTarget(fBmi)) {
        IOLog("QCA9377: M3 configureTarget failed\n");
        return false;
    }

    if (!qca::Fw::downloadBoardData(fBmi, boardData, boardLen)) {
        IOLog("QCA9377: M3 board data failed\n");
        return false;
    }

    uint32_t boardId = 0, chipId = 0;
    if (img.otp && img.otpLen) {
        if (!qca::Fw::runOtp(fBmi, img.otp, img.otpLen, &boardId, &chipId)) {
            IOLog("QCA9377: M3 OTP failed (continuing, cal may be wrong)\n");
        }
    } else {
        IOLog("QCA9377: M3 no OTP image in fw6 (cal may be wrong)\n");
    }

    if (!qca::Fw::downloadFirmware(fBmi, img.firmware, img.firmwareLen)) {
        IOLog("QCA9377: M3 firmware download failed\n");
        return false;
    }

    if (!qca::Fw::doneAndWaitTargetInit(fBmi, fCe, fBar0)) {
        IOLog("QCA9377: M3 target init wait failed\n");
        return false;
    }

    IOLog("QCA9377: M3 firmware booted (board=%u chip=%u)\n",
          boardId, chipId);
    return true;
}

// M4: HTC over CE0/1, then WMI-TLV events over CE3/2 (poll-only).
bool com_bswork_QCA9377::startHtcWmi(void)
{
    if (!fCe->initWmi()) {
        IOLog("QCA9377: M4 WMI CE pair init failed\n");
        return false;
    }

    fHtc = new qca::Htc(fCe);
    if (!fHtc->waitTarget(10000)) {
        IOLog("QCA9377: M4 HTC_READY not seen\n");
        return false;
    }

    uint8_t eid = 0xFF;
    uint16_t maxMsg = 0;
    if (!fHtc->connectService(qca::kHtcSvcWmiControl, &eid, &maxMsg)) {
        IOLog("QCA9377: M4 WMI service connect failed\n");
        return false;
    }
    if (!fHtc->setupComplete()) {
        IOLog("QCA9377: M4 SETUP_COMPLETE failed\n");
        return false;
    }

    fWmi = new qca::Wmi(fHtc, fCe);
    if (!fWmi->waitServiceAndReady(10000)) {
        IOLog("QCA9377: M4 WMI SERVICE_READY/READY failed\n");
        return false;
    }

    uint8_t mac[6];
    fWmi->macAddress(mac);
    IOLog("QCA9377: M4 WMI online - mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
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
    "0.5.3",
    -1,
    0, 0, 0, 0,
    qca9377_kmod_start,
    qca9377_kmod_stop
};

}
