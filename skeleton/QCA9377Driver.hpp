/*
 * QCA9377Driver.hpp - M1 skeleton IOKit PCI driver for QCA9377 (168c:0042).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k (drivers/net/wireless/ath/ath10k),
 * which is ISC-licensed: Copyright (c) Atheros Communications Inc.,
 * Copyright (c) Qualcomm Atheros, Inc. Adapted per ISC terms with attribution.
 *
 * Derived from Linux v7.2.3 drivers/net/wireless/ath/ath10k/{hw.c,hw.h,pci.c}
 * (tarball sha256 8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03).
 *
 * M1 SCOPE (read-only probe only):
 *   - match PCI 168c:0042
 *   - map BAR0 (2 MiB, 64-bit, non-prefetchable)
 *   - wake the SoC and read diagnostic registers
 *   - log everything; touch nothing else
 *
 * NOT in M1: MSI setup, copy engines, BMI, firmware load, WMI, HTT, network.
 */

#ifndef QCA9377Driver_hpp
#define QCA9377Driver_hpp

// Single source of truth for the version. Info.plist CFBundleVersion must
// match this string (ocvalidate battery compares the two).
#define QCA_DRIVER_VERSION "0.9.5"

#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOService.h>
#include "CE.hpp"
#include "BMI.hpp"
#include "FW.hpp"
#include "HTC.hpp"
#include "WMI.hpp"

// Register map - every constant below is verified against the pinned ath10k
// sources (ref/linux-ath10k), file and line cited inline. Do not edit values

static const uint32_t kRTC_SOC_BaseAddress   = 0x00000800;
static const uint32_t kSOC_CoreBaseAddress   = 0x0003A000;
static const uint32_t kCEWrapperBaseAddress  = 0x00034000;
static const uint32_t kPCIe_LocalBaseAddress = 0x00080000;
static const uint32_t kFWIndicatorAddress    = 0x0003A028;

static const uint32_t kSOC_ChipID_Offset     = kRTC_SOC_BaseAddress + 0x000000F0;

static const uint32_t kPCIe_SOCWake_Offset   = kPCIe_LocalBaseAddress + 0x00000004;
static const uint32_t kPCIe_SOCWake_V_MASK   = 0x00000001;

static const uint32_t kPCIe_BARReg_Offset    = 0x00040030;// Host-Interest area base in target RAM (all ath10k PCIe targets,
// targaddrs.h QCA988X_HOST_INTEREST_ADDRESS — shared by QCA6174/9377).
static const uint32_t kHiBaseAddress                 = 0x00400800;

// HI item offsets (targaddrs.h struct host_interest)
static const uint32_t kHiInterconnectStateOffset     = 0xf8;

// SOC-domain reset/control registers (RTC_SOC_BASE + offset). Values from
// ath10k hw.h/qca6174_regs — byte-verified against the pinned sources.
static const uint32_t kRTCStateMaskSt        = 0x00000007;

// CORE_CTRL (SOC_CORE_BASE + 0x0): CPU_INTR_MASK is the doorbell that wakes
// the target CPU — ath10k writes it after CE init, before the first BMI
// exchange (wake_target_cpu, pci.c).
static const uint32_t kSocCoreCtrlOffset     = 0x00000000;
static const uint32_t kCoreCtrlCpuIntrMask   = 0x00002000;

static const uint32_t kSocGlobalResetOffset  = 0x00000008;
static const uint32_t kSocResetControlOffset = 0x00000000;
static const uint32_t kSocResetCeRstMask     = 0x00000001;
static const uint32_t kSocResetCpuWarmRstMask = 0x00000040;
static const uint32_t kSocLfTimerControl0Offset = 0x00000050;
static const uint32_t kSocLfTimerEnableMask  = 0x00000004;

// Reset sequencing (v0.8.0): timeouts from ath10k constants.
// v0.9.4: the boot-20260923-1827 experiment — cold reset works (chip-id
// 0x003821FF read correctly post-reset) but the ROM never announced init
// within 3 s. Widened window + per-second samples to see if it announces
// LATE (timing) or NEVER (ROM dead in our environment).
static const uint32_t kTargetInitTimeout_ms  = 10000;
static const uint32_t kTargetInitStep_ms     = 10;
static const uint32_t kTargetInitSample_ms   = 1000;
static const uint32_t kColdResetDelay_ms     = 20;
static const uint32_t kWarmResetStep_ms      = 10;

static const uint32_t kWakeTimeout_us        = 30000;
static const uint32_t kWakeStepStart_us      = 5;
static const uint32_t kWakeStepMax_us        = 50;

static const uint32_t kChipIdRev_LSB         = 8;
static const uint32_t kChipIdRev_Mask        = 0x00000f00;

