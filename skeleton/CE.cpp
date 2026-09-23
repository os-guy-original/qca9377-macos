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
 * DMA allocation: IOBufferMemoryDescriptor::inTaskWithPhysicalMask with a
 * 32-bit physical mask (the Recovery kernelcache lacks IOMallocContiguous;
 * IODMACommand keeps this allocation mappable without getPhysicalSegment64,
 * which modern kernels removed).
 */

#include "CE.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <libkern/c++/OSObject.h>

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

// CEDiag (CE7) needs the PCIe-local window + CORE_CTRL offset for the
// targ->CE address conversion (qca6174_targ_cpu_to_ce_addr). Values mirror
// QCA9377Driver.hpp — per-TU constants are this file's convention.
static const uint32_t kPcieLocalBase   = 0x00080000;
static const uint32_t kCoreCtrlOffset  = 0x00000000;
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

    // 32-bit-masked contiguous DMA (Recovery-KC-compatible pattern, after
    // itlwm hal_iwm io.cpp): BMD with mask, prepare, IODMACommand over it,
    // then gen64IOVMSegments yields the bus address.
    fBmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache,
        fRegionSize,
        0x00000000FFFFFFFFULL);
    if (!fBmd) {
        IOLog("QCA9377-CE%u/%u: inTaskWithPhysicalMask(%u) failed\n",
              fSrcCe, fDstCe, fRegionSize);
        goto fail;
    }
    if (fBmd->prepare() != kIOReturnSuccess) {
        IOLog("QCA9377-CE%u/%u: BMD prepare failed\n", fSrcCe, fDstCe);
        goto fail;
    }
    fBmdPrepared = true;
    fRegionCpu = fBmd->getBytesNoCopy();
    if (!fRegionCpu) {
        IOLog("QCA9377-CE%u/%u: buffer has no kernel mapping\n", fSrcCe, fDstCe);
        goto fail;
    }
    memset(fRegionCpu, 0, fRegionSize);

    fDma = IODMACommand::withSpecification(
        kIODMACommandOutputHost64,
        32,               // numAddressBits: hardware sees 32-bit bus
        0,                // maxSegmentSize: unlimited
        IODMACommand::kMapped,
        0,                // maxTransferSize
        1,                // alignment
        nullptr, nullptr);
    if (!fDma) {
        IOLog("QCA9377-CE%u/%u: IODMACommand::withSpecification failed\n",
              fSrcCe, fDstCe);
        goto fail;
    }
    if (fDma->setMemoryDescriptor(fBmd, true) != kIOReturnSuccess) {
        IOLog("QCA9377-CE%u/%u: setMemoryDescriptor failed\n", fSrcCe, fDstCe);
        goto fail;
    }
    fDmaPrepared = true;

    {
        IODMACommand::Segment64 seg;
        uint64_t ofs = 0;
        uint32_t nseg = 1;
        if (fDma->gen64IOVMSegments(&ofs, &seg, &nseg) != kIOReturnSuccess
            || nseg == 0
            || seg.fLength < fRegionSize) {
            IOLog("QCA9377-CE%u/%u: no contiguous segment within 32 bits\n",
                  fSrcCe, fDstCe);
            goto fail;
        }
        fRegionPhys = seg.fIOVMAddr;
    }

    {
        uint8_t *cpu = (uint8_t *)fRegionCpu;
        fSrcDesc = (CEDescriptor *)cpu;
        fDstDesc = (CEDescriptor *)(cpu + ringAligned);
        fTxCpu   = cpu + 2 * ringAligned;
        fRxCpu   = fTxCpu + txAligned;
        fTxPhys  = fRegionPhys + 2 * ringAligned;
        fRxPhys  = fTxPhys + txAligned;
    }

    IOLog("QCA9377-CE%u/%u: region phys=0x%llx size=%u\n",
          fSrcCe, fDstCe, fRegionPhys, fRegionSize);
    return true;

fail:
    freeRegion();
    return false;
}

void CECopyPair::freeRegion()
{
    if (fDma) {
        if (fDmaPrepared) fDma->clearMemoryDescriptor(true);
        fDma->release();
        fDma = nullptr;
    }
    if (fBmd) {
        if (fBmdPrepared) fBmd->complete();
        fBmd->release();
        fBmd = nullptr;
    }
    fBmdPrepared = false;
    fDmaPrepared = false;
    fRegionCpu = nullptr;
    fRegionPhys = 0;
}

