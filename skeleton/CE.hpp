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

#include <libkern/c++/OSObject.h>
#include <IOKit/IOLib.h>
#include <stdint.h>

struct CEDescriptor {
    volatile uint32_t addr;
    volatile uint16_t nbytes;
    volatile uint16_t flags;
} __attribute__((packed));

static_assert(sizeof(CEDescriptor) == 8, "ce_desc must be 8 bytes");

static const uint32_t kQCAPollStep_us       = 50;
static const uint32_t kQCAExchangeTimeout_ms = 10000;

namespace qca {

class CopyEngine {
public:

    static CopyEngine *create(volatile uint32_t *bar0);
    void destroy();

    bool init();

    bool send(const void *buf, uint32_t len);

    // Target->host exchange, ath10k ordering (pci.c:2155-2176):

    bool postRecv();
    bool recvWait(uint32_t timeoutMs);

    // the response is not needed BEFORE the send — exchanges must use the

    bool recvPolling(uint32_t timeoutMs);

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

    void    *fRegionCpu  = nullptr;
    uint64_t fRegionPhys = 0;
    uint32_t fRegionSize = 0;

    // Ring geometry (ce.h:289-290: nentries must be a power of 2).
    static const uint32_t kRingN    = 16;
    static const uint32_t kRingMask = kRingN - 1;

    static const uint32_t kTxBufSz  = 256;
    static const uint32_t kRxBufSz  = 2048;

    // so the buffers never need to grow.)
    static const uint32_t kAlign    = 4096;

    CEDescriptor *fSrcDesc = nullptr;
    CEDescriptor *fDstDesc = nullptr;
    uint8_t *fTxCpu = nullptr;
    uint8_t *fRxCpu = nullptr;
    uint64_t fTxPhys = 0;
    uint64_t fRxPhys = 0;

    uint32_t fSrcWrite = 0;
    uint32_t fDstWrite = 0;
    uint32_t fSrcSw    = 0;
    uint32_t fDstSw    = 0;
    uint32_t fRxNbytes = 0;
    uint32_t fPostedIndex = 0;
};

}

#endif
