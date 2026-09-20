/*
 * CE.cpp — Copy Engine ring management for QCA9377 (M2/M4).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k ce.c/ce.h (see CE.hpp for the full
 * attribution and pin).
 *
 * Ring programming, register-for-register (qcax_ce_regs, hw.c:462-476;
 * CE base ath10k_ce_base_address, ce.h:341; qca6174 bases hw.c:52-59):
 *
 *   SRC ring:  base+0x00 sr_base_addr_lo, base+0x04 sr_size
 *              base+0x3c sr_wr_index doorbell, base+0x44 SRRI [RO]
 *   DST ring:  base+0x08 dr_base_addr_lo, base+0x0c dr_size
 *              base+0x40 dst_wr_index doorbell, base+0x48 DRRI [RO]
 *
 * Descriptor DMA: engine DMAs descriptors to/from system RAM; host writes
 * descriptors with OSSynchronizeIO() fences and re-reads them on completion.
 * TRAP: descriptors are 32-bit bus addresses — all DMA memory must be
 * allocated with a 32-bit physical mask (IOBufferMemoryDescriptor with
 * kIOMemoryPhysicallyContiguous + physMask), or init() fails here.
 */

#include "CE.hpp"
#include <IOKit/IOMemoryDescriptor.h>

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
static const uint32_t kCEStride = 0x400;
static inline uint32_t ceBase(uint32_t ce) { return kCE0Base + kCEStride * ce; }

CECopyPair::CECopyPair(volatile uint32_t *bar0, uint32_t srcCe, uint32_t dstCe)
    : fBar0(bar0), fSrcCe(srcCe), fDstCe(dstCe) {}

bool CECopyPair::allocRegion()
{
    const uint32_t ringBytes    = kRingN * (uint32_t)sizeof(CEDescriptor);
    const uint32_t ringAligned  = (ringBytes + kAlign - 1) & ~(kAlign - 1);
    const uint32_t txAligned    = (kTxBufSz + kAlign - 1) & ~(kAlign - 1);
    const uint32_t rxAligned    = (kRxBufSz + kAlign - 1) & ~(kAlign - 1);
    fRegionSize = 2 * ringAligned + txAligned + rxAligned;

    // TRAP: CE descriptors carry 32-bit bus addresses. Constrain the
    // allocation to <4G via the physical mask (10.4 fw consumes what the
    // engine DMAs; any 64-bit address here corrupts silently).
    IOBufferMemoryDescriptor *md =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut,
            fRegionSize, 0xFFFFFFFFULL);
    if (!md) {
        IOLog("QCA9377-CE%u/%u: buffer alloc failed\n", fSrcCe, fDstCe);
        return false;
    }
    if (md->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        IOLog("QCA9377-CE%u/%u: prepare failed\n", fSrcCe, fDstCe);
        md->release();
        return false;
    }

    fRegionCpu  = (void *)md->getBytesNoCopy();
    IOByteCount segLen = 0;
    uint64_t phys = md->getPhysicalSegment64(0, &segLen);
    if (!phys || (phys & ~0xFFFFFFFFULL)) {
        IOLog("QCA9377-CE%u/%u: phys 0x%llx exceeds 32-bit mask\n",
              fSrcCe, fDstCe, phys);
        md->complete();
        md->release();
        fRegionCpu = nullptr;
        return false;
    }
    fRegionPhys = phys;
    fMd = md;                       // keep for complete()/release()
    fMdPrepared = true;

    uint8_t *cpu = (uint8_t *)fRegionCpu;
    fSrcDesc = (CEDescriptor *)cpu;
    fDstDesc = (CEDescriptor *)(cpu + ringAligned);
    fTxCpu   = cpu + 2 * ringAligned;
    fRxCpu   = fTxCpu + txAligned;
    fTxPhys  = fRegionPhys + 2 * ringAligned;
    fRxPhys  = fTxPhys + txAligned;

    IOLog("QCA9377-CE%u/%u: region phys=0x%llx size=%u\n",
          fSrcCe, fDstCe, fRegionPhys, fRegionSize);
    return true;
}

void CECopyPair::freeRegion()
{
    if (fMd) {
        if (fMdPrepared) { fMd->complete(); fMdPrepared = false; }
        fMd->release();
        fMd = nullptr;
    }
    fRegionCpu = nullptr;
    fRegionPhys = 0;
}

bool CECopyPair::init()
{
    if (!fBar0 || !allocRegion())
        return false;

    const uint32_t ringAligned =
        ((kRingN * (uint32_t)sizeof(CEDescriptor)) + kAlign - 1) & ~(kAlign - 1);
    const uint64_t srcBus = fRegionPhys;
    const uint64_t dstBus = fRegionPhys + ringAligned;

    // Seed indices from current hardware state (previous OS may leave the
    // rings programmed — warm-boot case).
    fSrcSw    = read32(ceBase(fSrcCe) + kCECurrentSRRI) & kRingMask;
    fSrcWrite = read32(ceBase(fSrcCe) + kCESRWrIndex)   & kRingMask;
    fDstSw    = read32(ceBase(fDstCe) + kCECurrentDRRI) & kRingMask;
    fDstWrite = read32(ceBase(fDstCe) + kCEDSTWrIndex)  & kRingMask;

    write32(ceBase(fSrcCe) + kCESRBaseLo, (uint32_t)srcBus);
    write32(ceBase(fSrcCe) + kCESRSize,   kRingN);
    write32(ceBase(fDstCe) + kCEDRBaseLo, (uint32_t)dstBus);
    write32(ceBase(fDstCe) + kCEDRSize,   kRingN);
    OSSynchronizeIO();

    IOLog("QCA9377-CE%u/%u: init sr@0x%llx dr@0x%llx n=%u (hw idx sw=%u w=%u)\n",
          fSrcCe, fDstCe, srcBus, dstBus, kRingN, fSrcSw, fSrcWrite);
    return true;
}

