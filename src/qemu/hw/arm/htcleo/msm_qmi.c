/*
 * Dream (MSM7201A) baseband: QMI on SMD DATA5-7_CNTL
 *
 * The 2.6.25 trout kernel's smd_qmi driver (/dev/qmi0-2) is how HTC's RIL
 * makes a data call: rild writes "up:<apn>" to /dev/qmiN, the driver asks
 * the modem over QMUX for a WDS client id, starts the network with the
 * APN, waits for the packet service status indication, then reads the
 * runtime settings (address, mask, gateway, DNS) and reports them back to
 * rild as STATE=/ADDR=/... text.  rild then configures rmnetN, whose IP
 * datagrams travel on SMD DATA5-7 (msm_atmodem.c bridges them to the
 * "msm-modem" NIC).  This is the modem's side of that QMUX exchange.
 *
 * Message ids and TLVs are the standard QMI CTL and WDS ones that the
 * driver's log strings name ("ctl: wds use client_id", "wds: got handle",
 * "got network profile", "wds: CONNECTED").
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "htcleo.h"
#include "msm_modem.h"

#define QMI_CTL                 0x00
#define QMI_WDS                 0x01

#define CTL_GET_CLIENT_ID       0x0022
#define CTL_RELEASE_CLIENT_ID   0x0023
#define WDS_START_NETWORK       0x0020
#define WDS_STOP_NETWORK        0x0021
#define WDS_PKT_SRVC_STATUS     0x0022
#define WDS_GET_RUNTIME         0x002D

#define PKT_DISCONNECTED        1
#define PKT_CONNECTED           2

#define WDS_CLIENT_ID           1
#define WDS_HANDLE              0x51D00001

typedef struct QmiOut {
    uint8_t buf[256];
    int len;            /* TLV bytes so far */
} QmiOut;

static void tlv(QmiOut *o, uint8_t type, const void *data, int len)
{
    if (o->len + 3 + len > (int)sizeof(o->buf)) {
        return;
    }
    o->buf[o->len] = type;
    stw_le_p(o->buf + o->len + 1, len);
    memcpy(o->buf + o->len + 3, data, len);
    o->len += 3 + len;
}

static void tlv_result(QmiOut *o, uint16_t error)
{
    uint8_t r[4];

    stw_le_p(r, error ? 1 : 0);
    stw_le_p(r + 2, error);
    tlv(o, 0x02, r, 4);
}

static void tlv_ip(QmiOut *o, uint8_t type, uint32_t ip)
{
    uint8_t b[4];

    stl_le_p(b, ip);
    tlv(o, type, b, 4);
}

/*
 * QMUX: if_type 1, u16 length (all but if_type), ctrl flag (0x80 from the
 * modem), service, client id; then the SDU header (CTL: flags, u8 txn;
 * others: flags, u16 txn), u16 message id, u16 TLV length, TLVs.
 */
static void qmi_send(MsmModemState *s, SmdChan *c, uint8_t svc, uint8_t cid,
                     uint8_t flags, uint16_t txn, uint16_t msg, QmiOut *o)
{
    uint8_t p[16 + sizeof(o->buf)];
    int n = 0;

    p[n++] = 0x01;
    n += 2;                                 /* length, below */
    p[n++] = 0x80;
    p[n++] = svc;
    p[n++] = cid;
    p[n++] = flags;
    if (svc == QMI_CTL) {
        p[n++] = txn;
    } else {
        stw_le_p(p + n, txn);
        n += 2;
    }
    stw_le_p(p + n, msg);
    stw_le_p(p + n + 2, o->len);
    n += 4;
    memcpy(p + n, o->buf, o->len);
    n += o->len;
    stw_le_p(p + 1, n - 1);
    if (s->trace) {
        qemu_log("modem qmi: %s <- svc %u cid %u msg %04x (%d TLV bytes)\n",
                 c->name, svc, cid, msg, o->len);
    }
    if (smd_modem_send_packet(s, c, p, n) >= 0) {
        smd_kick_apps(s, c);
    }
}

/* "DATA5_CNTL" -> "DATA5", the rmnet channel it controls */
static void data_chan(const SmdChan *c, char *out, size_t size)
{
    const char *u = strchr(c->name, '_');

    pstrcpy(out, MIN(size, (size_t)(u ? u - c->name + 1 : size)), c->name);
}

