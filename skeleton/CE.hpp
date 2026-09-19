/*
 * CE.hpp — Copy Engine ring management for QCA9377 (M2).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k (drivers/net/wireless/ath/ath10k/ce.c,
 * ce.h), ISC-licensed: Copyright (c) Atheros Communications Inc., Copyright
 * (c) Qualcomm Atheros, Inc. Adapted per ISC terms with attribution.
 *
 * Derived from Linux v7.2.3 ath10k, sha256-pinned:
 * 8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03.
 *
 * M2 SCOPE: polling driver, one command in flight. 16-entry rings
 * (ath10k uses 512/2048 — ring sizes are host-written, see CE.cpp).
 * Plain C++ class: allocation via kernel new (libkmodc++), no OSObject
 * machinery — this object never crosses IOKit's registry or retainer.
 */

#ifndef QCA9377_CE_hpp
#define QCA9377_CE_hpp

#include <libkern/c++/OSObject.h>   // OSIncrementAtomic etc.
#include <IOKit/IOLib.h>            // IOMalloc/IOFree, IODelay, IOSync
#include <stdint.h>

// ce_desc: ce.h:44-48 (32-bit format; this chip is not target_64bit).
struct CEDescriptor {
    volatile uint32_t addr;    // LE bus address
    volatile uint16_t nbytes;  // LE
    volatile uint16_t flags;   // LE
} __attribute__((packed));

static_assert(sizeof(CEDescriptor) == 8, "ce_desc must be 8 bytes");

// Poll cadence + exchange timeout. BMI_COMMUNICATION_TIMEOUT_HZ is 1s in
// ath10k; 10s is a conservative first-hardware bound.
static const uint32_t kQCAPollStep_us       = 50;
static const uint32_t kQCAExchangeTimeout_ms = 10000;

namespace qca {

class CopyEngine {
public:
    // bar0: the driver's mapped BAR0 (LE 32-bit MMIO window). The driver
    // holds the target awake for the whole M2 session (SOC_WAKE stays set),
    // mirroring ath10k's wake-per-access without the per-access overhead.
    static CopyEngine *create(volatile uint32_t *bar0);
    void destroy();   // kernel-object teardown (IOFree etc.)

    // Allocate the coherent DMA region, program CE0 src / CE1 dst
    // registers, seed indices from the engine's current ones.
    bool init();

    // Host->target: copy payload into the DMA buffer, post one src
    // descriptor, doorbell, wait until SRRI consumes it
    // (send completion = SRRI read, ce.c:155-158).
    bool send(const void *buf, uint32_t len);

    // Post one recv buffer (doorbell CE1 dst write index), poll DRRI until
    // it moves past the posted slot, unpack via descriptor nbytes
    // (ce.c:756-775: nbytes==0 means not-yet-done).
    bool recvPolling(uint32_t timeoutMs);

    // Buffers (valid after init; rx data valid after recvPolling()==true).
    uint8_t *txBuf() { return fTxCpu; }
    uint8_t *rxBuf() { return fRxCpu; }
    uint32_t rxNbytes() const { return fRxNbytes; }

private:
    CopyEngine();
    ~CopyEngine();
    CopyEngine(const CopyEngine &) = delete;
    CopyEngine &operator=(const CopyEngine &) = delete;

    bool allocRegion();
    void freeRegion();

    uint32_t read32(uint32_t offset)  { return OSReadLittleInt32(fBar0, offset); }
    void     write32(uint32_t offset, uint32_t v) { OSWriteLittleInt32(fBar0, offset, v); }

    volatile uint32_t *fBar0 = nullptr;

    // One contiguous DMA region, 4KB-aligned sections:
    // [src ring][dst ring][tx buf][rx buf]
    void    *fRegionCpu  = nullptr;
    uint64_t fRegionPhys = 0;
    uint32_t fRegionSize = 0;

    // Ring geometry (ce.h:289-290: nentries must be a power of 2).
    static const uint32_t kRingN    = 16;
    static const uint32_t kRingMask = kRingN - 1; // CE_RING_IDX_* mask, ce.h:359-364
    static const uint32_t kTxBufSz  = 256;        // src_sz_max CE0, pci.c:122-127
    static const uint32_t kRxBufSz  = 2048;       // src_sz_max CE1, pci.c:129-134
    static const uint32_t kAlign    = 4096;

    CEDescriptor *fSrcDesc = nullptr;  // CE0 src ring (host-owned RAM)
    CEDescriptor *fDstDesc = nullptr;  // CE1 dst ring (host-owned RAM)
    uint8_t *fTxCpu = nullptr;
    uint8_t *fRxCpu = nullptr;
    uint64_t fTxPhys = 0;
    uint64_t fRxPhys = 0;

    uint32_t fSrcWrite = 0;  // CE0 host write index (doorbell value)
    uint32_t fDstWrite = 0;  // CE1 host write index (recv-post doorbell)
    uint32_t fSrcSw    = 0;  // CE0 host consumption index (SRRI shadow)
    uint32_t fDstSw    = 0;  // CE1 host consumption index (DRRI shadow)
    uint32_t fRxNbytes = 0;
};

} // namespace qca

#endif /* QCA9377_CE_hpp */