void CECopyPair::teardown()
{
    // Park both rings AND zero the descriptors before releasing DMA memory:
    // a zeroed sr_base with sr_size=0 keeps the engine from touching freed
    // RAM even if the target is mid-transfer (worst case: engine fault, not
    // host memory corruption).
    if (fBar0) {
        write32(ceBase(fSrcCe) + kCESRBaseLo, 0);
        write32(ceBase(fSrcCe) + kCESRSize, 0);
        write32(ceBase(fSrcCe) + kCESRWrIndex, 0);
        write32(ceBase(fDstCe) + kCEDRBaseLo, 0);
        write32(ceBase(fDstCe) + kCEDRSize, 0);
        write32(ceBase(fDstCe) + kCEDSTWrIndex, 0);
        OSSynchronizeIO();
    }
    if (fRegionCpu && fRegionSize) {
        // Poison descriptors so a stale DMA sweep reads len 0, not garbage.
        for (uint32_t i = 0; i < kRingN; i++) {
            fSrcDesc[i].addr = 0; fSrcDesc[i].nbytes = 0; fSrcDesc[i].flags = 0;
            fDstDesc[i].addr = 0; fDstDesc[i].nbytes = 0; fDstDesc[i].flags = 0;
        }
        OSSynchronizeIO();
    }
    freeRegion();
}

bool CECopyPair::init()
{
    if (!fBar0 || !allocRegion())
        return false;

    const uint32_t ringAligned =
        ((kRingN * (uint32_t)sizeof(CEDescriptor)) + kAlign - 1) & ~(kAlign - 1);
    const uint64_t srcBus = fRegionPhys;
    const uint64_t dstBus = fRegionPhys + ringAligned;

    // Start from zeroed indices, not a hardware-state restore: ring
    // *locations* are reprogrammed below anyway, so old index values would
    // point past our freshly seeded descriptors (postRecv would write at
    // fDstWrite against a DIFFERENT descriptor array, and recvWait would
    // consume stale entries). ath10k seeds from its own bookkeeping, which
    // we do not have across an OS boundary.
    fSrcSw    = 0;
    fSrcWrite = 0;
    fDstSw    = 0;
    fDstWrite = 0;

    // Heartbeat probe before wiring rings: a wedged/dead engine must fail
    // init here, not hang every later exchange for 10s each.
    {
        const uint32_t srri = read32(ceBase(fSrcCe) + kCECurrentSRRI);
        const uint32_t drri = read32(ceBase(fDstCe) + kCECurrentDRRI);
        if (srri == 0xffffffffu || drri == 0xffffffffu) {
            IOLog("QCA9377-CE%u/%u: engine reads 0xffffffff - dead or unmapped\n",
                  fSrcCe, fDstCe);
            return false;
        }
    }

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

    fDstWrite = (fDstWrite + 1) & kRingMask;
    write32(ceBase(fDstCe) + kCEDSTWrIndex, fDstWrite);
    OSSynchronizeIO();
    return true;
}

bool CECopyPair::recvWait(uint32_t timeoutMs)
{
    const uint32_t deadline = timeoutMs * 1000 / kQCAPollStep_us;

    // Poll the oldest unconsumed entry (FIFO at fDstSw), not a snapshot of
    // the write index: DRRI can run ahead of the descriptor landing, and
    // later posts must not skip the queue. One rx buffer is live per pair,
    // so the entry at fDstSw is the one this wait consumes.
    for (uint32_t i = 0; i < deadline; i++) {
        const uint32_t drri = read32(ceBase(fDstCe) + kCECurrentDRRI) & kRingMask;
        if (drri != fDstSw) {
            CEDescriptor d = fDstDesc[fDstSw];
            if (d.nbytes == 0) {
                // Race guard (ce.c:771-781): DRRI moved before the
                // descriptor DMA landed. Keep polling — do NOT consume,
                // do NOT fail (a stale-fail here permanently desyncs
                // every later exchange).
                IODelay(kQCAPollStep_us);
                continue;
            }
            fRxNbytes = d.nbytes;
            fDstSw = (fDstSw + 1) & kRingMask;
            OSSynchronizeIO();
            return true;
        }
        IODelay(kQCAPollStep_us);
    }

    IOLog("QCA9377-CE%u: recv timeout (DRRI=%u sw=%u)\n",
          fDstCe, read32(ceBase(fDstCe) + kCECurrentDRRI) & kRingMask, fDstSw);
    return false;
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
    if (fDiag) { delete fDiag; fDiag = nullptr; }
    if (fCtrl) { fCtrl->teardown(); delete fCtrl; fCtrl = nullptr; }
    if (fWmi)  { fWmi->teardown();  delete fWmi;  fWmi  = nullptr; }
    delete this;
}

bool CEManager::init()
{
    fCtrl = new CECopyPair(fBar0, 0, 1);   // CE0 host->t, CE1 t->host
    if (!fCtrl) return false;
    if (!fCtrl->init()) {
        delete fCtrl;
        fCtrl = nullptr;                   // NULL before delete: destroy()
        return false;                      // must not touch a freed object
    }
    return true;
}

