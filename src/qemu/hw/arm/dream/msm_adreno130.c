/*
 * HTC Dream (MSM7201A) Adreno 130 ("Q3Dimension"), as far as the captures
 * in htcdreamfulldriverdecomp hgl/gputrace go (NOTES.md there): libhgl.so
 * driven through regtrace.c on the phone, clears, a Gouraud triangle and
 * single-state variants (blend, scissor, cull, flat, depth, textures),
 * each with the frame the GPU drew.
 *
 * Window at 0xA0000000, 1 MiB, IRQ 20:
 *   +0x10000..  registers; PM4 type-0 register n is the dword at
 *               +0x10000 + 4n (ring write pointer 0x88 = +0x220, timestamp
 *               0x98 = +0x260), so MMIO and the command stream share one
 *               register file
 *   +0x20000..  on-chip memory: the render target (RGB565, rows top-down
 *               in screen space, i.e. GL row y at row H-1-y)
 *
 * Command processor: ring at reg 0x85 (+0x214), control 0x84, write
 * pointer 0x88 (dwords; a write runs the ring), read pointer written back
 * to the GPU address in reg 0x96.  Indirect buffers: type-0 writes of
 * {address, dwords} to regs 0x8a/0x8b.  Type 3 op 0x2a draws:
 *   word 0 = flags << 24 | count << 16 | primitive (0x0c list, 0x02 fan);
 *            flag 0x08 flat shading, 0x20 cull back faces
 *   reg 0x71a = 1: vertex pass: source stream reg 0x709 (format reg 0x703:
 *            stride in dwords in the top byte, bit 18 texcoords, bit 17
 *            colour), transformed by the 4x4 matrix in constants 0-15,
 *            written to the stream at reg 0x708
 *   reg 0x71a = 2: raster pass from the stream at reg 0x70a
 *   payload longer than 2 words: inline vertices {x, y, z, ABGR}
 * Constants: reg 0x70b selects (value >> 16) & 0x3fff as a float index,
 * reg 0x70c stores floats from there on (viewport scale at 0x40, offset at
 * 0x44, constant colour 0x2c).  Reg 0x70d takes vertex program microcode,
 * which is kept but not run: the vertex pass is the fixed-function
 * transform it encodes.
 *
 * GPU addresses: the two pmem pools libhgl uses, pmem_gpu1 (EBI) at GPU
 * 0x100000 and pmem_gpu0 (SMI) at GPU 0x900000, at the physical addresses
 * adreno130.ko gives them (properties).  How the GPU's own aperture
 * registers express this is not known.
 *
 * Off by default on -M dream (gpu=on): with the window unmapped libhgl
 * fails its chip check and Android draws in software; a GPU that passes
 * it has to draw everything SurfaceFlinger asks for, and window-sized
 * surfaces (tiling), strips and more texture formats are not modelled yet.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/physmem.h"
#include "msm_adreno130.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmAdreno130State, MSM_ADRENO130)

#define A130_SIZE       0x100000
#define A130_REG_BASE   0x10000
#define A130_NREGS      0x4000
#define A130_GMEM_BASE  0x20000
#define A130_GMEM_SIZE  (A130_SIZE - A130_GMEM_BASE)
#define A130_CHIP_ID    0x51301002
#define A130_IDLE       0x08000110

/* register numbers (dword index from +0x10000) */
#define R_CHIP_ID       0x000
#define R_STATUS        0x050   /* +0x140 */
#define R_IRQ_EN        0x080   /* +0x200, bit 18 */
#define R_IRQ_STAT      0x081
#define R_RB_CNTL       0x084
#define R_RB_BASE       0x085
#define R_RB_WPTR       0x088
#define R_IB_ADDR       0x08a
#define R_IB_SIZE       0x08b
#define R_RB_RPTR_ADDR  0x096
#define R_TIMESTAMP     0x098
#define R_DEPTH         0x653
#define R_TEX_MEM       0x670
#define R_ROP           0x67f
#define R_TEX_COMBINE   0x692
#define R_TEX_SAMPLER   0x6a0
#define R_BLEND         0x6a5
#define R_WIN_X         0x6aa
#define R_WIN_Y         0x6ad
#define R_TEX_FORMAT    0x6b3
#define R_SURFACE       0x6c5
#define R_TEX_SIZE      0x6dc
#define R_VTX_FMT_SRC   0x703
#define R_VTX_FMT_DRAW  0x705
#define R_STREAM_OUT    0x708
#define R_STREAM_SRC    0x709
#define R_STREAM_RAST   0x70a
#define R_CONST_SEL     0x70b
#define R_CONST_DATA    0x70c
#define R_VS_UCODE      0x70d
#define R_DRAW_MODE     0x71a