static const uint32_t kRTCStateValOn         = 3;

static const uint32_t kFWIndEventPending     = 1;
static const uint32_t kFWIndInitialized      = 2;

static const uint32_t kCE0Base               = 0x00034400;
static const uint32_t kCE1Base               = 0x00034800;
static const uint32_t kCEStride              = kCE1Base - kCE0Base;
static const uint32_t kCECount               = 8;

static const uint32_t kCESRBaseLo            = 0x00;
static const uint32_t kCESRSize              = 0x04;
static const uint32_t kCEDRBaseLo            = 0x08;
static const uint32_t kCEDRSize              = 0x0c;
static const uint32_t kCESRWrIndex           = 0x3c;
static const uint32_t kCEDSTWrIndex          = 0x40;
static const uint32_t kCECurrentSRRI         = 0x44;
static const uint32_t kCECurrentDRRI         = 0x48;

class com_bswork_QCA9377 : public IOService
{
    OSDeclareDefaultStructors(com_bswork_QCA9377)

public:
    bool init(OSDictionary *dictionary) override;
    void free(void) override;

    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    IOPCIDevice   *fPci       = nullptr;
    IODeviceMemory *fBar0Mem  = nullptr;
    volatile uint32_t *fBar0  = nullptr;
    IOByteCount     fBar0Len = 0;
    uint8_t         fPciRev  = 0;

    // PCIe link power management (ath10k hif_power_up clears ASPM before
    // any reset work — L1 PM substates on QCA61x4 are a known hang source).
    uint16_t        fAspmCtl   = 0;
    uint8_t         fAspmCapOff = 0;

    uint16_t        fSubVendor  = 0;
    uint16_t        fSubDevice  = 0;

    qca::CEManager *fCe = nullptr;
    qca::Bmi        *fBmi = nullptr;
    qca::Htc        *fHtc = nullptr;
    qca::Wmi        *fWmi = nullptr;

    uint32_t read32(uint32_t offset);
    void     write32(uint32_t offset, uint32_t value);

    bool wakeTarget(void);
    bool isAwake(void);

    bool probeRegisters(void);
    void probeCopyEngines(void);
    void logRevisionInfo(void);
    bool probeBmi(void);
    bool bootFirmware(void);
    bool startHtcWmi(void);
    bool resetChip(void);
    bool coldReset(void);
    bool waitForTargetInit(void);
    bool ensureCE(void); // v0.9.5: init_pipes equivalent (create/recreate fCe)
    // v0.9.5: full ath10k_pci_warm_reset (pci.c:2623) — SI0 + CPU warm reset
    // + LF timer + CE reset, WITH the init_pipes/wait_for_target_init
    // interludes (creates/recreates fCe as needed). The rescue path when the
    // cold reset revives the SOC but the ROM never announces (both boots
    // since v0.8.0: rtc=3, fw-ind=0 for the whole window).
    bool warmReset(void);
    void wakeTargetCpu(void);
    bool initConfig(void);
    void teardownHardware(void);

    // Staged bring-up gate (v0.7.1): boot-arg "qca-maxstage=1..4" caps how
    // far start() runs — 1 = M1 only, 2 = +BMI, 3 = +fw boot, 4 = +HTC/WMI
    // (default). Lets each boot test isolate one stage: a hang deep in M3
    // firmware boot can no longer destroy the boot's telemetry, and a
    // failing stage can be probed in isolation with the card left quiet.
    unsigned fMaxStage = 4;

    // Milestone telemetry (v0.6.0): probe progress as "qca-*" properties on
    // our own registry node. ioreg never evicts, so the diag's NVRAM report
    // carries the exact milestone chain even when dmesg has rotated.
    void publishStage(const char *stage);
    void publishNum(const char *key, uint32_t v);
    void publishMac(void);

    // v0.7.0: mirror the stage into NVRAM itself via the /options entry
    // (IODT plane is NVRAM-backed). "bswork-qca-stage" then survives even a
    // crashed/never-diag boot; Linux reads it straight from efivars.
    void nvramStage(const char *stage);
    // v0.9.3: the IODTNVRAM *service* — properties set on it commit to EFI
    // (registry nodes like /options never did; that was the v0.7.2 bug).
    class IOService *fNvram = nullptr;

    // v0.7.2: log-tail mirror. qlog() = IOLog + append to a scrolling buffer
    // + mirror the buffer to /options as "bswork-qca-logtail" on every line,
    // so the last ~20 messages survive ANY hang (not just between stages).
    void qlog(const char *fmt, ...);
    void logTailFlush(void);
    char     fLogTail[1536] = {0};
    unsigned fLogTailUsed   = 0;

    uint32_t ceBase(uint32_t ceId);
};

#endif
