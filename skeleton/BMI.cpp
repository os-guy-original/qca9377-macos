/*
 * BMI.cpp — Boot Module Interface protocol for QCA9377 (M2+M3).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k bmi.c/pci.c (see BMI.hpp for the full
 * attribution and pin).
 *
 * Exchange shapes (pci.c:2106-2193):
 *   with response:    postRecv(CE1) -> send(CE0) -> recvWait(CE1)
 *   send-only:        send(CE0), wait SRRI consumes it; no CE1 traffic
 * Command framing (bmi.c:48-78): u32 id + per-command payload.
 * Response framing (bmi.h:161-175): payload only, no id prefix.
 */

#include "BMI.hpp"
#include <IOKit/IOLib.h>

namespace qca {

// ------------------------------------------------------------------
// Response exchange: postRecv -> send -> recvWait (ath10k ordering).
// ------------------------------------------------------------------
bool Bmi::exchangeWait(const void *cmd, uint32_t cmdLen,
                       void *resp, uint32_t respLen, uint32_t *gotLen)
{
    if (fDoneSent) {
        IOLog("QCA9377-BMI: command disallowed after BMI_DONE\n");
        return false;
    }
    if (!fCe->postRecv()) return false;
    if (!fCe->send(cmd, cmdLen)) return false;
    if (!fCe->recvWait(kQCAExchangeTimeout_ms)) return false;
    if (fCe->rxNbytes() < respLen) {
        IOLog("QCA9377-BMI: short response (%u < %u)\n",
              fCe->rxNbytes(), respLen);
        return false;
    }
    bcopy(fCe->rxBuf(), resp, respLen);
    if (gotLen) *gotLen = fCe->rxNbytes();
    return true;
}

// ------------------------------------------------------------------
// M2: GET_TARGET_INFO
// ------------------------------------------------------------------
bool Bmi::getTargetInfo()
{
    if (!fCe || !fCe->txBuf() || !fCe->rxBuf())
        return false;

    BmiCmdGetTargetInfo cmd;
    cmd.id = kBmiGetTargetInfo;   // 4-byte command (bmi.h:128-129)

    BmiRespGetTargetInfo resp;
    if (!exchangeWait(&cmd, sizeof(cmd), &resp, sizeof(resp), nullptr))
        return false;

    fTargetVersion = resp.version;
    fTargetType    = resp.type;
    logTargetVersion(resp.version);
    return true;
}

void Bmi::logTargetVersion(uint32_t version)
{
    // Linux ground truth on this card: "target 0x05020001" —
    // QCA9377_HW_1_1_DEV_VERSION (hw.h:60). 0x0502 high half = qca6174
    // family target (docs/REFERENCE-TRACE.md).
    const char *family = "unknown";
    if ((version & 0xffff0000) == 0x05020000)
        family = "qca6174-family (QCA9377)";
    IOLog("QCA9377-BMI: TARGET INFO version=0x%08x [%s]\n", version, family);
}

// ------------------------------------------------------------------
// M3: READ_MEMORY (bmi.c:155-196)
// ------------------------------------------------------------------
bool Bmi::readMemory(uint32_t addr, void *out, uint32_t len)
{
    if (!out || len == 0 || len > kBmiMaxDataSize)
        return false;

    BmiCmdReadMemory cmd;
    cmd.id   = kBmiReadMemory;
    cmd.addr = addr;
    cmd.len  = len;

    BmiRespReadMemory resp;
    uint32_t got = 0;
    if (!exchangeWait(&cmd, sizeof(cmd), &resp, len, &got))
        return false;
    bcopy(resp.payload, out, len);
    return true;
}

// ------------------------------------------------------------------
// M3: WRITE_MEMORY (bmi.c:260-313)
// ------------------------------------------------------------------
bool Bmi::writeMemory(uint32_t addr, const void *buf, uint32_t len)
{
    if (!buf || len == 0)
        return false;

    // Command buffer: id + addr + len + up to 256B payload.
    struct {
        uint32_t id;
        uint32_t addr;
        uint32_t len;
        uint8_t  payload[kBmiMaxDataSize];
    } cmd;
    const uint32_t hdrLen = 12;

    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        uint32_t tx = len < kBmiMaxDataSize ? len : kBmiMaxDataSize;
        bcopy(p, cmd.payload, tx);
        uint32_t padded = (tx + 3) & ~3u;      // roundup(tx, 4), bmi.c:289
        cmd.id   = kBmiWriteMemory;
        cmd.addr = addr;
        cmd.len  = padded;
        if (!fCe->send(&cmd, hdrLen + padded)) {
            IOLog("QCA9377-BMI: write_memory @0x%08x failed\n", addr);
            return false;
        }
        addr  += tx;                            // advance by real length
        p     += tx;
        len   -= tx;                            // roundup fixup, bmi.c:308
    }
    return true;
}

