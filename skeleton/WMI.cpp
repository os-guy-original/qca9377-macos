/*
 * WMI.cpp — WMI-TLV event layer for QCA9377 (M4).
 *
 * SPDX-License-Identifier: ISC
 * Portions derived from Linux ath10k wmi-tlv.c (see WMI.hpp for the full
 * attribution and pin).
 */

#include "WMI.hpp"
#include <IOKit/IOLib.h>
#include <string.h>

namespace qca {

// WMI_INIT constants — all pinned to the reference source
// (ref/linux-ath10k/ath10k/{wmi-tlv.c,wmi-tlv.h,hw.h,core.c}).

// wmi_tlv_op_gen_init (wmi-tlv.c:1856-1968), qca6174-family values:
#define QCA_WMI_TLV_ABI_VER0 (((1u << 24) & 0xFF000000u) | 0u)   // MAJOR 1, MINOR 0
#define QCA_WMI_TLV_ABI_VER1 53u                                  // wmi-tlv.c:1258
#define QCA_WMI_TLV_ABI_VER_NS0 0x5F414351u                       // "QCA_"
#define QCA_WMI_TLV_ABI_VER_NS1 0x00004C4Du                       // "LM\0\0"
#define QCA_WMI_TLV_ABI_VER_NS2 0x00000000u
#define QCA_WMI_TLV_ABI_VER_NS3 0x00000000u

#define QCA_TARGET_TLV_NUM_VDEVS        4u    // hw.h:757
#define QCA_TARGET_TLV_NUM_PEERS        33u   // hw.h:759
#define QCA_TARGET_TLV_NUM_TIDS         66u   // 2*peers, hw.h:761
#define QCA_TARGET_TLV_NUM_TDLS_VDEVS   1u    // hw.h:760
#define QCA_TARGET_TLV_AST_SKID_LIMIT   0x10u // core.c:92 (6174 family)
#define QCA_TARGET_TLV_NUM_WDS_ENTRIES  0x20u // core.c:93
#define QCA_TARGET_TLV_NUM_MSDU_DESC    1056u // 1024+32, hw.h:762
#define QCA_WMI_RSRC_CFG_FLAG_TX_ACK_RSSI 0x1u // WMI_RSRC_CFG_FLAG_TX_ACK_RSSI

// WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL (wmi-tlv.h:1661).
#define QCA_WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL 0x200u

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
    // struct wmi_tlv_svc_rdy_ev (wmi-tlv.h:1721-1739):
    // fw_build_vers(0), abi[6](4-27), phy_capability(28), max_frag_entry(32),
    // num_rf_chains(36), ht_cap(40), vht_cap(44), vht_supp_mcs(48),
    // hw_min_tx_power(52), hw_max_tx_power(56), sys_cap_info(60),
    // min_pkt_size_enable(64), max_bcn_ie_size(68), num_mem_reqs(72),
    // max_num_scan_chans(76), hw_bd_id(80).
    fFwBuild    = rd32(ev + 0);
    fAbi[0]     = rd32(ev + 4);        // abi_ver0 — NOT fw_build (pre-0.5.6 log bug)
    for (int i = 1; i < 6; i++)
        fAbi[i] = rd32(ev + 4 + 4 * i);
    fPhyCapab   = rd32(ev + 28);
    fNumRfChains = rd32(ev + 36);
    fNumMemReqs = rd32(ev + 72);

    if (fNumMemReqs != 0)
        IOLog("QCA9377-WMI: NOTE num_mem_reqs=%u (host mem chunks are M5 work)\n",
              fNumMemReqs);

    // First ARRAY_UINT32 = service bitmap. Firmware-controlled length:
    // refuse anything that is not a multiple of 4 (all consumers below
    // step 4 bytes at a time).
    const uint8_t *bmap = tb[kTlvArrayUInt32].ptr;
    uint16_t bmapLen = tb[kTlvArrayUInt32].len;
    if (!bmap || bmapLen < 4 || (bmapLen & 3u)) {
        IOLog("QCA9377-WMI: bad service bitmap (len %u)\n", bmapLen);
        return false;
    }