#define IRQ_BIT         (1u << 18)
#define NCONST          0x400
#define MAX_VERTS       4096
#define OUT_STRIDE      10      /* x y z w s t ABGR, padded like libhgl's */

typedef struct A130Vtx {
    float x, y, z, w, s, t;
    float c[4];                 /* r g b a, 0..255 */
} A130Vtx;

struct MsmAdreno130State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[A130_NREGS];
    uint8_t *gmem;
    uint16_t *depth;            /* the depth buffer, 16-bit, per pixel */
    float consts[NCONST];
    int const_idx;
    uint32_t vs_ucode[1024];
    unsigned vs_len;
    uint32_t rptr;
    int ib_depth;

    /* GPU address space: two windows onto guest RAM */
    uint32_t ebi_phys, ebi_gpu, ebi_size;
    uint32_t smi_phys, smi_gpu, smi_size;
    uint32_t fb_pitch;          /* render target pixels per row */
    bool trace;
    uint64_t draws, tris;
};

/* ---------------------------------------------------------------- */
/* GPU memory                                                        */
/* ---------------------------------------------------------------- */

static bool gpu_phys(MsmAdreno130State *s, uint32_t gpu, uint32_t len,
                     hwaddr *pa)
{
    if (gpu >= s->ebi_gpu && gpu + (uint64_t)len <= s->ebi_gpu + s->ebi_size) {
        *pa = s->ebi_phys + (gpu - s->ebi_gpu);
        return true;
    }
    if (gpu >= s->smi_gpu && gpu + (uint64_t)len <= s->smi_gpu + s->smi_size) {
        *pa = s->smi_phys + (gpu - s->smi_gpu);
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "adreno130: GPU address %08x+%u unmapped\n",
                  gpu, len);
    return false;
}

static bool gpu_read(MsmAdreno130State *s, uint32_t gpu, void *buf,
                     uint32_t len)
{
    hwaddr pa;

    if (!gpu_phys(s, gpu, len, &pa)) {
        memset(buf, 0, len);
        return false;
    }
    physical_memory_read(pa, buf, len);
    return true;
}

static void gpu_write(MsmAdreno130State *s, uint32_t gpu, const void *buf,
                      uint32_t len)
{
    hwaddr pa;

    if (gpu_phys(s, gpu, len, &pa)) {
        physical_memory_write(pa, buf, len);
    }
}

static float f32(uint32_t v)
{
    float f;

    memcpy(&f, &v, 4);
    return f;
}

static uint32_t u32f(float f)
{
    uint32_t v;

    memcpy(&v, &f, 4);
    return v;
}

static void abgr(uint32_t v, float c[4])
{
    c[0] = v & 0xff;
    c[1] = (v >> 8) & 0xff;
    c[2] = (v >> 16) & 0xff;
    c[3] = v >> 24;
}

/* ---------------------------------------------------------------- */
/* Pixels                                                            */
/* ---------------------------------------------------------------- */

