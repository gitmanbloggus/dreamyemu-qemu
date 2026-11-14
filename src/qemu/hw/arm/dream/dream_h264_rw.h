/*
 * HTC Dream VIDEOTASK: H.264 subframe packets back to an Annex-B stream.
 * No QEMU dependencies, so tests can build it on its own.
 */
#ifndef HW_ARM_DREAM_H264_RW_H
#define HW_ARM_DREAM_H264_RW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct H264Rw {
    int chroma_qp_offset;       /* the PPS value; not in the packets */
    unsigned frame_num;         /* of the next picture */
    unsigned idr_id;
} H264Rw;

typedef struct H264RwPic {
    bool end;                   /* b203: end of stream */
    int width, height, line_width;
    uint32_t out_addr;          /* NV21 output frame buffer */
} H264RwPic;

/*
 * Rewrite one packet (host-order 16-bit words, as the DSP reads them) into
 * Annex-B NAL units: SPS + PPS before every IDR, then the slice.  Returns
 * the bytes written to out, 0 for the end packet, -1 for a packet it does
 * not understand (or one that does not fit in outcap).
 */
int h264rw_packet(H264Rw *rw, const uint16_t *w, size_t nwords,
                  uint8_t *out, size_t outcap, H264RwPic *pic);

#endif