    IOLog("QCA9377-WMI: SERVICE_READY fw_build=0x%08x abi0=0x%08x phy=0x%08x rf=%u memReq=%u abi=[%08x %08x %08x %08x %08x %08x]\n",
          fFwBuild, fAbi[0], fPhyCapab, fNumRfChains, fNumMemReqs,
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

// ---- WMI_INIT (ath10k_wmi_tlv_op_gen_init, wmi-tlv.c:1856-1968) -------------

// struct wmi_tlv_resource_config field order (wmi-tlv.h:1747-1789), __le32
// each, __packed - 44 u32 slots (rx_timeout_pri is [4]; count = 40 __le32
// declarations + 3 extra slots from the array). QCA6174-family values
// (core.c hw_params + hw.h:757-762; tail fields incl. host_capab from
// wmi-tlv.c:1914-1968).
static const uint32_t kRsrcCfg[44] = {
    /*  0 num_vdevs                */ QCA_TARGET_TLV_NUM_VDEVS,
    /*  1 num_peers                */ QCA_TARGET_TLV_NUM_PEERS,
    /*  2 num_offload_peers        */ 0,
    /*  3 num_offload_reorder_bufs */ 0,
    /*  4 num_peer_keys            */ 2,
    /*  5 num_tids                 */ QCA_TARGET_TLV_NUM_TIDS,
    /*  6 ast_skid_limit           */ QCA_TARGET_TLV_AST_SKID_LIMIT,
    /*  7 tx_chain_mask            */ 0x7,
    /*  8 rx_chain_mask            */ 0x7,
    /*  9 rx_timeout_pri[0]        */ 0x64,
    /* 10 rx_timeout_pri[1]        */ 0x64,
    /* 11 rx_timeout_pri[2]        */ 0x64,
    /* 12 rx_timeout_pri[3]        */ 0x28,
    /* 13 rx_decap_mode            */ 1,            // NATIVE_WIFI (hw.h:423)
    /* 14 scan_max_pending_reqs    */ 4,
    /* 15 bmiss_offload_max_vdev   */ QCA_TARGET_TLV_NUM_VDEVS,
    /* 16 roam_offload_max_vdev    */ QCA_TARGET_TLV_NUM_VDEVS,
    /* 17 roam_offload_max_ap_prof */ 8,
    /* 18 num_mcast_groups         */ 0,
    /* 19 num_mcast_table_elems    */ 0,
    /* 20 mcast2ucast_mode         */ 0,
    /* 21 tx_dbg_log_size          */ 0x400,
    /* 22 num_wds_entries          */ QCA_TARGET_TLV_NUM_WDS_ENTRIES,
    /* 23 dma_burst_size           */ 0,
    /* 24 mac_aggr_delim           */ 0,
    /* 25 rx_skip_defrag_dup_chk   */ 0,
    /* 26 vow_config               */ 0,
    /* 27 gtk_offload_max_vdev     */ 2,
    /* 28 num_msdu_desc            */ QCA_TARGET_TLV_NUM_MSDU_DESC,
    /* 29 max_frag_entries         */ 2,
    /* 30 num_tdls_vdevs           */ QCA_TARGET_TLV_NUM_TDLS_VDEVS,
    /* 31 num_tdls_conn_table_ent  */ 0x20,
    /* 32 beacon_tx_offload_max_vd */ 2,
    /* 33 num_multicast_filter_ent */ 5,
    /* 34 num_wow_filters          */ 22,           // TARGET_TLV_NUM_WOW_PATTERNS
    /* 35 num_keep_alive_pattern   */ 6,
    /* 36 keep_alive_pattern_size  */ 0,
    /* 37 max_tdls_conc_sleep_sta  */ 1,
    /* 38 max_tdls_conc_buffer_sta */ 1,
    /* 39 wmi_send_separate        */ 0,
    /* 40 num_ocb_vdevs            */ 0,
    /* 41 num_ocb_channels         */ 0,
    /* 42 num_ocb_schedules        */ 0,
    /* 43 host_capab               */ QCA_WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL,
};
static_assert(sizeof(kRsrcCfg) / sizeof(kRsrcCfg[0]) == 44,
              "resource_config must have exactly 44 u32 fields");

static inline void wrLe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)(v >> 24);
}

static inline void wrTlvHdr(uint8_t *p, uint16_t len, uint16_t tag)
{
    p[0] = (uint8_t)(len & 0xff);
    p[1] = (uint8_t)(len >> 8);
    p[2] = (uint8_t)(tag & 0xff);
    p[3] = (uint8_t)(tag >> 8);
}