bool CECopyPair::send(const void *buf, uint32_t len)
{
    if (len == 0 || len > kTxBufSz || len > 0xFFFF) {
        IOLog("QCA9377-CE%u: send bad len %u\n", fSrcCe, len);
        return false;
    }

    // One in flight: wait until the engine consumed everything written.
    const uint32_t deadline = kQCAExchangeTimeout_ms * 1000 / kQCAPollStep_us;
    uint32_t srri = read32(ceBase(fSrcCe) + kCECurrentSRRI) & kRingMask;
    for (uint32_t i = 0; i < deadline && srri != fSrcWrite; i++) {
        IODelay(kQCAPollStep_us);
        srri = read32(ceBase(fSrcCe) + kCECurrentSRRI) & kRingMask;
    }
    if (srri != fSrcWrite) {
        IOLog("QCA9377-CE%u: send pre-wait timeout (SRRI=%u w=%u)\n",
              fSrcCe, srri, fSrcWrite);
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
    write32(ceBase(fSrcCe) + kCESRWrIndex, fSrcWrite);
    OSSynchronizeIO();

    srri = read32(ceBase(fSrcCe) + kCECurrentSRRI) & kRingMask;
    for (uint32_t i = 0; i < deadline && srri != fSrcWrite; i++) {
        IODelay(kQCAPollStep_us);
        srri = read32(ceBase(fSrcCe) + kCECurrentSRRI) & kRingMask;
    }
    if (srri != fSrcWrite) {
        IOLog("QCA9377-CE%u: send completion timeout (SRRI=%u w=%u)\n",
              fSrcCe, srri, fSrcWrite);
        return false;
    }
    return true;
}

bool CECopyPair::postRecv()
{
    CEDescriptor d;
    d.addr   = (uint32_t)fRxPhys;
    d.nbytes = 0;
    d.flags  = 0;
    fDstDesc[fDstWrite] = d;
    OSSynchronizeIO();

    fPostedIndex = fDstWrite;
    fDstWrite = (fDstWrite + 1) & kRingMask;
    write32(ceBase(fDstCe) + kCEDSTWrIndex, fDstWrite);
    OSSynchronizeIO();
    return true;
}

bool CECopyPair::recvWait(uint32_t timeoutMs)
{
    const uint32_t posted = fPostedIndex;
    const uint32_t deadline = timeoutMs * 1000 / kQCAPollStep_us;
    uint32_t drri = read32(ceBase(fDstCe) + kCECurrentDRRI) & kRingMask;
    for (uint32_t i = 0; i < deadline && drri == fDstSw; i++) {
        IODelay(kQCAPollStep_us);
        drri = read32(ceBase(fDstCe) + kCECurrentDRRI) & kRingMask;
    }
    if (drri == fDstSw) {
        IOLog("QCA9377-CE%u: recv timeout (DRRI=%u sw=%u)\n", fDstCe, drri, fDstSw);
        return false;
    }

    CEDescriptor d = fDstDesc[posted];
    if (d.nbytes == 0) {
        // Race guard (ce.c:771-776): DRRI moved before the descriptor DMA
        // landed. Treat as not-done.
        IOLog("QCA9377-CE%u: DRRI moved but nbytes==0 (race)\n", fDstCe);
        return false;
    }
    fRxNbytes = d.nbytes;
    fDstSw = (fDstSw + 1) & kRingMask;
    OSSynchronizeIO();
    return true;
}

// ---------------------------------------------------------------------------
// CEManager
// ---------------------------------------------------------------------------

CEManager *CEManager::create(volatile uint32_t *bar0)
{
    CEManager *m = new CEManager();
    if (!m) return nullptr;
    m->fBar0 = bar0;
    IOLog("QCA9377-CE: manager created\n");
    return m;
}

void CEManager::destroy()
{
    if (fCtrl) { fCtrl->teardown(); delete fCtrl; fCtrl = nullptr; }
    if (fWmi)  { fWmi->teardown();  delete fWmi;  fWmi  = nullptr; }
    delete this;
}

bool CEManager::init()
{
    fCtrl = new CECopyPair(fBar0, 0, 1);   // CE0 host->t, CE1 t->host
    if (!fCtrl) return false;
    if (!fCtrl->init()) {
        delete fCtrl; fCtrl = nullptr;
        return false;
    }
    return true;
}

bool CEManager::initWmi()
{
    if (fWmi) return true;                 // already up
    fWmi = new CECopyPair(fBar0, 3, 2);    // CE3 host->t, CE2 t->host
    if (!fWmi) return false;
    if (!fWmi->init()) {
        delete fWmi; fWmi = nullptr;
        IOLog("QCA9377-CE: WMI pair init failed\n");
        return false;
    }
    return true;
}

} // namespace qca