static void tex_fetch(MsmAdreno130State *s, int x, int y, int w, int h,
                      float c[4])
{
    uint32_t fmt = s->regs[R_TEX_FORMAT] >> 24;
    uint32_t base = s->regs[R_TEX_MEM] & 0xffff0000;
    bool repeat_s = s->regs[R_TEX_SAMPLER] & 0x1000;
    bool repeat_t = s->regs[R_TEX_SAMPLER] & 0x4000;
    uint8_t b[4];

    x = repeat_s ? x & (w - 1) : MIN(MAX(x, 0), w - 1);
    y = repeat_t ? y & (h - 1) : MIN(MAX(y, 0), h - 1);
    if (fmt == 0x20) {                          /* RGB565 */
        uint16_t p;

        gpu_read(s, base + (y * w + x) * 2, b, 2);
        p = lduw_le_p(b);
        c[0] = ((p >> 11) & 31) * 255.0f / 31;
        c[1] = ((p >> 5) & 63) * 255.0f / 63;
        c[2] = (p & 31) * 255.0f / 31;
        c[3] = 255;
    } else {                                    /* 0xb8: RGBA8888 bytes */
        gpu_read(s, base + (y * w + x) * 4, b, 4);
        c[0] = b[0];
        c[1] = b[1];
        c[2] = b[2];
        c[3] = b[3];
    }
}

static void tex_sample(MsmAdreno130State *s, float u, float v, float c[4])
{
    int w = 1 << (s->regs[R_TEX_SIZE] & 0xf);
    int h = 1 << ((s->regs[R_TEX_SIZE] >> 4) & 0xf);
    float x = u * w - 0.5f, y = v * h - 0.5f;

    if (s->regs[R_TEX_SAMPLER] & 0xa0) {        /* linear */
        int x0 = floorf(x), y0 = floorf(y);
        float fx = x - x0, fy = y - y0, t[4][4];

        tex_fetch(s, x0, y0, w, h, t[0]);
        tex_fetch(s, x0 + 1, y0, w, h, t[1]);
        tex_fetch(s, x0, y0 + 1, w, h, t[2]);
        tex_fetch(s, x0 + 1, y0 + 1, w, h, t[3]);
        for (int i = 0; i < 4; i++) {
            c[i] = (t[0][i] * (1 - fx) + t[1][i] * fx) * (1 - fy) +
                   (t[2][i] * (1 - fx) + t[3][i] * fx) * fy;
        }
    } else {
        tex_fetch(s, floorf(u * w), floorf(v * h), w, h, c);
    }
}

static float blend_factor(unsigned f, const float src[4], const float dst[4],
                          int ch)
{
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return src[ch] / 255;
    case 3: return 1 - src[ch] / 255;
    case 4: return src[3] / 255;
    case 5: return 1 - src[3] / 255;
    case 6: return dst[3] / 255;
    case 7: return 1 - dst[3] / 255;
    case 8: return dst[ch] / 255;
    case 9: return 1 - dst[ch] / 255;
    default:
        qemu_log_mask(LOG_UNIMP, "adreno130: blend factor %u\n", f);
        return 1;
    }
}

static void put_pixel(MsmAdreno130State *s, int x, int y, float z,
                      const float col[4])
{
    uint32_t off = (y * s->fb_pitch + x) * 2;
    uint32_t dctl = s->regs[R_DEPTH];
    float c[4] = { col[0], col[1], col[2], col[3] };
    uint16_t p;

    if (off + 2 > A130_GMEM_SIZE) {
        return;
    }
    if ((s->regs[R_SURFACE] & 0x800) && (dctl & 1)) {
        uint16_t zi = MIN(MAX(z, 0), 65535);
        uint16_t zb = s->depth[y * s->fb_pitch + x];
        bool pass;

        switch ((dctl >> 4) & 7) {
        case 0: pass = false; break;
        case 1: pass = zi < zb; break;
        case 2: pass = zi == zb; break;
        case 3: pass = zi <= zb; break;
        case 4: pass = zi > zb; break;
        case 5: pass = zi != zb; break;
        case 6: pass = zi >= zb; break;
        default: pass = true; break;           /* 7: ALWAYS */
        }
        if (!pass) {
            return;
        }
        if (dctl & 0x100) {
            s->depth[y * s->fb_pitch + x] = zi;
        }
    }
    if (s->regs[R_ROP] & 0x800) {               /* blending */
        uint32_t bf = s->regs[R_BLEND];
        float d[4];
        uint16_t q = lduw_le_p(s->gmem + off);

        d[0] = ((q >> 11) & 31) * 255.0f / 31;
        d[1] = ((q >> 5) & 63) * 255.0f / 63;
        d[2] = (q & 31) * 255.0f / 31;
        d[3] = 255;
        for (int i = 0; i < 3; i++) {
            c[i] = c[i] * blend_factor((bf >> 12) & 15, col, d, i) +
                   d[i] * blend_factor((bf >> 8) & 15, col, d, i);
        }
    }
    for (int i = 0; i < 3; i++) {
        c[i] = MIN(MAX(c[i], 0), 255);
    }
    p = ((int)(c[0] * 31 / 255 + 0.5f) << 11) |
        ((int)(c[1] * 63 / 255 + 0.5f) << 5) |
        (int)(c[2] * 31 / 255 + 0.5f);
    stw_le_p(s->gmem + off, p);
}