static void send_pkt_status(MsmModemState *s, SmdChan *c, uint8_t cid,
                            uint16_t txn, uint8_t flags, bool connected)
{
    QmiOut o = { .len = 0 };
    uint8_t st[2] = { connected ? PKT_CONNECTED : PKT_DISCONNECTED, 0 };

    if (flags == 0x02) {                    /* a response, not the ind */
        tlv_result(&o, 0);
    } else {
        /*
         * the driver takes indications only as broadcasts (measured: to
         * its client id it logged "unknown msg type 0x0022")
         */
        cid = 0xFF;
    }
    tlv(&o, 0x01, st, 2);
    qmi_send(s, c, QMI_WDS, cid, flags, txn, WDS_PKT_SRVC_STATUS, &o);
}

static void wds_msg(MsmModemState *s, SmdChan *c, uint8_t cid, uint16_t txn,
                    uint16_t msg, const uint8_t *tlvs, int len)
{
    QmiOut o = { .len = 0 };
    char data[21];
    uint32_t ip, gw, dns;
    uint8_t h[4];

    data_chan(c, data, sizeof(data));
    switch (msg) {
    case WDS_START_NETWORK:
        msm_rmnet_set_active(s, data, true);
        tlv_result(&o, 0);
        stl_le_p(h, WDS_HANDLE);
        tlv(&o, 0x01, h, 4);
        qmi_send(s, c, QMI_WDS, cid, 0x02, txn, msg, &o);
        send_pkt_status(s, c, cid, 0, 0x04, true);
        break;
    case WDS_STOP_NETWORK:
        msm_rmnet_set_active(s, data, false);
        tlv_result(&o, 0);
        qmi_send(s, c, QMI_WDS, cid, 0x02, txn, msg, &o);
        send_pkt_status(s, c, cid, 0, 0x04, false);
        break;
    case WDS_PKT_SRVC_STATUS:
        send_pkt_status(s, c, cid, txn, 0x02,
                        msm_rmnet_is_active(s, data));
        break;
    case WDS_GET_RUNTIME:
        msm_rmnet_addrs(s, &ip, &gw, &dns);
        tlv_result(&o, 0);
        tlv_ip(&o, 0x15, dns);              /* primary DNS */
        tlv_ip(&o, 0x16, dns);              /* secondary DNS */
        tlv_ip(&o, 0x1E, ip);               /* IPv4 address */
        tlv_ip(&o, 0x20, gw);               /* gateway */
        tlv_ip(&o, 0x21, 0xFFFFFF00);       /* subnet mask */
        qmi_send(s, c, QMI_WDS, cid, 0x02, txn, msg, &o);
        break;
    default:
        tlv_result(&o, 0);
        qmi_send(s, c, QMI_WDS, cid, 0x02, txn, msg, &o);
        break;
    }
}

static void ctl_msg(MsmModemState *s, SmdChan *c, uint16_t txn, uint16_t msg,
                    const uint8_t *tlvs, int len)
{
    QmiOut o = { .len = 0 };
    uint8_t id[2] = { QMI_WDS, WDS_CLIENT_ID };

    switch (msg) {
    case CTL_GET_CLIENT_ID:
        /* the request's TLV 0x01 names the service */
        if (len >= 4 && tlvs[0] == 0x01) {
            id[0] = tlvs[3];
        }
        tlv_result(&o, 0);
        tlv(&o, 0x01, id, 2);
        break;
    default:            /* RELEASE_CLIENT_ID, SYNC, ...: echo success */
        tlv_result(&o, 0);
        if (msg == CTL_RELEASE_CLIENT_ID && len >= 5 && tlvs[0] == 0x01) {
            tlv(&o, 0x01, tlvs + 3, 2);
        }
        break;
    }
    qmi_send(s, c, QMI_CTL, 0, 0x01, txn, msg, &o);
}

void msm_qmi_input(MsmModemState *s, SmdChan *c, const uint8_t *p, int len)
{
    while (len >= 12 && p[0] == 0x01) {
        int qlen = lduw_le_p(p + 1) + 1, off;
        uint8_t svc = p[4], cid = p[5];
        uint16_t txn, msg, tlen;

        if (qlen > len) {
            break;
        }
        if (svc == QMI_CTL) {
            txn = p[7];
            off = 8;
        } else {
            txn = lduw_le_p(p + 7);
            off = 9;
        }
        if (off + 4 > qlen) {
            break;
        }
        msg = lduw_le_p(p + off);
        tlen = MIN(lduw_le_p(p + off + 2), qlen - off - 4);
        if (s->trace) {
            qemu_log("modem qmi: %s -> svc %u cid %u txn %u msg %04x "
                     "(%u TLV bytes)\n", c->name, svc, cid, txn, msg, tlen);
        }
        if (svc == QMI_CTL) {
            ctl_msg(s, c, txn, msg, p + off + 4, tlen);
        } else if (svc == QMI_WDS) {
            wds_msg(s, c, cid, txn, msg, p + off + 4, tlen);
        }
        p += qlen;
        len -= qlen;
    }
}