// skb = {4B cmd_hdr}{TLV INIT_CMD(20B)}{TLV RESOURCE_CONFIG(160B)}{TLV
// ARRAY_STRUCT(0B)}. Zero host mem chunks: if firmware's num_mem_reqs != 0
// it will likely fail READY — that failure is the honest signal, logged.
bool Wmi::sendInit(void)
{
    // Frame = 4 cmd + (8+28) INIT_CMD + (8+176) RESOURCE_CONFIG + 8 ARRAY_STRUCT
    // = 232 B. INIT_CMD payload 28 = abi 24 + num_host_mem_chunks 4;
    // RESOURCE_CONFIG payload 176 = 44 x u32.
    uint8_t buf[4 + 8 + 28 + 8 + 176 + 8];
    memset(buf, 0, sizeof(buf));

    wrLe32(buf + 0, kWmiTlvCmdInit & kWmiCmdIdMask);   // WMI cmd header

    uint8_t *p = buf + 4;

    // TLV INIT_CMD {abi[6]=24B, num_host_mem_chunks=4B} = 28 B payload
    // (wmi-tlv.h:1806-1809; abi struct = 6 x __le32, wmi-tlv.h:1699-1706)
    wrTlvHdr(p, 28, kTlvInitCmd);
    wrLe32(p + 4,  QCA_WMI_TLV_ABI_VER0);
    wrLe32(p + 8,  QCA_WMI_TLV_ABI_VER1);
    wrLe32(p + 12, QCA_WMI_TLV_ABI_VER_NS0);
    wrLe32(p + 16, QCA_WMI_TLV_ABI_VER_NS1);
    wrLe32(p + 20, QCA_WMI_TLV_ABI_VER_NS2);
    wrLe32(p + 24, QCA_WMI_TLV_ABI_VER_NS3);
    wrLe32(p + 28, 0);                                 // num_host_mem_chunks
    p += 8 + 28;

    // TLV RESOURCE_CONFIG {44 x __le32} = 176 B (wmi-tlv.h:1747-1789)
    wrTlvHdr(p, 176, kTlvResourceConfig);
    for (uint32_t i = 0; i < 44; i++)
        wrLe32(p + 4 + 4 * i, kRsrcCfg[i]);
    p += 8 + 176;

    // TLV ARRAY_STRUCT, empty (num_mem_chunks == 0) (wmi-tlv.c:1881-1889)
    wrTlvHdr(p, 0, kTlvArrayStruct);
    p += 8;

    const uint32_t frameLen = (uint32_t)(p - buf);
    if (frameLen != 4 + 36 + 184 + 8) {                // 232 total
        IOLog("QCA9377-WMI: init frame size %u != 232\n", frameLen);
        return false;
    }
    return fHtc->sendWmi(buf, frameLen);
}

// ---- wait loop -------------------------------------------------------------

bool Wmi::waitServiceAndReady(uint32_t timeoutMs)
{
    uint8_t buf[2048];
    uint32_t deadlineMs = timeoutMs;

    // Pre-arm CE2 before SETUP_COMPLETE-driven events can arrive: the
    // target may send SERVICE_READY at any moment after HTC setup.
    fCe->wmiArmRecv();

    // Phase 1: SERVICE_READY (core.c:3095-3130). Skip unrelated events;
    // only a timeout or a parse failure ends the wait.
    while (fFwBuild == 0 && fAbi[0] == 0) {
        uint32_t n = fHtc->recvWmi(buf, sizeof(buf), deadlineMs);
        if (n < 4) {
            IOLog("QCA9377-WMI: no SERVICE_READY event (%u)\n", n);
            return false;
        }
        fCe->wmiArmRecv();               // re-arm for the next event
        uint32_t id = rd32(buf) & kWmiCmdIdMask;
        deadlineMs = kQCAExchangeTimeout_ms;   // first event got the budget

        if (id == kWmiTlvEvtServiceReady) {
            if (!parseServiceReady(buf + 4, n - 4))
                return false;
        } else {
            IOLog("QCA9377-WMI: pre-svcrdy event id=0x%06x (skip)\n", id);
        }
    }

    // Phase 2: WMI_INIT (core.c:3206). Without it the target never sends
    // READY - this send was missing before v0.5.6.
    if (!sendInit()) {
        IOLog("QCA9377-WMI: WMI_INIT send failed\n");
        return false;
    }
    IOLog("QCA9377-WMI: WMI_INIT sent, waiting for READY\n");

    // Phase 3: READY (core.c:3209-3216), bounded skip of unrelated events.
    for (int tries = 0; tries < 5; tries++) {
        uint32_t n = fHtc->recvWmi(buf, sizeof(buf), kQCAExchangeTimeout_ms);
        if (n < 4) {
            IOLog("QCA9377-WMI: no READY event (%u)\n", n);
            return false;
        }
        fCe->wmiArmRecv();
        uint32_t id = rd32(buf) & kWmiCmdIdMask;
        if (id == kWmiTlvEvtReady)
            return parseReady(buf + 4, n - 4);
        IOLog("QCA9377-WMI: pre-ready event id=0x%06x (skip %d/5)\n",
              id, tries + 1);
    }
    IOLog("QCA9377-WMI: READY never arrived after INIT\n");
    return false;
}

void Wmi::macAddress(uint8_t out[6]) const
{
    for (int i = 0; i < 6; i++)
        out[i] = fMac[i];
}

} // namespace qca