/* ---------------------------------------------------------------- */
/* Triangles                                                         */
/* ---------------------------------------------------------------- */

static void viewport(MsmAdreno130State *s, A130Vtx *v)
{
    float iw = v->w ? 1 / v->w : 1;

    v->x = v->x * iw * s->consts[0x40] + s->consts[0x44];
    v->y = v->y * iw * s->consts[0x41] + s->consts[0x45];
    v->z = v->z * iw * s->consts[0x42] + s->consts[0x46];
}

static void raster_tri(MsmAdreno130State *s, const A130Vtx *a,
                       const A130Vtx *b, const A130Vtx *c, uint32_t flags)
{
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    int x0 = s->regs[R_WIN_X] & 0xffff, x1 = s->regs[R_WIN_X] >> 16;
    int y0 = s->regs[R_WIN_Y] & 0xffff, y1 = s->regs[R_WIN_Y] >> 16;
    bool tex = !(s->regs[R_ROP] & 0x40);       /* 0x67f bit 6: no texture */
    bool replace = (s->regs[R_TEX_COMBINE] & 0xff) == 0;
    int bx0, bx1, by0, by1;

    if (area == 0) {
        return;
    }
    /* GL's counter-clockwise is negative area with y down */
    if ((flags & 0x20) && area > 0) {
        return;
    }
    bx0 = MAX(x0, (int)floorf(MIN(a->x, MIN(b->x, c->x))));
    bx1 = MIN(x1, (int)ceilf(MAX(a->x, MAX(b->x, c->x))));
    by0 = MAX(y0, (int)floorf(MIN(a->y, MIN(b->y, c->y))));
    by1 = MIN(y1, (int)ceilf(MAX(a->y, MAX(b->y, c->y))));
    s->tris++;

    for (int y = by0; y <= by1; y++) {
        for (int x = bx0; x <= bx1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) / area;
            float w1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) / area;
            float w2 = 1 - w0 - w1, col[4];

            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }
            for (int i = 0; i < 4; i++) {
                col[i] = flags & 0x08 ? c->c[i] :
                         w0 * a->c[i] + w1 * b->c[i] + w2 * c->c[i];
            }
            if (tex) {
                float t[4];

                tex_sample(s, w0 * a->s + w1 * b->s + w2 * c->s,
                           w0 * a->t + w1 * b->t + w2 * c->t, t);
                for (int i = 0; i < 4; i++) {
                    col[i] = replace ? t[i] : col[i] * t[i] / 255;
                }
            }
            put_pixel(s, x, y, w0 * a->z + w1 * b->z + w2 * c->z, col);
        }
    }
}

