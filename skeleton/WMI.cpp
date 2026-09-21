/*
 * WMI.cpp — WMI-TLV event layer for QCA9377 (M4).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k wmi-tlv.c (see WMI.hpp for the full
 * attribution and pin).
 */

#include "WMI.hpp"
#include <IOKit/IOLib.h>

namespace qca {


// ---- TLV iterator (wmi-tlv.c ath10k_wmi_tlv_iter) --------------------------

struct TlvRec {
    uint16_t len;
    uint16_t tag;
    // value follows
};

// Returns records via callback-style scan: fills tb[tag] = {ptr, len}.
struct TlvView {
    const uint8_t *ptr;
    uint16_t len;
};

static const uint32_t kTlvMaxTag = 128;

// Parse a TLV stream; returns false on malformed input.
static bool tlvParse(const uint8_t *p, uint32_t len,
                     TlvView *tb, uint32_t tbLen)
{
    for (uint32_t i = 0; i < tbLen; i++) {
        tb[i].ptr = nullptr;
        tb[i].len = 0;
    }
    while (len > 0) {
        if (len < 4)
            return false;
        uint16_t tlvLen = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t tlvTag = (uint16_t)(p[2] | (p[3] << 8));
        p += 4;
        len -= 4;
        if (tlvLen > len)
            return false;
        if (tlvTag < tbLen) {
            tb[tlvTag].ptr = p;
            tb[tlvTag].len = tlvLen;
        }
        p += tlvLen;                     // no alignment advance (pinned iter)
        len -= tlvLen;
    }
    return true;
}

// ---- event decode ----------------------------------------------------------

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool Wmi::parseServiceReady(const uint8_t *p, uint32_t len)
{
    TlvView tb[kTlvMaxTag];
    if (!tlvParse(p, len, tb, kTlvMaxTag)) {
        IOLog("QCA9377-WMI: svc_ready TLV malformed\n");
        return false;
    }

    const uint8_t *ev = tb[kTlvSvcReadyEvent].ptr;
    uint16_t evLen = tb[kTlvSvcReadyEvent].len;
    if (!ev || evLen < 40) {             // through num_rf_chains (+40)
        IOLog("QCA9377-WMI: svc_ready ev missing/short (%u)\n", evLen);
        return false;
    }

    // struct wmi_tlv_svc_rdy_ev: fw_build(0-3), abi 6xu32 (4-27),
    // phy_capability (28), max_frag_entry (32), num_rf_chains (36).
    fSwVer0     = rd32(ev + 0);
    for (int i = 0; i < 6; i++)
        fAbi[i] = rd32(ev + 4 + 4 * i);
    fPhyCapab   = rd32(ev + 28);
    fNumRfChains = rd32(ev + 36);

    // First ARRAY_UINT32 = service bitmap.
    const uint8_t *bmap = tb[kTlvArrayUInt32].ptr;
    uint16_t bmapLen = tb[kTlvArrayUInt32].len;
    if (!bmap) {
        IOLog("QCA9377-WMI: no service bitmap\n");
        return false;
    }

    IOLog("QCA9377-WMI: SERVICE_READY fw_build=0x%08x phy=0x%08x rf=%u abi=[%08x %08x %08x %08x %08x %08x]\n",
          fSwVer0, fPhyCapab, fNumRfChains,
          fAbi[0], fAbi[1], fAbi[2], fAbi[3], fAbi[4], fAbi[5]);

    IOLog("QCA9377-WMI: service bitmap (%u B):", bmapLen);
    for (uint32_t i = 0; i < bmapLen && i < 64; i += 4)
        IOLog(" %08x", rd32(bmap + i));
    IOLog("\n");
    return true;
}

bool Wmi::parseReady(const uint8_t *p, uint32_t len)
{
    TlvView tb[kTlvMaxTag];
    if (!tlvParse(p, len, tb, kTlvMaxTag)) {
        IOLog("QCA9377-WMI: ready TLV malformed\n");
        return false;
    }

    const uint8_t *ev = tb[kTlvReadyEvent].ptr;
    uint16_t evLen = tb[kTlvReadyEvent].len;
    if (!ev || evLen < 36) {             // abi(24)+mac(8)+status(4)
        IOLog("QCA9377-WMI: ready ev missing/short (%u)\n", evLen);
        return false;
    }

    // struct wmi_tlv_rdy_ev { abi[6xu32], mac_addr(8B), status }
    for (int i = 0; i < 6; i++)
        fAbi[i] = rd32(ev + 4 * i);
    // mac_addr union: u8 addr[6] within 8B struct
    for (int i = 0; i < 6; i++)
        fMac[i] = ev[24 + i];
    uint32_t status = rd32(ev + 32);

    IOLog("QCA9377-WMI: READY mac=%02x:%02x:%02x:%02x:%02x:%02x status=%u\n",
          fMac[0], fMac[1], fMac[2], fMac[3], fMac[4], fMac[5], status);
    return true;
}

// ---- wait loop -------------------------------------------------------------

bool Wmi::waitServiceAndReady(uint32_t timeoutMs)
{
    uint8_t buf[2048];
    uint32_t deadlineMs = timeoutMs;

    // Pre-arm CE2 before SETUP_COMPLETE-driven events can arrive: the
    // target may send SERVICE_READY at any moment after HTC setup.
    fCe->wmiArmRecv();

    while (!fGotServiceReady) {
        uint32_t n = fHtc->recvWmi(buf, sizeof(buf), deadlineMs);
        if (n < 4) {
            IOLog("QCA9377-WMI: no event (%u)\n", n);
            return false;
        }
        fCe->wmiArmRecv();               // re-arm for the next event
        uint32_t id = rd32(buf) & kWmiCmdIdMask;
        deadlineMs = kQCAExchangeTimeout_ms;   // first event got the budget

        if (id == kWmiTlvEvtServiceReady) {
            if (!parseServiceReady(buf + 4, n - 4))
                return false;
            fGotServiceReady = true;
        } else {
            IOLog("QCA9377-WMI: pre-svcrdy event id=0x%06x (skip)\n", id);
        }
    }

    // READY: finite wait.
    uint32_t n = fHtc->recvWmi(buf, sizeof(buf), kQCAExchangeTimeout_ms);
    if (n < 4) {
        IOLog("QCA9377-WMI: no READY event (%u)\n", n);
        return false;
    }
    uint32_t id = rd32(buf) & kWmiCmdIdMask;
    if (id != kWmiTlvEvtReady) {
        IOLog("QCA9377-WMI: expected READY, got 0x%06x\n", id);
        return false;
    }
    return parseReady(buf + 4, n - 4);
}

void Wmi::macAddress(uint8_t out[6]) const
{
    for (int i = 0; i < 6; i++)
        out[i] = fMac[i];
}

} // namespace qca
