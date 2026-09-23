/*
 * CE.hpp — Copy Engine ring management for QCA9377 (M2/M4).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k (drivers/net/wireless/ath/ath10k/ce.c,
 * ce.h), ISC-licensed: Copyright (c) Atheros Communications Inc., Copyright
 * (c) Qualcomm Atheros, Inc. Adapted per ISC terms with attribution.
 *
 * Derived from Linux v7.2.3 ath10k, sha256-pinned:
 * 8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03.
 *
 * Two ring pairs, one object:
 *   ctrl pair: CE0 host->target, CE1 target->host (BMI + HTC control)
 *   wmi  pair: CE3 host->target, CE2 target->host (WMI, M4)
 * (pci_target_service_to_ce_map_wlan: WMI_CONTROL UL=3 DL=2,
 *  RSVD_CTRL UL=0 DL=1.)
 *
 * Polling driver, one command in flight. 16-entry rings (ring sizes are
 * host-written; ath10k uses 512/2048). Plain C++ class (libkmodc++).
 *
 * DMA: the whole per-pair region (rings + tx + rx) is one contiguous
 * allocation with a 32-bit physical mask, via
 * IOBufferMemoryDescriptor::inTaskWithPhysicalMask. IOMallocContiguous is
 * NOT exported by the Sequoia Recovery kernelcache — kexts linking against
 * it fail OC prelink injection with EFI_INVALID_PARAMETER.
 */

#ifndef QCA9377_CE_hpp
#define QCA9377_CE_hpp

#include <libkern/c++/OSObject.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <stdint.h>

class IOBufferMemoryDescriptor;
class IODMACommand;

struct CEDescriptor {
    volatile uint32_t addr;
    volatile uint16_t nbytes;
    volatile uint16_t flags;
} __attribute__((packed));

static_assert(sizeof(CEDescriptor) == 8, "ce_desc must be 8 bytes");

static const uint32_t kQCAPollStep_us        = 50;
static const uint32_t kQCAExchangeTimeout_ms = 10000;

namespace qca {

class CECopyPair {
public:
    CECopyPair(volatile uint32_t *bar0, uint32_t srcCe, uint32_t dstCe);

    bool init();
    void teardown();

    bool send(const void *buf, uint32_t len);
    // Post rx BEFORE the matching send — ath10k ordering (pci.c:2155-2176);
    // posting after races the target's response.
    bool postRecv();
    // Number of unconsumed dst entries (posted minus completed). 0 = no
    // buffer in flight; callers must not post again until this returns 0.
    uint32_t rxArmed() const                     { return (fDstWrite - fDstSw) & kRingMask; }
    // Wait for the oldest unconsumed dst entry (FIFO, at fDstSw). A
    // nbytes==0 race (descriptor DMA not landed yet) keeps polling instead
    // of failing, exactly like ath10k's completed_recv_next (ce.c:756-786).
    bool recvWait(uint32_t timeoutMs);

    uint8_t *txBuf() { return fTxCpu; }
    uint8_t *rxBuf() { return fRxCpu; }
    uint32_t rxNbytes() const { return fRxNbytes; }

private:
    bool allocRegion();
    void freeRegion();

    uint32_t read32(uint32_t offset)  { return OSReadLittleInt32(fBar0, offset); }
    void     write32(uint32_t offset, uint32_t v) { OSWriteLittleInt32(fBar0, offset, v); }

    volatile uint32_t *fBar0;
    uint32_t fSrcCe;             // CE id, host->target
    uint32_t fDstCe;             // CE id, target->host

    void    *fRegionCpu  = nullptr;
    uint64_t fRegionPhys = 0;
    uint32_t fRegionSize = 0;

    IOBufferMemoryDescriptor *fBmd = nullptr;
    IODMACommand             *fDma = nullptr;
    bool fBmdPrepared  = false;
    bool fDmaPrepared  = false;

    static const uint32_t kRingN    = 16;
    static const uint32_t kRingMask = kRingN - 1;

    static const uint32_t kTxBufSz = 2048;
    static const uint32_t kRxBufSz = 2048;
    static const uint32_t kAlign   = 4096;

    CEDescriptor *fSrcDesc = nullptr;
    CEDescriptor *fDstDesc = nullptr;
    uint8_t *fTxCpu = nullptr;
    uint8_t *fRxCpu = nullptr;
    uint64_t fTxPhys = 0;
    uint64_t fRxPhys = 0;

    uint32_t fSrcWrite   = 0;
    uint32_t fDstWrite   = 0;
    uint32_t fSrcSw      = 0;
    uint32_t fDstSw      = 0;
    uint32_t fRxNbytes   = 0;
};

// ---------------------------------------------------------------------------
// CEDiag — CE7 diagnostic window (M2.5), ported from ath10k
// ath10k_pci_diag_read_mem (pci.c:897) / diag_write_mem (pci.c:1049).
//
// CE7 is ONE engine serving both directions (src ring + dst ring on the same
// register block). Descriptors carry raw addresses and the chip's internal
// interconnect routes them to target RAM/registers:
//   read:  dst-desc -> bounce buffer, src-desc -> CE-address of target mem
//   write: dst-desc -> CE-address of target mem, src-desc -> bounce buffer
// Target-CPU-address -> CE-address map (qca6174_targ_cpu_to_ce_addr):
//   ce = ((CORE_CTRL & 0x7ff) << 21) | (addr >= 0x100000 ? 0x100000 : 0)
//        | (addr & 0xfffff)
// ---------------------------------------------------------------------------
class CEDiag {
public:
    explicit CEDiag(volatile uint32_t *bar0) : fBar0(bar0) {}
    ~CEDiag() { teardown(); }