// ------------------------------------------------------------------
// M3: 32-bit HI accessors (bmi.h:242-269)
// ------------------------------------------------------------------
bool Bmi::read32(uint32_t itemOffset, uint32_t *val)
{
    // QCA988X_HOST_INTEREST_ADDRESS (targaddrs.h:28) + struct offset.
    static const uint32_t kHostInterestAddress = 0x00400800;
    return readMemory(kHostInterestAddress + itemOffset, val, 4);
}

bool Bmi::write32(uint32_t itemOffset, uint32_t val)
{
    static const uint32_t kHostInterestAddress = 0x00400800;
    return writeMemory(kHostInterestAddress + itemOffset, &val, 4);
}

// ------------------------------------------------------------------
// M3: EXECUTE (bmi.c:316-347)
// ------------------------------------------------------------------
bool Bmi::execute(uint32_t addr, uint32_t param, uint32_t *result)
{
    BmiCmdExecute cmd;
    cmd.id    = kBmiExecute;
    cmd.addr  = addr;
    cmd.param = param;

    BmiRespExecute resp;
    if (!exchangeWait(&cmd, sizeof(cmd), &resp, sizeof(resp), nullptr))
        return false;
    if (result) *result = resp.result;
    return true;
}

// ------------------------------------------------------------------
// M3: LZ fast download (bmi.c:432-495)
// ------------------------------------------------------------------
bool Bmi::fastDownload(uint32_t addr, const void *buf, uint32_t len)
{
    if (!buf || len == 0)
        return false;

    // 1. LZ_STREAM_START(addr) — send-only (bmi.c:444-450).
    BmiCmdLzStreamStart start;
    start.id   = kBmiLzStreamStart;
    start.addr = addr;
    if (!fCe->send(&start, sizeof(start))) {
        IOLog("QCA9377-BMI: lz_stream_start(0x%08x) failed\n", addr);
        return false;
    }

    // 2. LZ_DATA chunks (bmi.c:479-483): the ALIGNED head only
    // (rounddown(length,4)); the ragged tail is sent separately as a
    // zero-padded 4-byte word below — exactly as ath10k does, otherwise
    // the last bytes would be transferred twice.
    struct {
        uint32_t id;
        uint32_t len;
        uint8_t  payload[kBmiMaxDataSize];
    } cmd;
    const uint32_t hdrLen = 8;
    const uint32_t chunkCap = kBmiMaxDataSize - hdrLen;   // 248

    const uint8_t *p = (const uint8_t *)buf;
    const uint32_t tail = len & 3;
    uint32_t remaining = len - tail;
    while (remaining > 0) {
        uint32_t tx = remaining < chunkCap ? remaining : chunkCap;
        bcopy(p, cmd.payload, tx);
        cmd.id  = kBmiLzData;
        cmd.len = tx;
        if (!fCe->send(&cmd, hdrLen + tx)) {
            IOLog("QCA9377-BMI: lz_data failed at %u/%u\n",
                  len - remaining, len);
            return false;
        }
        p += tx;
        remaining -= tx;
    }

    // 3. Zero-padded trailer so the last word lands 4-aligned
    // (bmi.c:474-483; only when length % 4 != 0).
    if (tail) {
        uint8_t trailer[4] = {0, 0, 0, 0};
        bcopy(p, trailer, tail);   // p == buf + head now
        cmd.id  = kBmiLzData;
        cmd.len = 4;
        bcopy(trailer, cmd.payload, 4);
        if (!fCe->send(&cmd, hdrLen + 4)) {
            IOLog("QCA9377-BMI: lz_data trailer failed\n");
            return false;
        }
    }

    // 4. LZ_STREAM_START(0): close the stream, flush target caches
    // (bmi.c:489-494).
    BmiCmdLzStreamStart flush;
    flush.id   = kBmiLzStreamStart;
    flush.addr = 0;
    if (!fCe->send(&flush, sizeof(flush))) {
        IOLog("QCA9377-BMI: lz_stream_start(0) flush failed\n");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------
// M3: BMI_DONE (bmi.c:103-127)
// ------------------------------------------------------------------
bool Bmi::done()
{
    BmiCmdDone cmd;
    cmd.id = kBmiDone;
    if (!fCe->send(&cmd, sizeof(cmd))) {
        IOLog("QCA9377-BMI: BMI_DONE send failed\n");
        return false;
    }
    fDoneSent = true;
    IOLog("QCA9377-BMI: BMI_DONE sent\n");
    return true;
}

} // namespace qca
