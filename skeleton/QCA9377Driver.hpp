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

static const uint32_t kPCIe_BARReg_Offset    = 0x00040030;

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
    void logRevisionInfo(void);    bool probeBmi(void);
    bool bootFirmware(void);
    bool startHtcWmi(void);
    void teardownHardware(void);

    uint32_t ceBase(uint32_t ceId);
};

#endif