static void raster(MsmAdreno130State *s, A130Vtx *v, int n, uint32_t prim,
                   uint32_t flags)
{
    for (int i = 0; i < n; i++) {
        viewport(s, &v[i]);
        if (s->trace) {
            qemu_log("adreno130:   v%d window %.2f %.2f %.2f  rgba %.0f %.0f "
                     "%.0f %.0f  st %.3f %.3f\n", i, v[i].x, v[i].y, v[i].z,
                     v[i].c[0], v[i].c[1], v[i].c[2], v[i].c[3], v[i].s, v[i].t);
        }
    }
    switch (prim) {
    case 0x0c:                                  /* triangle list */
        for (int i = 0; i + 2 < n; i += 3) {
            raster_tri(s, &v[i], &v[i + 1], &v[i + 2], flags);
        }
        break;
    case 0x02:                                  /* fan (the clear quads) */
        for (int i = 1; i + 1 < n; i++) {
            raster_tri(s, &v[0], &v[i], &v[i + 1], flags);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "adreno130: primitive %02x\n", prim);
        break;
    }
}

/* the vertex pass: source stream -> transformed stream */
static void vertex_pass(MsmAdreno130State *s, int n)
{
    uint32_t fmt = s->regs[R_VTX_FMT_SRC], stride = fmt >> 24;
    const float *m = s->consts;
    uint32_t in[16], out[OUT_STRIDE];

    if (!stride || stride > 16) {
        return;
    }
    for (int i = 0; i < n; i++) {
        float x, y, z, st[2] = { 0, 0 };
        int k = 3;

        gpu_read(s, s->regs[R_STREAM_SRC] + 4 * stride * i, in, 4 * stride);
        x = f32(in[0]);
        y = f32(in[1]);
        z = f32(in[2]);
        if (fmt & 0x40000) {
            st[0] = f32(in[k++]);
            st[1] = f32(in[k++]);
        }
        memset(out, 0xff, sizeof(out));
        /* column-major, as GL hands it over */
        for (int r = 0; r < 4; r++) {
            out[r] = u32f(m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r]);
        }
        out[4] = fmt & 0x20000 ? in[k] : u32f(0) ;
        if (!(fmt & 0x20000)) {                 /* constant colour */
            uint32_t c = 0;

            for (int j = 0; j < 4; j++) {
                c |= (uint32_t)MIN(MAX(s->consts[0x2c + j], 0), 255) << (8 * j);
            }
            out[4] = c;
        }
        out[5] = u32f(st[0]);
        out[6] = u32f(st[1]);
        gpu_write(s, s->regs[R_STREAM_OUT] + 4 * OUT_STRIDE * i, out,
                  sizeof(out));
    }
}

static void draw(MsmAdreno130State *s, const uint32_t *p, uint32_t n)
{
    uint32_t w0 = p[0], prim = w0 & 0xff, flags = w0 >> 24;
    int cnt = MIN((w0 >> 16) & 0xff, MAX_VERTS);
    g_autofree A130Vtx *v = g_new0(A130Vtx, MAX(cnt, 1));

    s->draws++;
    if (s->trace) {
        qemu_log("adreno130: draw %08x mode %u, %u payload words\n", w0,
                 s->regs[R_DRAW_MODE], n);
    }
    if (n > 2) {                                /* inline {x y z ABGR} */
        cnt = MIN(cnt, (int)(n - 1) / 4);
        for (int i = 0; i < cnt; i++) {
            const uint32_t *q = p + 1 + 4 * i;

            v[i] = (A130Vtx){ f32(q[0]), f32(q[1]), f32(q[2]), 1 };
            abgr(q[3], v[i].c);
        }
        raster(s, v, cnt, prim, flags);
        return;
    }
    if (s->regs[R_DRAW_MODE] == 1) {
        vertex_pass(s, cnt);
        return;
    }
    for (int i = 0; i < cnt; i++) {
        uint32_t q[OUT_STRIDE];

        gpu_read(s, s->regs[R_STREAM_RAST] + 4 * OUT_STRIDE * i, q, sizeof(q));
        v[i] = (A130Vtx){ f32(q[0]), f32(q[1]), f32(q[2]), f32(q[3]),
                          f32(q[5]), f32(q[6]) };
        abgr(q[4], v[i].c);
    }
    raster(s, v, cnt, prim, flags);
}

/* ---------------------------------------------------------------- */
/* Command processor                                                 */
/* ---------------------------------------------------------------- */

static void reg_write(MsmAdreno130State *s, uint32_t r, uint32_t v);

