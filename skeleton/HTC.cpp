/*
 * HTC.cpp — Host-Target Communications layer for QCA9377 (M4).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k htc.c (see HTC.hpp for the full
 * attribution and pin).
 */

#include "HTC.hpp"
#include <IOKit/IOLib.h>

namespace qca {

// ---- wire message structs (packed, LE) ------------------------------------

struct HtcMsgHdr16 {        // __le16 message_id prefix of every HTC message
    uint16_t messageId;
} __attribute__((packed));

struct HtcReady {           // HTC_READY (msg id 1)
    uint16_t creditCount;
    uint16_t creditSize;
    uint8_t  maxEndpoints;
    uint8_t  pad0;
} __attribute__((packed));

struct HtcReadyExt {        // extended ready
    HtcReady base;
    uint8_t  htcVersion;
    uint8_t  maxMsgsPerBundle;
    uint16_t reserved;
} __attribute__((packed));

struct HtcConnSvc {         // CONNECT_SERVICE (msg id 2)
    uint16_t serviceId;
    uint16_t flags;
    uint8_t  pad0;
    uint8_t  pad1;
} __attribute__((packed));

struct HtcConnSvcResp {     // CONNECT_SERVICE_RESP (msg id 3)
    uint16_t serviceId;
    uint8_t  status;
    uint8_t  eid;
    uint16_t maxMsgSize;
} __attribute__((packed));

struct HtcSetupCompleteExt { // SETUP_COMPLETE_EX (msg id 5)
    uint8_t  pad0;
    uint8_t  pad1;
    uint32_t flags;
    uint8_t  maxMsgsPerBundledRecv;
    uint8_t  pad2;
    uint8_t  pad3;
    uint8_t  pad4;
} __attribute__((packed));

struct HtcRecordHdr {       // trailer record: {u8 id, u8 len}
    uint8_t id;
    uint8_t len;
} __attribute__((packed));

// ---- helpers --------------------------------------------------------------

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

// ---- credit accounting ----------------------------------------------------

void Htc::applyCreditReport(const uint8_t *trailer, uint32_t trailerLen)
{
    // Trailer records: 4-byte header {id, len, pad0, pad1} (htc.h:216-221);
    // credit entries are 4 bytes each: {eid, credits, pad0, pad1}
    // (htc.h:223-228, ath10k_htc_process_credit_report htc.c:249).
    uint32_t off = 0;
    while (off + 2 <= trailerLen) {
        const uint8_t recId  = trailer[off];
        const uint8_t recLen = trailer[off + 1];
        off += 4;                            // full record header
        if (off + recLen > trailerLen)
            break;
        if (recId == kHtcRecordCredits && recLen >= 4) {
            for (uint32_t i = 0; i + 4 <= recLen; i += 4) {
                uint8_t eid     = trailer[off + i];
                uint8_t credits = trailer[off + i + 1];
                if (eid == fWmiEid)
                    fWmiCredits += credits;
            }
        }
        off += recLen;
    }
}

// ---- control path (CE0/CE1) ------------------------------------------------

bool Htc::sendCtrlFrame(const void *payload, uint32_t len)
{
    if (len > 256)
        return false;

    uint8_t frame[8 + 256];
    HtcFrameHdr *h = (HtcFrameHdr *)frame;
    h->eid       = 0;                    // ep0 = pseudo control service
    h->flags     = 0;
    h->len       = len;
    h->ctrlByte0 = 0;
    h->ctrlByte1 = fCtrlSeq++;
    h->pad0      = 0;
    h->pad1      = 0;

    uint8_t *p = frame + sizeof(HtcFrameHdr);
    for (uint32_t i = 0; i < len; i++)
        p[i] = ((const uint8_t *)payload)[i];

    return fCe->send(frame, sizeof(HtcFrameHdr) + len);
}

// ---- WMI path (CE3/CE2) ----------------------------------------------------

bool Htc::sendWmi(const void *payload, uint32_t len)
{
    if (fWmiEid == 0xFF) {
        IOLog("QCA9377-HTC: WMI endpoint not connected\n");
        return false;
    }
    if (len == 0 || len > 2040) {
        IOLog("QCA9377-HTC: sendWmi bad len %u\n", len);
        return false;
    }

    // Credit flow control is enabled for WMI control.
    uint32_t needed = (len + fCreditSize - 1) / fCreditSize;
    if (fWmiCredits < needed) {
        IOLog("QCA9377-HTC: insufficient credits (%u < %u)\n",
              fWmiCredits, needed);
        return false;
    }

    if (!fCe->wmiPostRecv())             // post rx BEFORE send (ath10k order)
        return false;

    uint8_t frame[8 + 2048];
    HtcFrameHdr *h = (HtcFrameHdr *)frame;
    h->eid       = fWmiEid;
    h->flags     = kHtcTxFlagNeedCreditUpdate;
    h->len       = len;
    h->ctrlByte0 = 0;
    h->ctrlByte1 = fWmiSeq++;
    h->pad0      = 0;
    h->pad1      = 0;

    uint8_t *p = frame + sizeof(HtcFrameHdr);
    for (uint32_t i = 0; i < len; i++)
        p[i] = ((const uint8_t *)payload)[i];

    if (!fCe->wmiSend(frame, sizeof(HtcFrameHdr) + len)) {
        return false;
    }

    fWmiCredits -= needed;               // consume on success
    return true;
}

uint32_t Htc::recvWmi(void *buf, uint32_t bufLen, uint32_t timeoutMs)
{
    if (!fCe->wmiRecvWait(timeoutMs))
        return 0;

    uint8_t *raw = fCe->wmiRxBuf();
    uint32_t rawLen = fCe->wmiRxNbytes();
    if (rawLen < sizeof(HtcFrameHdr)) {
        IOLog("QCA9377-HTC: short WMI frame %u\n", rawLen);
        return 0;
    }

    uint8_t eid       = raw[0];
    uint8_t flags     = raw[1];
    uint16_t len      = rd16(raw + 2);
    uint8_t trailerLen = raw[4];

    if (eid != fWmiEid && eid != 0) {
        IOLog("QCA9377-HTC: WMI eid %u (ours %u)\n", eid, fWmiEid);
        return 0;
    }
    if (len > rawLen - sizeof(HtcFrameHdr)) {
        IOLog("QCA9377-HTC: WMI hdr len %u > frame %u\n", len, rawLen);
        return 0;
    }
    if (trailerLen > len) {              // ath10k htc.c:496-500 guard
        IOLog("QCA9377-HTC: WMI trailer %u > len %u\n", trailerLen, len);
        return 0;
    }

    if (trailerLen && (flags & kHtcRxFlagTrailerPresent)) {
        const uint32_t trOff = sizeof(HtcFrameHdr) + len - trailerLen;
        if (trOff >= sizeof(HtcFrameHdr) &&
            trOff + trailerLen <= rawLen)
            applyCreditReport(raw + trOff, trailerLen);
    }

    const uint32_t payloadLen = len - trailerLen;
    if (payloadLen == 0 || payloadLen > bufLen) {
        IOLog("QCA9377-HTC: WMI payload %u (buf %u)\n", payloadLen, bufLen);
        return 0;
    }
    // Payload precedes the trailer; copy only the non-trailer prefix.
    for (uint32_t i = 0; i < payloadLen; i++)
        ((uint8_t *)buf)[i] = raw[sizeof(HtcFrameHdr) + i];
    return payloadLen;
}

// ---- boot sequence --------------------------------------------------------

bool Htc::waitTarget(uint32_t timeoutMs)
{
    // HTC_READY arrives unsolicited on CE1 (eid 0). Any pre-READY chatter
    // (service-available etc.) must not end the wait — keep polling until
    // READY itself lands or the budget runs out.
    const uint32_t deadline = timeoutMs * 1000 / kQCAPollStep_us;
    bool got = false;
    for (uint32_t i = 0; i < deadline && !got; i++) {
        if (!fCe->postRecv())
            return false;
        if (!fCe->recvWait(timeoutMs))
            return false;

        uint8_t *r = fCe->rxBuf();
        uint32_t rawLen = fCe->rxNbytes();
        if (rawLen < sizeof(HtcFrameHdr) + sizeof(HtcMsgHdr16)) {
            IOLog("QCA9377-HTC: ctrl frame too short (%u)\n", rawLen);
            continue;
        }
        uint16_t msgId = rd16(r + sizeof(HtcFrameHdr));
        if (msgId != kHtcMsgReady) {
            IOLog("QCA9377-HTC: pre-READY frame id=0x%04x (skip)\n", msgId);
            continue;
        }
        got = true;
    }
    if (!got) {
        IOLog("QCA9377-HTC: no READY within %u ms\n", timeoutMs);
        return false;
    }

    uint8_t *r = fCe->rxBuf();
    uint32_t rawLen = fCe->rxNbytes();
    if (rawLen < sizeof(HtcFrameHdr) + sizeof(HtcMsgHdr16) + sizeof(HtcReady)) {
        IOLog("QCA9377-HTC: ready frame too short (%u)\n", rawLen);
        return false;
    }

    const uint8_t *body = r + sizeof(HtcFrameHdr) + sizeof(HtcMsgHdr16);
    fTotalCredits = rd16(body);
    fCreditSize   = rd16(body + 2);

    IOLog("QCA9377-HTC: READY credits=%u size=%u endpoints=%u\n",
          fTotalCredits, fCreditSize, body[4]);

    if (rawLen >= sizeof(HtcFrameHdr) + sizeof(HtcMsgHdr16) +
                  sizeof(HtcReadyExt)) {
        const uint8_t *ext = body;
        IOLog("QCA9377-HTC: ready ext ver=%u bundle=%u\n",
              ext[6], ext[7]);
    }
    return fTotalCredits > 0 && fCreditSize > 0;
}

bool Htc::connectService(uint16_t serviceId, uint8_t *outEid,
                         uint16_t *outMaxMsg)
{
    uint8_t payload[sizeof(uint16_t) + sizeof(HtcConnSvc)];
    wr16(payload, kHtcMsgConnectSvc);

    uint16_t flags = kHtcConnFlagsDisableCreditFlow;
    if (serviceId == kHtcSvcWmiControl) {
        flags = 0;                       // credit flow ON for WMI control
        flags |= (uint16_t)((fTotalCredits & 0xFF) << kHtcConnFlagsRecvAllocShift);
    }

    HtcConnSvc *cs = (HtcConnSvc *)(payload + sizeof(uint16_t));
    cs->serviceId = serviceId;
    cs->flags     = flags;
    cs->pad0      = 0;
    cs->pad1      = 0;

    // Post the response recv BEFORE sending — the target may answer before
    // our doorbell write returns (same ordering rule as recvWmi/BMI).
    if (!fCe->postRecv())
        return false;
    if (!sendCtrlFrame(payload, sizeof(payload)))
        return false;

    // Target responds on CE1 ep0.
    if (!fCe->recvWait(kQCAExchangeTimeout_ms))
        return false;

    uint8_t *r = fCe->rxBuf();
    uint32_t rawLen = fCe->rxNbytes();
    if (rawLen < sizeof(HtcFrameHdr) + sizeof(uint16_t) + sizeof(HtcConnSvcResp)) {
        IOLog("QCA9377-HTC: connect resp too short (%u)\n", rawLen);
        return false;
    }

    uint16_t msgId = rd16(r + sizeof(HtcFrameHdr));
    if (msgId != kHtcMsgConnectResp) {
        IOLog("QCA9377-HTC: expected CONNECT_RESP, got 0x%04x\n", msgId);
        return false;
    }

    const uint8_t *body = r + sizeof(HtcFrameHdr) + sizeof(uint16_t);
    uint16_t svc  = rd16(body);
    uint8_t  st   = body[2];
    uint8_t  eid  = body[3];
    uint16_t mmsg = rd16(body + 4);

    if (svc != serviceId || st != 0) {
        IOLog("QCA9377-HTC: connect svc=0x%04x status=%u\n", svc, st);
        return false;
    }

    if (serviceId == kHtcSvcWmiControl) {
        fWmiEid = eid;
        fWmiCredits = fTotalCredits;     // all tx credits to WMI control
    }

    if (outEid)    *outEid = eid;
    if (outMaxMsg) *outMaxMsg = mmsg;
    IOLog("QCA9377-HTC: connected svc=0x%04x ep=%u maxMsg=%u\n",
          serviceId, eid, mmsg);
    return true;
}

bool Htc::setupComplete()
{
    uint8_t payload[sizeof(uint16_t) + sizeof(HtcSetupCompleteExt)];
    for (uint32_t i = 0; i < sizeof(payload); i++)
        payload[i] = 0;
    wr16(payload, kHtcMsgSetupCompleteEx);
    // flags = 0 (no RX bundle for PCI); rest zeroed.
    return sendCtrlFrame(payload, sizeof(payload));
}

} // namespace qca
