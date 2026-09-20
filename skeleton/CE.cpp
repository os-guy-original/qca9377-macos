/*
 * CE.cpp — Copy Engine ring management for QCA9377 (M2).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k ce.c/ce.h (see CE.hpp for the full
 * attribution and pin).
 *
 * Ring programming, register-for-register (qcax_ce_regs, hw.c:462-476;
 * CE base ath10k_ce_base_address, ce.h:341; qca6174 bases hw.c:52-59):
 *
 *   SRC ring (CE0):  base+0x00 = ring bus addr (sr_base_addr_lo)
 *                    base+0x04 = nentries      (sr_size_addr)
 *                    base+0x3c = host write index doorbell (sr_wr_index_addr)
 *                    base+0x44 = target read index (current_srri_addr) [RO]
 *   DST ring (CE1):  base+0x08 = ring bus addr (dr_base_addr_lo)
 *                    base+0x0c = nentries      (dr_size_addr)
 *                    base+0x40 = host write index doorbell (dst_wr_index_addr)
 *                    base+0x48 = target completion index (current_drri_addr)[RO]
 *
 * Descriptor DMA semantics: the engine DMAs descriptors to/from system RAM
 * (ath10k_ce_init_src_ring memsets the host ring and hands the bus address
 * to the engine — ce.c:1369-1384). We use one physically-contiguous
 * IOMallocContiguous region, write descriptors with OSSynchronizeIO() fences, and
 * re-read them from RAM on completion (ce.c:756-775).
 */

#include "CE.hpp"
#include "QCA9377Driver.hpp"