bool CEManager::initWmi()
{
    if (fWmi) return true;                 // already up
    fWmi = new CECopyPair(fBar0, 3, 2);    // CE3 host->t, CE2 t->host
    if (!fWmi) return false;
    if (!fWmi->init()) {
        delete fWmi;
        fWmi = nullptr;                    // destroy() would double-free
        IOLog("QCA9377-CE: WMI pair init failed\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// CEDiag — CE7 diagnostic window (M2.5). Port of ath10k_pci_diag_read_mem
// (pci.c:897) / diag_write_mem (pci.c:1049) with the CE7-serves-both-
// directions model: descriptors carry raw addresses, the chip interconnect
// routes them to target memory.
// ---------------------------------------------------------------------------

uint32_t CEDiag::targToCeAddr(uint32_t addr)
{
    // qca6174_targ_cpu_to_ce_addr (pci.c): the CE-interconnect window key is
    // built from CORE_CTRL's wrap ID bits (0x7ff mask) and the >=0x100000
    // DRAM flag. CORE_CTRL read is PCIe-local, no wake needed (engine awake
    // during BMI phase).
    uint32_t val = (regRead32(kPcieLocalBase + kCoreCtrlOffset)
                   & 0x7ff) << 21;
    val |= ((addr >= 0x100000) ? 0x100000 : 0) | (addr & 0xfffff);
    return val;
}

bool CEDiag::xfer(uint32_t postAddr, uint32_t sendAddr, uint32_t len)
{
    const uint32_t ceBase    = kCE0Base + kCEStride * kDiagCe;
    const uint32_t deadline  = kWaitTimeout_us / kWaitStep_us;
    uint32_t       i;

    // dst ring: post the receiving descriptor (bounce for reads, target
    // address for writes)
    CEDescriptor d;
    d.addr   = postAddr;
    d.nbytes = 0;
    d.flags  = 0;
    fDstDesc[fDstWrite] = d;
    OSSynchronizeIO();
    fDstWrite = (fDstWrite + 1) & kRingMask;
    regWrite32(ceBase + kCEDSTWrIndex, fDstWrite);
    OSSynchronizeIO();

    // src ring: send descriptor (CE-address for reads, bounce for writes)
    d.addr   = sendAddr;
    d.nbytes = (uint16_t)len;
    d.flags  = 0;
    fSrcDesc[fSrcWrite] = d;
    OSSynchronizeIO();
    fSrcWrite = (fSrcWrite + 1) & kRingMask;
    regWrite32(ceBase + kCESRWrIndex, fSrcWrite);
    OSSynchronizeIO();

    // send completion: SRRI catches up to our write index
    for (i = 0; i < deadline; i++) {
        if ((regRead32(ceBase + kCECurrentSRRI) & kRingMask) == fSrcWrite)
            break;
        IODelay(kWaitStep_us);
    }
    if (i == deadline) {
        IOLog("QCA9377-CE7: send completion timeout (SRRI=%u w=%u)\n",
              regRead32(ceBase + kCECurrentSRRI) & kRingMask, fSrcWrite);
        return false;
    }
    // recv completion: DRRI moves past our consume index; verify the
    // completed descriptor's length matches (ath10k checks nbytes + cookie)
    for (i = 0; i < deadline; i++) {
        if ((regRead32(ceBase + kCECurrentDRRI) & kRingMask) != fDstSw)
            break;
        IODelay(kWaitStep_us);
    }
    if (i == deadline) {
        IOLog("QCA9377-CE7: recv completion timeout (DRRI=%u sw=%u)\n",
              regRead32(ceBase + kCECurrentDRRI) & kRingMask, fDstSw);
        return false;
    }
    CEDescriptor done = fDstDesc[fDstSw];
    fDstSw = (fDstSw + 1) & kRingMask;
    if (done.nbytes != (uint16_t)len) {
        IOLog("QCA9377-CE7: completed nbytes %u != %u\n", done.nbytes, len);
        return false;
    }
    return true;
}

bool CEDiag::readMem(uint32_t targAddr, void *out, uint32_t len)
{
    if (!fRegionCpu || len == 0 || len > kDiagMax) return false;

    const uint32_t ceAddr = targToCeAddr(targAddr);
    if (!xfer(fBouncePhys, ceAddr, len))          // dst=bounce, src=target
        return false;
    bcopy(fBounce, out, len);
    return true;
}

bool CEDiag::writeMem(uint32_t targAddr, const void *buf, uint32_t len)
{
    if (!fRegionCpu || len == 0 || len > kDiagMax) return false;

    const uint32_t ceAddr = targToCeAddr(targAddr);
    bcopy(buf, fBounce, len);
    OSSynchronizeIO();
    return xfer(ceAddr, fBouncePhys, len);       // dst=target, src=bounce
}

bool CEDiag::init()
{
    if (!fBar0 || !allocRegion())
        return false;

    const uint32_t ringAligned =
        ((kRingN * (uint32_t)sizeof(CEDescriptor)) + kAlign - 1) & ~(kAlign - 1);
    fSrcDesc    = (CEDescriptor *)fRegionCpu;
    fDstDesc    = (CEDescriptor *)((uint8_t *)fRegionCpu + ringAligned);
    fBounce     = (uint8_t *)fRegionCpu + 2 * ringAligned;
    fBouncePhys = fRegionPhys + 2 * ringAligned;

    // Same heartbeat probe as CECopyPair::init — fail fast on a dead engine.
    const uint32_t ceBase = kCE0Base + kCEStride * kDiagCe;
    const uint32_t srri = regRead32(ceBase + kCECurrentSRRI);
    const uint32_t drri = regRead32(ceBase + kCECurrentDRRI);
    if (srri == 0xffffffffu || drri == 0xffffffffu) {
        IOLog("QCA9377-CE7: engine reads 0xffffffff - dead or unmapped\n");
        return false;
    }

    regWrite32(ceBase + kCESRBaseLo, (uint32_t)fRegionPhys);
    regWrite32(ceBase + kCESRSize,   kRingN);
    regWrite32(ceBase + kCEDRBaseLo, (uint32_t)(fRegionPhys + ringAligned));
    regWrite32(ceBase + kCEDRSize,   kRingN);
    OSSynchronizeIO();
    IOLog("QCA9377-CE7: diag window ready sr@0x%llx dr@0x%llx\n",
          fRegionPhys, fRegionPhys + ringAligned);
    return true;
}

void CEDiag::teardown()
{
    const uint32_t ceBase = kCE0Base + kCEStride * kDiagCe;
    if (fBar0) {
        regWrite32(ceBase + kCESRBaseLo, 0);
        regWrite32(ceBase + kCESRSize, 0);
        regWrite32(ceBase + kCESRWrIndex, 0);
        regWrite32(ceBase + kCEDRBaseLo, 0);
        regWrite32(ceBase + kCEDRSize, 0);
        regWrite32(ceBase + kCEDSTWrIndex, 0);
    }
    freeRegion();
}

bool CEDiag::allocRegion()
{
    const uint32_t ringBytes   = kRingN * (uint32_t)sizeof(CEDescriptor);
    const uint32_t ringAligned = (ringBytes + kAlign - 1) & ~(kAlign - 1);
    const uint32_t bounceAligned = (kDiagMax + kAlign - 1) & ~(kAlign - 1);
    fRegionSize = 2 * ringAligned + bounceAligned;

    fBmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache,
        fRegionSize,
        0x00000000FFFFFFFFULL);
    if (!fBmd) {
        IOLog("QCA9377-CE7: inTaskWithPhysicalMask(%u) failed\n", fRegionSize);
        goto fail;
    }
    if (fBmd->prepare() != kIOReturnSuccess) {
        IOLog("QCA9377-CE7: BMD prepare failed\n");
        goto fail;
    }
    fBmdPrepared = true;
    fRegionCpu = fBmd->getBytesNoCopy();
    if (!fRegionCpu) goto fail;
    memset(fRegionCpu, 0, fRegionSize);

    fDma = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 32, 0,
        IODMACommand::kMapped, 0, 1, nullptr, nullptr);
    if (!fDma) {
        IOLog("QCA9377-CE7: IODMACommand::withSpecification failed\n");
        goto fail;
    }
    if (fDma->setMemoryDescriptor(fBmd, true) != kIOReturnSuccess) {
        IOLog("QCA9377-CE7: setMemoryDescriptor failed\n");
        goto fail;
    }
    fDmaPrepared = true;

    {
        IODMACommand::Segment64 seg;
        uint64_t ofs = 0;
        uint32_t nseg = 1;
        if (fDma->gen64IOVMSegments(&ofs, &seg, &nseg) != kIOReturnSuccess
            || nseg == 0 || seg.fLength < fRegionSize) {
            IOLog("QCA9377-CE7: no contiguous segment within 32 bits\n");
            goto fail;
        }
        fRegionPhys = seg.fIOVMAddr;
    }
    return true;

fail:
    freeRegion();
    return false;
}

void CEDiag::freeRegion()
{
    if (fDma) {
        if (fDmaPrepared) fDma->clearMemoryDescriptor(true);
        fDma->release();
        fDma = nullptr;
    }
    if (fBmd) {
        if (fBmdPrepared) fBmd->complete();
        fBmd->release();
        fBmd = nullptr;
    }
    fBmdPrepared = false;
    fDmaPrepared = false;
    fRegionCpu = nullptr;
    fBounce = nullptr;
    fBouncePhys = 0;
    fRegionPhys = 0;
    fRegionSize = 0;
}

} // namespace qca