static void run_packets(MsmAdreno130State *s, const uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n;) {
        uint32_t h = w[i], t = h >> 30, cnt = ((h >> 16) & 0x3fff) + 1;

        if (t == 0) {
            uint32_t r = h & 0x7fff;

            for (uint32_t k = 0; k < cnt && i + 1 + k < n; k++) {
                reg_write(s, h & 0x8000 ? r : r + k, w[i + 1 + k]);
            }
            i += 1 + cnt;
        } else if (t == 3) {
            uint32_t op = (h >> 8) & 0xff;

            if (op == 0x2a && i + cnt < n + 1) {
                draw(s, w + i + 1, MIN(cnt, n - i - 1));
            } else {
                qemu_log_mask(LOG_UNIMP, "adreno130: type-3 op %02x\n", op);
            }
            i += 1 + cnt;
        } else if (t == 1) {
            i += 3;
        } else {
            i++;                                /* type 2: filler */
        }
    }
}

static void run_ib(MsmAdreno130State *s, uint32_t addr, uint32_t ndw)
{
    g_autofree uint32_t *w = NULL;

    if (!ndw || ndw > 0x100000 || s->ib_depth > 3) {
        return;
    }
    w = g_new(uint32_t, ndw);
    if (!gpu_read(s, addr, w, 4 * ndw)) {
        return;
    }
    for (uint32_t i = 0; i < ndw; i++) {
        w[i] = le32_to_cpu(w[i]);
    }
    s->ib_depth++;
    run_packets(s, w, ndw);
    s->ib_depth--;
}

static void run_ring(MsmAdreno130State *s, uint32_t wptr)
{
    uint32_t size = 2u << (s->regs[R_RB_CNTL] & 0x1f);    /* dwords */
    uint32_t base = s->regs[R_RB_BASE];
    uint8_t wb[4];

    if (size > 0x100000) {
        return;
    }
    while (s->rptr != wptr % size) {
        uint32_t end = wptr % size >= s->rptr ? wptr % size : size;
        uint32_t n = end - s->rptr;
        g_autofree uint32_t *w = g_new(uint32_t, n);

        gpu_read(s, base + 4 * s->rptr, w, 4 * n);
        for (uint32_t i = 0; i < n; i++) {
            w[i] = le32_to_cpu(w[i]);
        }
        run_packets(s, w, n);
        s->rptr = end % size;
    }
    if (s->regs[R_RB_RPTR_ADDR]) {
        stl_le_p(wb, s->rptr);
        gpu_write(s, s->regs[R_RB_RPTR_ADDR], wb, 4);
    }
    if (s->regs[R_IRQ_EN] & IRQ_BIT) {
        s->regs[R_IRQ_STAT] |= IRQ_BIT;
        qemu_irq_pulse(s->irq);
    }
}

static void reg_write(MsmAdreno130State *s, uint32_t r, uint32_t v)
{
    if (r >= A130_NREGS) {
        return;
    }
    switch (r) {
    case R_CHIP_ID:
    case R_STATUS:
        return;
    case R_RB_BASE:
        s->regs[r] = v;
        s->rptr = 0;
        return;
    case R_RB_WPTR:
        s->regs[r] = v;
        run_ring(s, v);
        return;
    case R_IB_SIZE:
        s->regs[r] = v;
        run_ib(s, s->regs[R_IB_ADDR], v);
        return;
    case R_IRQ_STAT:
        s->regs[r] &= ~v;
        return;
    case R_CONST_SEL:
        s->regs[r] = v;
        s->const_idx = v ? (int)((v >> 16) & 0x3fff) : -1;   /* 0x4040 -> 0x40 */
        return;
    case R_CONST_DATA:
        if (s->const_idx >= 0 && s->const_idx < NCONST) {
            s->consts[s->const_idx++] = f32(v);
        }
        return;
    case R_VS_UCODE:
        if (s->vs_len < ARRAY_SIZE(s->vs_ucode)) {
            s->vs_ucode[s->vs_len++] = v;
        }
        return;
    }
    s->regs[r] = v;
}