namespace qca {

static const uint32_t kCESRBaseLo    = 0x00;
static const uint32_t kCESRSize      = 0x04;
static const uint32_t kCEDRBaseLo    = 0x08;
static const uint32_t kCEDRSize      = 0x0c;
static const uint32_t kCESRWrIndex   = 0x3c;
static const uint32_t kCEDSTWrIndex  = 0x40;
static const uint32_t kCECurrentSRRI = 0x44;
static const uint32_t kCECurrentDRRI = 0x48;

static const uint32_t kCE0Base = 0x00034400;
static const uint32_t kCE1Base = 0x00034800;

CopyEngine::CopyEngine() = default;
CopyEngine::~CopyEngine() { freeRegion(); }

CopyEngine *CopyEngine::create(volatile uint32_t *bar0)
{
    CopyEngine *ce = new CopyEngine();
    if (!ce)
        return nullptr;
    ce->fBar0 = bar0;
    IOLog("QCA9377-CE: created\n");
    return ce;
}

void CopyEngine::destroy()
{
    delete this;
}

bool CopyEngine::allocRegion()
{
    const uint32_t ringBytes = kRingN * (uint32_t)sizeof(CEDescriptor);
    const uint32_t ringAligned = (ringBytes + kAlign - 1) & ~(kAlign - 1);
    const uint32_t txAligned   = (kTxBufSz + kAlign - 1) & ~(kAlign - 1);
    const uint32_t rxAligned   = (kRxBufSz + kAlign - 1) & ~(kAlign - 1);
    fRegionSize = 2 * ringAligned + txAligned + rxAligned;

    fRegionCpu = IOMallocContiguous(fRegionSize, kAlign, &fRegionPhys);
    if (!fRegionCpu || !fRegionPhys) {
        IOLog("QCA9377-CE: IOMallocContiguous(%u) failed\n", fRegionSize);
        fRegionCpu = nullptr;
        return false;
    }

    uint8_t *cpu = (uint8_t *)fRegionCpu;
    fSrcDesc = (CEDescriptor *)cpu;
    fDstDesc = (CEDescriptor *)(cpu + ringAligned);
    fTxCpu   = cpu + 2 * ringAligned;
    fRxCpu   = fTxCpu + txAligned;
    fTxPhys  = fRegionPhys + 2 * ringAligned;
    fRxPhys  = fTxPhys + txAligned;

    IOLog("QCA9377-CE: region phys=0x%llx size=%u "
          "src@0x%llx dst@0x%llx tx@0x%llx rx@0x%llx\n",
          fRegionPhys, fRegionSize,
          fRegionPhys, fRegionPhys + ringAligned, fTxPhys, fRxPhys);
    return true;
}

void CopyEngine::freeRegion()
{
    if (fRegionCpu) {
        IOFreeContiguous(fRegionCpu, fRegionSize);
        fRegionCpu = nullptr;
        fRegionPhys = 0;
    }
}

bool CopyEngine::init()
{
    if (!fBar0 || !allocRegion())
        return false;

    const uint32_t ringAligned =
        ((kRingN * (uint32_t)sizeof(CEDescriptor)) + kAlign - 1) & ~(kAlign - 1);
    const uint64_t srcBus = fRegionPhys;
    const uint64_t dstBus = fRegionPhys + ringAligned;

    fSrcSw    = read32(kCE0Base + kCECurrentSRRI) & kRingMask;
    fSrcWrite = read32(kCE0Base + kCESRWrIndex)   & kRingMask;
    fDstSw    = read32(kCE1Base + kCECurrentDRRI) & kRingMask;
    fDstWrite = read32(kCE1Base + kCEDSTWrIndex)  & kRingMask;

    write32(kCE0Base + kCESRBaseLo, (uint32_t)srcBus);
    write32(kCE0Base + kCESRSize,   kRingN);
    write32(kCE1Base + kCEDRBaseLo, (uint32_t)dstBus);
    write32(kCE1Base + kCEDRSize,   kRingN);
    OSSynchronizeIO();

    IOLog("QCA9377-CE: init sr@0x%llx dr@0x%llx n=%u "
          "(hw idx src w=%u r=%u dst w=%u r=%u)\n",
          srcBus, dstBus, kRingN, fSrcWrite, fSrcSw, fDstWrite, fDstSw);
    return true;
}

bool CopyEngine::send(const void *buf, uint32_t len)
{
    if (len == 0 || len > kTxBufSz || len > 0xFFFF) {
        IOLog("QCA9377-CE: send bad len %u\n", len);
        return false;
    }

    const uint32_t deadline = kQCAExchangeTimeout_ms * 1000 / kQCAPollStep_us;
    uint32_t srri = read32(kCE0Base + kCECurrentSRRI) & kRingMask;
    for (uint32_t i = 0; i < deadline && srri != fSrcWrite; i++) {
        IODelay(kQCAPollStep_us);
        srri = read32(kCE0Base + kCECurrentSRRI) & kRingMask;
    }
    if (srri != fSrcWrite) {
        IOLog("QCA9377-CE: send pre-wait timeout (SRRI=%u w=%u)\n",
              srri, fSrcWrite);
        return false;
    }

    bcopy(buf, fTxCpu, len);
    OSSynchronizeIO();

    CEDescriptor d;
    d.addr   = (uint32_t)fTxPhys;
    d.nbytes = (uint16_t)len;
    d.flags  = 0;
    fSrcDesc[fSrcWrite] = d;
    OSSynchronizeIO();

    fSrcWrite = (fSrcWrite + 1) & kRingMask;
    write32(kCE0Base + kCESRWrIndex, fSrcWrite);
    OSSynchronizeIO();

    srri = read32(kCE0Base + kCECurrentSRRI) & kRingMask;
    for (uint32_t i = 0; i < deadline && srri != fSrcWrite; i++) {
        IODelay(kQCAPollStep_us);
        srri = read32(kCE0Base + kCECurrentSRRI) & kRingMask;
    }
    if (srri != fSrcWrite) {
        IOLog("QCA9377-CE: send completion timeout (SRRI=%u w=%u)\n",
              srri, fSrcWrite);
        return false;
    }
    return true;
}

// MUST be called before the corresponding send() — ath10k posts rx first

bool CopyEngine::postRecv()
{
    CEDescriptor d;
    d.addr   = (uint32_t)fRxPhys;
    d.nbytes = 0;
    d.flags  = 0;
    fDstDesc[fDstWrite] = d;
    OSSynchronizeIO();

    fPostedIndex = fDstWrite;
    fDstWrite = (fDstWrite + 1) & kRingMask;
    write32(kCE1Base + kCEDSTWrIndex, fDstWrite);
    OSSynchronizeIO();
    return true;
}

// race).
bool CopyEngine::recvWait(uint32_t timeoutMs)
{
    const uint32_t posted = fPostedIndex;
    const uint32_t deadline = timeoutMs * 1000 / kQCAPollStep_us;
    uint32_t drri = read32(kCE1Base + kCECurrentDRRI) & kRingMask;
    for (uint32_t i = 0; i < deadline && drri == fDstSw; i++) {
        IODelay(kQCAPollStep_us);
        drri = read32(kCE1Base + kCECurrentDRRI) & kRingMask;
    }
    if (drri == fDstSw) {
        IOLog("QCA9377-CE: recv timeout (DRRI=%u sw=%u)\n", drri, fDstSw);
        return false;
    }

    CEDescriptor d = fDstDesc[posted];
    if (d.nbytes == 0) {
        IOLog("QCA9377-CE: DRRI moved but nbytes==0 (race; ce.c:771-776)\n");
        return false;
    }
    fRxNbytes = d.nbytes;
    fDstSw = (fDstSw + 1) & kRingMask;
    OSSynchronizeIO();
    return true;
}

bool CopyEngine::recvPolling(uint32_t timeoutMs)
{
    if (!postRecv()) return false;
    return recvWait(timeoutMs);
}

}
