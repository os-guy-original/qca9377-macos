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

// ---------------------------------------------------------------------------
// Register map - every constant below is verified against the pinned ath10k
// sources (ref/linux-ath10k), file and line cited inline. Do not edit values
// without re-reading those files.
// ---------------------------------------------------------------------------

// QCA9377 uses the qca6174 register set. Source: ath10k/core.c:3649-3653
// ("case ATH10K_HW_QCA6174: case ATH10K_HW_QCA9377: ar->regs = &qca6174_regs").
// All offsets below are relative to BAR0 (MMIO window), from hw.c:44-72.

static const uint32_t kRTC_SOC_BaseAddress   = 0x00000800; // hw.c:45
static const uint32_t kSOC_CoreBaseAddress   = 0x0003A000; // hw.c:47
static const uint32_t kCEWrapperBaseAddress  = 0x00034000; // hw.c:49 (CE0..7 at +0x400 steps, hw.c:50-57)
static const uint32_t kPCIe_LocalBaseAddress = 0x00080000; // hw.c:63
static const uint32_t kFWIndicatorAddress    = 0x0003A028; // hw.c:62 (SOC_CORE_BASE + scratch_3)

// SOC chip id register: hw.c:60 (soc_chip_id_address = 0xf0), read as
// BAR0 + kRTC_SOC_BaseAddress + 0xf0 via ath10k_pci_soc_read32 (pci.c:691-694).
static const uint32_t kSOC_ChipID_Offset     = kRTC_SOC_BaseAddress + 0x000000F0; // = 0x8f0

// SoC wake control: hw.h:866-868 (PCIE_SOC_WAKE_ADDRESS 0x0004), accessed
// relative to PCIE_LOCAL_BASE per ath10k/pci.c:433.
static const uint32_t kPCIe_SOCWake_Offset   = kPCIe_LocalBaseAddress + 0x00000004; // = 0x80004
static const uint32_t kPCIe_SOCWake_V_MASK   = 0x00000001; // hw.h:866

// Boot info register read raw from BAR0 per ath10k/pci.c:883
// ("val = ath10k_pci_read32(ar, PCIE_BAR_REG_ADDRESS)"), hw.h:996.
static const uint32_t kPCIe_BARReg_Offset    = 0x00040030;

// Wake timeout: ath10k/pci.h:201 "#define PCIE_WAKE_TIMEOUT 30000 /* 30ms */".
// Poll pattern copied from ath10k_pci_wake_wait (pci.c:468-488): 5us steps
// growing to 50us. QCA9377 uses this path: pci.c:3588 sets pci_ps=true for
// QCA9377_1_0_DEVICE_ID, which gates ath10k_pci_wake (pci.c:538).
static const uint32_t kWakeTimeout_us        = 30000;  // ath10k PCIE_WAKE_TIMEOUT
static const uint32_t kWakeStepStart_us      = 5;
static const uint32_t kWakeStepMax_us        = 50;

// Chip-id revision field: hw.h:916-917 (SOC_CHIP_ID_REV_LSB=8,
// SOC_CHIP_ID_REV_MASK=0x00000f00). QCA6174 hw revision map: hw.h:70-72
// (1_0=0, 1_1=1, 1_3=2). Linux dmesg on this card: chip_id 0x003821ff.
static const uint32_t kChipIdRev_LSB         = 8;
static const uint32_t kChipIdRev_Mask        = 0x00000f00;

// RTC awake value: hw.c:153 (qca6174_values.rtc_state_val_on = 3).
static const uint32_t kRTCStateValOn         = 3;

// Firmware indicator bits in SCRATCH_3: hw.h:984-987.
static const uint32_t kFWIndEventPending     = 1; // hw.h:986
static const uint32_t kFWIndInitialized      = 2; // hw.h:987

// Copy engines: hw.c:52-59 (qca6174_regs) - CE0=0x34400, CE1=0x34800,
// stride 0x400; count = 8 (hw.c:154, qca6174_values.ce_count).
static const uint32_t kCE0Base               = 0x00034400;
static const uint32_t kCE1Base               = 0x00034800;
static const uint32_t kCEStride              = kCE1Base - kCE0Base;
static const uint32_t kCECount               = 8;

// Per-CE ring register offsets: qcax_ce_regs (hw.c:462-476), the set
// assigned to QCA6174/QCA9377 at core.c:3649-3653.
static const uint32_t kCESRBaseLo            = 0x00; // sr_base_addr_lo
static const uint32_t kCESRSize              = 0x04; // sr_size_addr
static const uint32_t kCEDRBaseLo            = 0x08; // dr_base_addr_lo
static const uint32_t kCEDRSize              = 0x0c; // dr_size_addr
static const uint32_t kCESRWrIndex           = 0x3c; // sr_wr_index_addr
static const uint32_t kCEDSTWrIndex          = 0x40; // dst_wr_index_addr
static const uint32_t kCECurrentSRRI         = 0x44; // current_srri_addr
static const uint32_t kCECurrentDRRI         = 0x48; // current_drri_addr

// ---------------------------------------------------------------------------
// Driver class
// ---------------------------------------------------------------------------

class com_bswork_QCA9377 : public IOService
{
    OSDeclareDefaultStructors(com_bswork_QCA9377)

public:
    bool init(OSDictionary *dictionary) override;
    void free(void) override;

    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    // IOKit power management placeholder - M1 does not implement PM.
    // (Intentionally absent so the power-on default applies.)

private:
    IOPCIDevice   *fPci       = nullptr;
    IODeviceMemory *fBar0Mem  = nullptr;
    volatile uint32_t *fBar0  = nullptr;   // mapped BAR0
    IOByteCount     fBar0Len = 0;
    uint8_t         fPciRev  = 0;          // PCI config-space revision ID
    uint16_t        fSubVendor  = 0;       // PCI config 0x2C (board select)
    uint16_t        fSubDevice  = 0;       // PCI config 0x2E (board select)

    // M2: CE ring manager + BMI protocol (owned, plain C++ objects).
    qca::CopyEngine *fCe = nullptr;
    qca::Bmi        *fBmi = nullptr;

    // -- MMIO helpers (M1: plain 32-bit LE reads/writes, no windowing -
    //    matches ath10k_bus_pci_read32, pci.c:652-671, in this kernel) --
    uint32_t read32(uint32_t offset);
    void     write32(uint32_t offset, uint32_t value);

    // -- wake protocol (ath10k/pci.c:468-488 pattern) --
    bool wakeTarget(void);
    bool isAwake(void);

    // -- M1 diagnostics --
    bool probeRegisters(void);   // chip id, fw indicator, boot info
    void probeCopyEngines(void); // read-only sweep of CE0..7 ring state
    void logRevisionInfo(void);  // decode+log chip-id revision vs PCIe rev

    // -- M2: Copy Engine rings + BMI handshake --
    bool probeBmi(void);         // CE init + GET_TARGET_INFO exchange

    // -- M3: firmware boot through BMI --
    bool bootFirmware(void);     // parse fw6, board select, configure,
                                 // board data, OTP, firmware, BMI_DONE+wait

    uint32_t ceBase(uint32_t ceId); // CE0_BASE + stride*id (ce.h:341)
};

#endif /* QCA9377Driver_hpp */