    bool init();
    void teardown();

    // len must be <= kDiagMax (caller chunks for bigger transfers)
    bool readMem(uint32_t targAddr, void *out, uint32_t len);
    bool writeMem(uint32_t targAddr, const void *buf, uint32_t len);
    bool read32(uint32_t targAddr, uint32_t *val)  { return readMem(targAddr, val, 4); }
    bool write32(uint32_t targAddr, uint32_t val)  { return writeMem(targAddr, &val, 4); }

private:
    bool allocRegion();
    void freeRegion();
    bool xfer(uint32_t postAddr, uint32_t sendAddr, uint32_t len);
    uint32_t targToCeAddr(uint32_t addr);
    uint32_t regRead32(uint32_t off)        { return OSReadLittleInt32(fBar0, off); }
    void     regWrite32(uint32_t off, uint32_t v) { OSWriteLittleInt32(fBar0, off, v); }

    volatile uint32_t *fBar0;

    IOBufferMemoryDescriptor *fBmd = nullptr;
    IODMACommand             *fDma = nullptr;
    bool     fBmdPrepared = false;
    bool     fDmaPrepared = false;
    void    *fRegionCpu  = nullptr;
    uint64_t fRegionPhys = 0;
    uint32_t fRegionSize = 0;

    CEDescriptor *fSrcDesc = nullptr;
    CEDescriptor *fDstDesc = nullptr;
    uint8_t  *fBounce     = nullptr;
    uint64_t  fBouncePhys = 0;

    uint32_t fSrcWrite = 0, fDstWrite = 0, fSrcSw = 0, fDstSw = 0;

    static const uint32_t kDiagCe      = 7;
    static const uint32_t kRingN       = 16;
    static const uint32_t kRingMask    = kRingN - 1;
    static const uint32_t kDiagMax     = 512;   // bounce size (ath10k chunks at 0x5000)
    static const uint32_t kWaitStep_us    = 50;    // DIAG_ACCESS_CE_WAIT_US
    static const uint32_t kWaitTimeout_us = 10000; // DIAG_ACCESS_CE_TIMEOUT_US
    static const uint32_t kAlign      = 4096;
};

class CEManager {
public:
    static CEManager *create(volatile uint32_t *bar0);
    void destroy();

    bool init();
    bool initWmi();            // WMI pair (M4); call after BMI is up

    // Control pair (CE0/CE1) — BMI + HTC. Same surface as the old class.
    bool send(const void *buf, uint32_t len)     { return fCtrl->send(buf, len); }
    bool postRecv()                              { return fCtrl->postRecv(); }
    bool recvWait(uint32_t timeoutMs)            { return fCtrl->recvWait(timeoutMs); }
    uint8_t *txBuf()                             { return fCtrl->txBuf(); }
    uint8_t *rxBuf()                             { return fCtrl->rxBuf(); }
    uint32_t rxNbytes() const                    { return fCtrl->rxNbytes(); }

    // WMI pair (CE3/CE2).
    bool wmiSend(const void *buf, uint32_t len)  { return fWmi->send(buf, len); }
    bool wmiPostRecv()                           { return fWmi->postRecv(); }
    bool wmiRecvWait(uint32_t timeoutMs)         { return fWmi->recvWait(timeoutMs); }
    // Pre-arm: post an rx buffer on CE2 only when none is in flight. The
    // single shared fRxCpu buffer means a second post while one is pending
    // would let the engine DMA both entries into the same address
    // (self-aliasing → torn/duplicated events).
    void wmiArmRecv()                            { if (fWmi->rxArmed() == 0) (void)fWmi->postRecv(); }
    // Post-on-send path for WMI commands: HTC sendWmi arms CE2 iff idle
    // right before its doorbell, so a spontaneous event cannot arrive in
    // the window where a command response is the expected next frame.
    bool wmiPostRecvIfIdle()                     { if (fWmi->rxArmed() != 0) return true; return fWmi->postRecv(); }
    uint8_t *wmiTxBuf()                          { return fWmi->txBuf(); }
    uint8_t *wmiRxBuf()                          { return fWmi->rxBuf(); }
    uint32_t wmiRxNbytes() const                 { return fWmi->rxNbytes(); }

    // CE7 diag window (M2.5) — raw target-memory access via CE interconnect.
    bool initDiag() {
        if (fDiag) return true;
        fDiag = new CEDiag(fBar0);
        if (!fDiag || !fDiag->init()) {
            delete fDiag; fDiag = nullptr;
            return false;
        }
        return true;
    }
    bool diagRead32(uint32_t targAddr, uint32_t *val) { return fDiag ? fDiag->read32(targAddr, val) : false; }
    bool diagWrite32(uint32_t targAddr, uint32_t val) { return fDiag ? fDiag->write32(targAddr, val) : false; }
    bool diagReadMem(uint32_t targAddr, void *out, uint32_t len) { return fDiag ? fDiag->readMem(targAddr, out, len) : false; }
    bool diagWriteMem(uint32_t targAddr, const void *buf, uint32_t len) { return fDiag ? fDiag->writeMem(targAddr, buf, len) : false; }

private:
    CEManager() = default;
    ~CEManager() = default;
    CEManager(const CEManager &) = delete;
    CEManager &operator=(const CEManager &) = delete;

    volatile uint32_t *fBar0 = nullptr;
    CECopyPair *fCtrl = nullptr;
    CECopyPair *fWmi  = nullptr;
    CEDiag     *fDiag = nullptr;
};

} // namespace qca

#endif