/* ---------------------------------------------------------------- */
/* MMIO                                                              */
/* ---------------------------------------------------------------- */

static uint64_t a130_read(void *opaque, hwaddr off, unsigned size)
{
    MsmAdreno130State *s = opaque;

    if (off >= A130_GMEM_BASE) {
        return ldn_le_p(s->gmem + off - A130_GMEM_BASE, size);
    }
    if (off >= A130_REG_BASE) {
        uint32_t r = (off - A130_REG_BASE) / 4, v;

        switch (r) {
        case R_CHIP_ID:
            v = A130_CHIP_ID;
            break;
        case R_STATUS:
            v = A130_IDLE;
            break;
        default:
            v = s->regs[r];
            break;
        }
        return extract32(v, (off & 3) * 8, size * 8);
    }
    qemu_log_mask(LOG_UNIMP, "adreno130: read +%05" HWADDR_PRIx "\n", off);
    return 0;
}

static void a130_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmAdreno130State *s = opaque;

    if (off >= A130_GMEM_BASE) {
        stn_le_p(s->gmem + off - A130_GMEM_BASE, size, val);
        return;
    }
    if (off >= A130_REG_BASE && size == 4 && !(off & 3)) {
        reg_write(s, (off - A130_REG_BASE) / 4, val);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "adreno130: write +%05" HWADDR_PRIx " = %08"
                  PRIx64 " (%u)\n", off, val, size);
}

static const MemoryRegionOps a130_ops = {
    .read = a130_read,
    .write = a130_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void a130_reset(DeviceState *dev)
{
    MsmAdreno130State *s = MSM_ADRENO130(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->consts, 0, sizeof(s->consts));
    memset(s->gmem, 0, A130_GMEM_SIZE);
    memset(s->depth, 0, A130_GMEM_SIZE);
    s->const_idx = -1;
    s->vs_len = 0;
    s->rptr = 0;
}

static void a130_realize(DeviceState *dev, Error **errp)
{
    MsmAdreno130State *s = MSM_ADRENO130(dev);

    s->gmem = g_malloc0(A130_GMEM_SIZE);
    s->depth = g_malloc0(A130_GMEM_SIZE);       /* 16-bit per gmem pixel */
}

static void a130_init(Object *obj)
{
    MsmAdreno130State *s = MSM_ADRENO130(obj);

    memory_region_init_io(&s->iomem, obj, &a130_ops, s, "msm-adreno130",
                          A130_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property a130_props[] = {
    /* adreno130.ko's pools (pmem_gpu1 / pmem_gpu0) and where libhgl puts
     * them in GPU space */
    DEFINE_PROP_UINT32("ebi-phys", MsmAdreno130State, ebi_phys, 0x16500000),
    DEFINE_PROP_UINT32("ebi-gpu", MsmAdreno130State, ebi_gpu, 0x00100000),
    DEFINE_PROP_UINT32("ebi-size", MsmAdreno130State, ebi_size, 0x00800000),
    DEFINE_PROP_UINT32("smi-phys", MsmAdreno130State, smi_phys, 0x00000000),
    DEFINE_PROP_UINT32("smi-gpu", MsmAdreno130State, smi_gpu, 0x00900000),
    DEFINE_PROP_UINT32("smi-size", MsmAdreno130State, smi_size, 0x00700000),
    /* the render target's row length: only 64x64 pbuffers are captured */
    DEFINE_PROP_UINT32("fb-pitch", MsmAdreno130State, fb_pitch, 64),
    DEFINE_PROP_BOOL("trace", MsmAdreno130State, trace, false),
};

static void a130_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = a130_realize;
    device_class_set_legacy_reset(dc, a130_reset);
    device_class_set_props(dc, a130_props);
}

static const TypeInfo a130_info = {
    .name = TYPE_MSM_ADRENO130,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmAdreno130State),
    .instance_init = a130_init,
    .class_init = a130_class_init,
};

static void a130_register_types(void)
{
    type_register_static(&a130_info);
}

type_init(a130_register_types)
