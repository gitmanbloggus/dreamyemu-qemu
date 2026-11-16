/*
 * HTC Dream VIDEOTASK: H.264 subframe packets back to an Annex-B stream.
 *
 * The host (libOmxH264Dec on Android) parses the slice header itself and
 * hands the DSP what it needs (htcdreamfulldriverdecomp dsp/vdec.c):
 *
 *   b201 W H line_width frame_info  8x ref buffer (hi,lo)  8x 0 (hi,lo)
 *        output buffer (hi,lo) 7fff
 *   b202 0 qp|0x200 (I)|0x800 (pad byte) 018c  [P: nref-1, 4 list words]
 *        payload bytes (hi,lo) slice-header-bits 7fff
 *   the NAL after its header byte, padded to even, each 16-bit word swapped
 *
 * "slice header bits" counts the RBSP bits of the original slice header,
 * so the DSP starts at slice_data() and never sees the SPS/PPS.  This
 * rewriter does the same: it strips the original header and writes a new
 * one against a fixed SPS/PPS of its own (POC type 2, 4-bit frame_num, four
 * reference frames, pic_init_qp 26, deblocking at its defaults), so any
 * H.264 decoder can take the result.  I pictures become IDRs.
 */

#include <stdlib.h>
#include <string.h>
#include "dream_h264_rw.h"

typedef struct BitW {
    uint8_t *b;
    size_t cap, pos;            /* pos in bits */
    bool overflow;
} BitW;

static void bw_bit(BitW *w, unsigned v)
{
    if ((w->pos >> 3) >= w->cap) {
        w->overflow = true;
        return;
    }
    if (!(w->pos & 7)) {
        w->b[w->pos >> 3] = 0;
    }
    if (v) {
        w->b[w->pos >> 3] |= 0x80 >> (w->pos & 7);
    }
    w->pos++;
}

static void bw_u(BitW *w, int n, uint32_t v)
{
    while (n--) {
        bw_bit(w, (v >> n) & 1);
    }
}

static void bw_ue(BitW *w, uint32_t v)
{
    int n = 0;

    while ((v + 1) >> (n + 1)) {
        n++;
    }
    bw_u(w, n, 0);
    bw_u(w, n + 1, v + 1);
}

static void bw_se(BitW *w, int v)
{
    bw_ue(w, v > 0 ? 2 * v - 1 : -2 * v);
}

static void bw_trailing(BitW *w)
{
    bw_bit(w, 1);
    while (w->pos & 7) {
        bw_bit(w, 0);
    }
}

/* start code + NAL header + RBSP with emulation prevention */
static int put_nal(uint8_t *out, size_t cap, uint8_t hdr,
                   const uint8_t *rbsp, size_t n)
{
    size_t o = 0;
    int zeros = 0;

    if (cap < 5) {
        return -1;
    }
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 1;
    out[o++] = hdr;
    for (size_t i = 0; i < n; i++) {
        if (o + 2 > cap) {
            return -1;
        }
        if (zeros >= 2 && rbsp[i] <= 3) {
            out[o++] = 3;
            zeros = 0;
        }
        out[o++] = rbsp[i];
        zeros = rbsp[i] ? 0 : zeros + 1;
    }
    return o;
}

static int put_sps_pps(H264Rw *rw, uint8_t *out, size_t cap, int mbw, int mbh)
{
    uint8_t buf[32];
    BitW w = { buf, sizeof(buf) };
    int a, b;

    bw_u(&w, 8, 66);            /* Baseline */
    bw_u(&w, 8, 0xC0);          /* constraint_set0/1 */
    bw_u(&w, 8, 30);            /* level 3.0 */
    bw_ue(&w, 0);               /* sps id */
    bw_ue(&w, 0);               /* log2_max_frame_num - 4 */
    bw_ue(&w, 2);               /* pic_order_cnt_type */
    bw_ue(&w, 4);               /* max_num_ref_frames */
    bw_u(&w, 1, 0);             /* gaps_in_frame_num_allowed */
    bw_ue(&w, mbw - 1);
    bw_ue(&w, mbh - 1);
    bw_u(&w, 1, 1);             /* frame_mbs_only */
    bw_u(&w, 1, 1);             /* direct_8x8_inference */
    bw_u(&w, 1, 0);             /* frame_cropping */
    bw_u(&w, 1, 0);             /* vui */
    bw_trailing(&w);
    a = put_nal(out, cap, 0x67, buf, w.pos >> 3);
    if (a < 0) {
        return -1;
    }

    w = (BitW){ buf, sizeof(buf) };
    bw_ue(&w, 0);               /* pps id */
    bw_ue(&w, 0);               /* sps id */
    bw_u(&w, 1, 0);             /* CAVLC */
    bw_u(&w, 1, 0);             /* bottom_field_pic_order */
    bw_ue(&w, 0);               /* one slice group */
    bw_ue(&w, 0);               /* num_ref_idx_l0_default - 1 */
    bw_ue(&w, 0);               /* num_ref_idx_l1_default - 1 */
    bw_u(&w, 1, 0);             /* weighted_pred */
    bw_u(&w, 2, 0);             /* weighted_bipred */
    bw_se(&w, 0);               /* pic_init_qp - 26 */
    bw_se(&w, 0);               /* pic_init_qs - 26 */
    bw_se(&w, rw->chroma_qp_offset);
    bw_u(&w, 1, 0);             /* deblocking_filter_control_present */
    bw_u(&w, 1, 0);             /* constrained_intra_pred */
    bw_u(&w, 1, 0);             /* redundant_pic_cnt_present */
    bw_trailing(&w);
    b = put_nal(out + a, cap - a, 0x68, buf, w.pos >> 3);
    return b < 0 ? -1 : a + b;
}

int h264rw_packet(H264Rw *rw, const uint16_t *w, size_t nwords,
                  uint8_t *out, size_t outcap, H264RwPic *pic)
{
    size_t k, plen, rlen, nbits, last1;
    unsigned flags, qp, hdr_bits, nref = 1;
    bool intra;
    uint8_t *pay, *rbsp, *slice;
    BitW bw;
    int o = 0, n;

    memset(pic, 0, sizeof(*pic));
    if (nwords >= 1 && w[0] == 0xb203) {
        pic->end = true;
        return 0;
    }
    if (nwords < 48 || w[0] != 0xb201 || w[39] != 0x7fff || w[40] != 0xb202) {
        return -1;
    }
    pic->width = w[1];
    pic->height = w[2];
    pic->line_width = w[3];
    pic->out_addr = (uint32_t)w[37] << 16 | w[38];
    if (!pic->width || !pic->height || (pic->width | pic->height) & 15) {
        return -1;
    }

    flags = w[42];
    intra = flags & 0x200;
    qp = flags & 0x3f;
    k = 44;
    if (!intra) {
        nref = w[k] + 1;
        k += 5;
    }
    if (k + 4 > nwords || w[k + 3] != 0x7fff) {
        return -1;
    }
    plen = (size_t)w[k] << 16 | w[k + 1];
    hdr_bits = w[k + 2];
    k += 4;
    if (plen > 2 * (nwords - k)) {
        return -1;
    }
    if (flags & 0x800 && plen) {
        plen--;                 /* the pad byte */
    }

    /* unswap, then drop the emulation prevention bytes */
    pay = malloc(3 * plen + 64);
    rbsp = pay + plen;
    slice = rbsp + plen;            /* plen + 64 bytes */
    if (!pay) {
        return -1;
    }
    for (size_t i = 0; i < plen; i++) {
        uint16_t v = w[k + i / 2];
        pay[i] = i & 1 ? v & 0xff : v >> 8;
    }
    rlen = 0;
    for (size_t i = 0, z = 0; i < plen; i++) {
        if (z >= 2 && pay[i] == 3) {
            z = 0;
            continue;
        }
        rbsp[rlen++] = pay[i];
        z = pay[i] ? 0 : z + 1;
    }
    /* the RBSP stop bit: slice data is everything before it */
    nbits = rlen * 8;
    while (nbits && !(rbsp[(nbits - 1) >> 3] & (0x80 >> ((nbits - 1) & 7)))) {
        nbits--;
    }
    if (!nbits || nbits - 1 < hdr_bits) {
        goto fail;
    }
    last1 = nbits - 1;

    if (intra) {
        rw->frame_num = 0;
        n = put_sps_pps(rw, out, outcap, pic->width / 16, pic->height / 16);
        if (n < 0) {
            goto fail;
        }
        o = n;
    }

    /* the new slice header, then the original slice data */
    bw = (BitW){ slice, plen + 64 };
    bw_ue(&bw, 0);                      /* first_mb_in_slice */
    bw_ue(&bw, intra ? 7 : 5);          /* I / P, all slices alike */
    bw_ue(&bw, 0);                      /* pps id */
    bw_u(&bw, 4, rw->frame_num & 15);
    if (intra) {
        bw_ue(&bw, rw->idr_id++ & 1);   /* idr_pic_id */
    } else {
        bw_u(&bw, 1, 1);                /* num_ref_idx_active_override */
        bw_ue(&bw, nref - 1);
        bw_u(&bw, 1, 0);                /* ref_pic_list_modification */
    }
    if (intra) {
        bw_u(&bw, 1, 0);                /* no_output_of_prior_pics */
        bw_u(&bw, 1, 0);                /* long_term_reference */
    } else {
        bw_u(&bw, 1, 0);                /* adaptive_ref_pic_marking */
    }
    bw_se(&bw, (int)qp - 26);           /* slice_qp_delta */
    for (size_t i = hdr_bits; i < last1; i++) {
        bw_bit(&bw, (rbsp[i >> 3] >> (7 - (i & 7))) & 1);
    }
    bw_trailing(&bw);
    if (bw.overflow) {
        goto fail;
    }
    n = put_nal(out + o, outcap - o, intra ? 0x65 : 0x41, slice, bw.pos >> 3);
    if (n < 0) {
        goto fail;
    }
    free(pay);
    rw->frame_num++;
    return o + n;

fail:
    free(pay);
    return -1;
}
