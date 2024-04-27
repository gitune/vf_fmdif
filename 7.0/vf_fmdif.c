/*
 * Field Match Deinterlacing Filter
 * Copyright (C) 2024 Tsunehisa Kazawa <digitune+ffmpeg@gmail.com>
 *
 * Based on vf_yadif:
 * Copyright (C) 2006-2011 Michael Niedermayer <michaelni@gmx.at>
 *               2010      James Darnley <james.darnley@gmail.com>
 *
 * Based on vf_fieldmatch:
 * Copyright (c) 2012 Fredrik Mellbin
 * Copyright (c) 2013 Clément Bœsch
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"
#include "yadif.h"

#define INPUT_MAIN     0

typedef struct FMDIFContext {
    YADIFContext yadif;
    int hsub[1], vsub[1];           ///< chroma subsampling values
    int bpc;                        ///< bytes per component
    int *last_match;                ///< last values of match
    int fid;                        ///< current frame id

    /* options */
    int cthresh;
    int chroma;
    int blockx, blocky;
    int combpel;
    int cycle;

    /* misc buffers */
    uint8_t *cmask_data[4];
    int cmask_linesize[4];
    int *c_array;
} FMDIFContext;

typedef struct ThreadData {
    AVFrame *frame;
    int plane;
    int w, h;
    int parity;
    int tff;
} ThreadData;

#define CHECK(j)\
    {   int score = FFABS(cur[mrefs - 1 + (j)] - cur[prefs - 1 - (j)])\
                  + FFABS(cur[mrefs  +(j)] - cur[prefs  -(j)])\
                  + FFABS(cur[mrefs + 1 + (j)] - cur[prefs + 1 - (j)]);\
        if (score < spatial_score) {\
            spatial_score= score;\
            spatial_pred= (cur[mrefs  +(j)] + cur[prefs  -(j)])>>1;\

/* The is_not_edge argument here controls when the code will enter a branch
 * which reads up to and including x-3 and x+3. */

#define FILTER(start, end, is_not_edge) \
    for (x = start;  x < end; x++) { \
        int c = cur[mrefs]; \
        int d = (prev2[0] + next2[0])>>1; \
        int e = cur[prefs]; \
        int temporal_diff0 = FFABS(prev2[0] - next2[0]); \
        int temporal_diff1 =(FFABS(prev[mrefs] - c) + FFABS(prev[prefs] - e) )>>1; \
        int temporal_diff2 =(FFABS(next[mrefs] - c) + FFABS(next[prefs] - e) )>>1; \
        int diff = FFMAX3(temporal_diff0 >> 1, temporal_diff1, temporal_diff2); \
        int spatial_pred = (c+e) >> 1; \
 \
        if (is_not_edge) {\
            int spatial_score = FFABS(cur[mrefs - 1] - cur[prefs - 1]) + FFABS(c-e) \
                              + FFABS(cur[mrefs + 1] - cur[prefs + 1]) - 1; \
            CHECK(-1) CHECK(-2) }} }} \
            CHECK( 1) CHECK( 2) }} }} \
        }\
 \
        if (!(mode&2)) { \
            int b = (prev2[2 * mrefs] + next2[2 * mrefs])>>1; \
            int f = (prev2[2 * prefs] + next2[2 * prefs])>>1; \
            int max = FFMAX3(d - e, d - c, FFMIN(b - c, f - e)); \
            int min = FFMIN3(d - e, d - c, FFMAX(b - c, f - e)); \
 \
            diff = FFMAX3(diff, min, -max); \
        } \
 \
        if (spatial_pred > d + diff) \
           spatial_pred = d + diff; \
        else if (spatial_pred < d - diff) \
           spatial_pred = d - diff; \
 \
        dst[0] = spatial_pred; \
 \
        dst++; \
        cur++; \
        prev++; \
        next++; \
        prev2++; \
        next2++; \
    }

static void filter_line_c(void *dst1,
                          void *prev1, void *cur1, void *next1,
                          int w, int prefs, int mrefs, int parity, int mode)
{
    uint8_t *dst  = dst1;
    uint8_t *prev = prev1;
    uint8_t *cur  = cur1;
    uint8_t *next = next1;
    int x;
    uint8_t *prev2 = parity ? prev : cur ;
    uint8_t *next2 = parity ? cur  : next;

    /* The function is called with the pointers already pointing to data[3] and
     * with 6 subtracted from the width.  This allows the FILTER macro to be
     * called so that it processes all the pixels normally.  A constant value of
     * true for is_not_edge lets the compiler ignore the if statement. */
    FILTER(0, w, 1)
}

#define MAX_ALIGN 8
static void filter_edges(void *dst1, void *prev1, void *cur1, void *next1,
                         int w, int prefs, int mrefs, int parity, int mode)
{
    uint8_t *dst  = dst1;
    uint8_t *prev = prev1;
    uint8_t *cur  = cur1;
    uint8_t *next = next1;
    int x;
    uint8_t *prev2 = parity ? prev : cur ;
    uint8_t *next2 = parity ? cur  : next;

    const int edge = MAX_ALIGN - 1;
    int offset = FFMAX(w - edge, 3);

    /* Only edge pixels need to be processed here.  A constant value of false
     * for is_not_edge should let the compiler ignore the whole branch. */
    FILTER(0, FFMIN(3, w), 0)

    dst  = (uint8_t*)dst1  + offset;
    prev = (uint8_t*)prev1 + offset;
    cur  = (uint8_t*)cur1  + offset;
    next = (uint8_t*)next1 + offset;
    prev2 = (uint8_t*)(parity ? prev : cur);
    next2 = (uint8_t*)(parity ? cur  : next);

    FILTER(offset, w - 3, 1)
    offset = FFMAX(offset, w - 3);
    FILTER(offset, w, 0)
}


static void filter_line_c_16bit(void *dst1,
                                void *prev1, void *cur1, void *next1,
                                int w, int prefs, int mrefs, int parity,
                                int mode)
{
    uint16_t *dst  = dst1;
    uint16_t *prev = prev1;
    uint16_t *cur  = cur1;
    uint16_t *next = next1;
    int x;
    uint16_t *prev2 = parity ? prev : cur ;
    uint16_t *next2 = parity ? cur  : next;
    mrefs /= 2;
    prefs /= 2;

    FILTER(0, w, 1)
}

static void filter_edges_16bit(void *dst1, void *prev1, void *cur1, void *next1,
                               int w, int prefs, int mrefs, int parity, int mode)
{
    uint16_t *dst  = dst1;
    uint16_t *prev = prev1;
    uint16_t *cur  = cur1;
    uint16_t *next = next1;
    int x;
    uint16_t *prev2 = parity ? prev : cur ;
    uint16_t *next2 = parity ? cur  : next;

    const int edge = MAX_ALIGN / 2 - 1;
    int offset = FFMAX(w - edge, 3);

    mrefs /= 2;
    prefs /= 2;

    FILTER(0,  FFMIN(3, w), 0)

    dst   = (uint16_t*)dst1  + offset;
    prev  = (uint16_t*)prev1 + offset;
    cur   = (uint16_t*)cur1  + offset;
    next  = (uint16_t*)next1 + offset;
    prev2 = (uint16_t*)(parity ? prev : cur);
    next2 = (uint16_t*)(parity ? cur  : next);

    FILTER(offset, w - 3, 1)
    offset = FFMAX(offset, w - 3);
    FILTER(offset, w, 0)
}

static int filter_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    FMDIFContext *fmdif = ctx->priv;
    YADIFContext *s = &fmdif->yadif;
    ThreadData *td  = arg;
    int refs = s->cur->linesize[td->plane];
    int df = (s->csp->comp[td->plane].depth + 7) / 8;
    int pix_3 = 3 * df;
    int slice_start = (td->h *  jobnr   ) / nb_jobs;
    int slice_end   = (td->h * (jobnr+1)) / nb_jobs;
    int y;
    int edge = 3 + MAX_ALIGN / df - 1;

    /* filtering reads 3 pixels to the left/right; to avoid invalid reads,
     * we need to call the c variant which avoids this for border pixels
     */
    for (y = slice_start; y < slice_end; y++) {
        if ((y ^ td->parity) & 1) {
            uint8_t *prev = &s->prev->data[td->plane][y * refs];
            uint8_t *cur  = &s->cur ->data[td->plane][y * refs];
            uint8_t *next = &s->next->data[td->plane][y * refs];
            uint8_t *dst  = &td->frame->data[td->plane][y * td->frame->linesize[td->plane]];
            int     mode  = y == 1 || y + 2 == td->h ? 2 : s->mode;
            s->filter_line(dst + pix_3, prev + pix_3, cur + pix_3,
                           next + pix_3, td->w - edge,
                           y + 1 < td->h ? refs : -refs,
                           y ? -refs : refs,
                           td->parity ^ td->tff, mode);
            s->filter_edges(dst, prev, cur, next, td->w,
                            y + 1 < td->h ? refs : -refs,
                            y ? -refs : refs,
                            td->parity ^ td->tff, mode);
        } else {
            memcpy(&td->frame->data[td->plane][y * td->frame->linesize[td->plane]],
                   &s->cur->data[td->plane][y * refs], td->w * df);
        }
    }
    return 0;
}

/* ================ field match ================ */

enum { mP, mC, mN };

static int get_width(const FMDIFContext *fm, const AVFrame *f, int plane)
{
    return plane ? AV_CEIL_RSHIFT(f->width, fm->hsub[INPUT_MAIN]) : f->width;
}

static int get_height(const FMDIFContext *fm, const AVFrame *f, int plane)
{
    return plane ? AV_CEIL_RSHIFT(f->height, fm->vsub[INPUT_MAIN]) : f->height;
}

static void copy_fields(const FMDIFContext *fm, AVFrame *dst,
                        const AVFrame *src, int field)
{
    int plane;
    for (plane = 0; plane < 4 && src->data[plane] && src->linesize[plane]; plane++) {
        const int plane_h = get_height(fm, src, plane);
        const int nb_copy_fields = (plane_h >> 1) + (field ? 0 : (plane_h & 1));
        av_image_copy_plane(dst->data[plane] + field*dst->linesize[plane], dst->linesize[plane] << 1,
                            src->data[plane] + field*src->linesize[plane], src->linesize[plane] << 1,
                            get_width(fm, src, plane) * fm->bpc, nb_copy_fields);
    }
}

static AVFrame *create_weave_frame(AVFilterContext *ctx, int match, int field,
                                   const AVFrame *prv, AVFrame *src, const AVFrame *nxt)
{
    AVFrame *dst;
    FMDIFContext *fm = ctx->priv;

    if (match == mC) {
        dst = av_frame_clone(src);
    } else {
        AVFilterLink *link = ctx->inputs[INPUT_MAIN];

        dst = ff_get_video_buffer(link, link->w, link->h);
        if (!dst)
            return NULL;
        av_frame_copy_props(dst, src);

        switch (match) {
        case mP: copy_fields(fm, dst, src, 1-field); copy_fields(fm, dst, prv, field); break;
        case mN: copy_fields(fm, dst, src, field); copy_fields(fm, dst, nxt, 1-field); break;
        default: av_assert0(0);
        }
    }
    return dst;
}

static void fill_buf(uint8_t *data, int w, int h, int linesize, uint8_t v)
{
    int y;

    for (y = 0; y < h; y++) {
        memset(data, v, w);
        data += linesize;
    }
}

static int calc_combed_score(const FMDIFContext *fm, const AVFrame *src)
{
    int x, y, plane, max_v = 0;
    const int cthresh = fm->cthresh;
    const int cthresh6 = cthresh * 6;

    for (plane = 0; plane < (fm->chroma ? 3 : 1); plane++) {
        const uint8_t *srcp = src->data[plane];
        const int src_linesize = src->linesize[plane];
        const int width  = get_width (fm, src, plane);
        const int height = get_height(fm, src, plane);
        uint8_t *cmkp = fm->cmask_data[plane];
        const int cmk_linesize = fm->cmask_linesize[plane];

        if (cthresh < 0) {
            fill_buf(cmkp, width, height, cmk_linesize, 0xff);
            continue;
        }
        fill_buf(cmkp, width, height, cmk_linesize, 0);

        /* [1 -3 4 -3 1] vertical filter */
#define CCFILTER(xm2, xm1, xp1, xp2) \
        abs(  4 * srcp[x] \
             -3 * (srcp[x + (xm1)*src_linesize] + srcp[x + (xp1)*src_linesize]) \
             +    (srcp[x + (xm2)*src_linesize] + srcp[x + (xp2)*src_linesize])) > cthresh6

        /* first line */
        for (x = 0; x < width; x++) {
            const int s1 = abs(srcp[x] - srcp[x + src_linesize]);
            if (s1 > cthresh && CCFILTER(2, 1, 1, 2))
                cmkp[x] = 0xff;
        }
        srcp += src_linesize;
        cmkp += cmk_linesize;

        /* second line */
        for (x = 0; x < width; x++) {
            const int s1 = abs(srcp[x] - srcp[x - src_linesize]);
            const int s2 = abs(srcp[x] - srcp[x + src_linesize]);
            if (s1 > cthresh && s2 > cthresh && CCFILTER(2, -1, 1, 2))
                cmkp[x] = 0xff;
        }
        srcp += src_linesize;
        cmkp += cmk_linesize;

        /* all lines minus first two and last two */
        for (y = 2; y < height-2; y++) {
            for (x = 0; x < width; x++) {
                const int s1 = abs(srcp[x] - srcp[x - src_linesize]);
                const int s2 = abs(srcp[x] - srcp[x + src_linesize]);
                if (s1 > cthresh && s2 > cthresh && CCFILTER(-2, -1, 1, 2))
                    cmkp[x] = 0xff;
            }
            srcp += src_linesize;
            cmkp += cmk_linesize;
        }

        /* before-last line */
        for (x = 0; x < width; x++) {
            const int s1 = abs(srcp[x] - srcp[x - src_linesize]);
            const int s2 = abs(srcp[x] - srcp[x + src_linesize]);
            if (s1 > cthresh && s2 > cthresh && CCFILTER(-2, -1, 1, -2))
                cmkp[x] = 0xff;
        }
        srcp += src_linesize;
        cmkp += cmk_linesize;

        /* last line */
        for (x = 0; x < width; x++) {
            const int s1 = abs(srcp[x] - srcp[x - src_linesize]);
            if (s1 > cthresh && CCFILTER(-2, -1, -1, -2))
                cmkp[x] = 0xff;
        }
    }

    if (fm->chroma) {
        uint8_t *cmkp  = fm->cmask_data[0];
        uint8_t *cmkpU = fm->cmask_data[1];
        uint8_t *cmkpV = fm->cmask_data[2];
        const int width  = AV_CEIL_RSHIFT(src->width,  fm->hsub[INPUT_MAIN]);
        const int height = AV_CEIL_RSHIFT(src->height, fm->vsub[INPUT_MAIN]);
        const int cmk_linesize   = fm->cmask_linesize[0] << 1;
        const int cmk_linesizeUV = fm->cmask_linesize[2];
        uint8_t *cmkpp  = cmkp - (cmk_linesize>>1);
        uint8_t *cmkpn  = cmkp + (cmk_linesize>>1);
        uint8_t *cmkpnn = cmkp +  cmk_linesize;
        for (y = 1; y < height - 1; y++) {
            cmkpp  += cmk_linesize;
            cmkp   += cmk_linesize;
            cmkpn  += cmk_linesize;
            cmkpnn += cmk_linesize;
            cmkpV  += cmk_linesizeUV;
            cmkpU  += cmk_linesizeUV;
            for (x = 1; x < width - 1; x++) {
#define HAS_FF_AROUND(p, lz) (p[(x)-1 - (lz)] == 0xff || p[(x) - (lz)] == 0xff || p[(x)+1 - (lz)] == 0xff || \
                              p[(x)-1       ] == 0xff ||                          p[(x)+1       ] == 0xff || \
                              p[(x)-1 + (lz)] == 0xff || p[(x) + (lz)] == 0xff || p[(x)+1 + (lz)] == 0xff)
                if ((cmkpV[x] == 0xff && HAS_FF_AROUND(cmkpV, cmk_linesizeUV)) ||
                    (cmkpU[x] == 0xff && HAS_FF_AROUND(cmkpU, cmk_linesizeUV))) {
                    ((uint16_t*)cmkp)[x]  = 0xffff;
                    ((uint16_t*)cmkpn)[x] = 0xffff;
                    if (y&1) ((uint16_t*)cmkpp)[x]  = 0xffff;
                    else     ((uint16_t*)cmkpnn)[x] = 0xffff;
                }
            }
        }
    }

    {
        const int blockx = fm->blockx;
        const int blocky = fm->blocky;
        const int xhalf = blockx/2;
        const int yhalf = blocky/2;
        const int cmk_linesize = fm->cmask_linesize[0];
        const uint8_t *cmkp    = fm->cmask_data[0] + cmk_linesize;
        const int width  = src->width;
        const int height = src->height;
        const int xblocks = ((width+xhalf)/blockx) + 1;
        const int xblocks4 = xblocks<<2;
        const int yblocks = ((height+yhalf)/blocky) + 1;
        int *c_array = fm->c_array;
        const int arraysize = (xblocks*yblocks)<<2;
        int      heighta = (height/(blocky/2))*(blocky/2);
        const int widtha = (width /(blockx/2))*(blockx/2);
        if (heighta == height)
            heighta = height - yhalf;
        memset(c_array, 0, arraysize * sizeof(*c_array));

#define C_ARRAY_ADD(v) do {                         \
    const int box1 = (x / blockx) * 4;              \
    const int box2 = ((x + xhalf) / blockx) * 4;    \
    c_array[temp1 + box1    ] += v;                 \
    c_array[temp1 + box2 + 1] += v;                 \
    c_array[temp2 + box1 + 2] += v;                 \
    c_array[temp2 + box2 + 3] += v;                 \
} while (0)

#define VERTICAL_HALF(y_start, y_end) do {                                  \
    for (y = y_start; y < y_end; y++) {                                     \
        const int temp1 = (y / blocky) * xblocks4;                          \
        const int temp2 = ((y + yhalf) / blocky) * xblocks4;                \
        for (x = 0; x < width; x++)                                         \
            if (cmkp[x - cmk_linesize] == 0xff &&                           \
                cmkp[x               ] == 0xff &&                           \
                cmkp[x + cmk_linesize] == 0xff)                             \
                C_ARRAY_ADD(1);                                             \
        cmkp += cmk_linesize;                                               \
    }                                                                       \
} while (0)

        VERTICAL_HALF(1, yhalf);

        for (y = yhalf; y < heighta; y += yhalf) {
            const int temp1 = (y / blocky) * xblocks4;
            const int temp2 = ((y + yhalf) / blocky) * xblocks4;

            for (x = 0; x < widtha; x += xhalf) {
                const uint8_t *cmkp_tmp = cmkp + x;
                int u, v, sum = 0;
                for (u = 0; u < yhalf; u++) {
                    for (v = 0; v < xhalf; v++)
                        if (cmkp_tmp[v - cmk_linesize] == 0xff &&
                            cmkp_tmp[v               ] == 0xff &&
                            cmkp_tmp[v + cmk_linesize] == 0xff)
                            sum++;
                    cmkp_tmp += cmk_linesize;
                }
                if (sum)
                    C_ARRAY_ADD(sum);
            }

            for (x = widtha; x < width; x++) {
                const uint8_t *cmkp_tmp = cmkp + x;
                int u, sum = 0;
                for (u = 0; u < yhalf; u++) {
                    if (cmkp_tmp[-cmk_linesize] == 0xff &&
                        cmkp_tmp[            0] == 0xff &&
                        cmkp_tmp[ cmk_linesize] == 0xff)
                        sum++;
                    cmkp_tmp += cmk_linesize;
                }
                if (sum)
                    C_ARRAY_ADD(sum);
            }
            cmkp += cmk_linesize * yhalf;
        }

        VERTICAL_HALF(heighta, height - 1);

        for (x = 0; x < arraysize; x++)
            if (c_array[x] > max_v)
                max_v = c_array[x];
    }
    return max_v;
}


static void filter(AVFilterContext *ctx, AVFrame *dstpic,
                   int parity, int tff)
{
    FMDIFContext *fm = ctx->priv;
    YADIFContext *yadif = &fm->yadif;
    int combs[] = { -1, -1, -1 };
    AVFrame *gen_frame = NULL;
    AVFrame *p1_frame;
    AVFrame *p2_frame;
    ThreadData td = { .frame = dstpic, .parity = parity, .tff = tff };
    int i, match = -1, p1, p2;
    int fnum = parity ^ !tff;

    /* the last matched frame is priority */
    switch (fm->last_match[fm->fid + (fm->cycle * fnum)]) {
    case mP:
    case mN:
        p1 = fnum * 2;
        p2 = mC;
        p1_frame = gen_frame = create_weave_frame(ctx, fnum * 2, tff, yadif->prev, yadif->cur, yadif->next);
        p2_frame = yadif->cur;
        if (gen_frame)
            break;
    case mC:
    default:
        p1 = mC;
        p2 = fnum * 2;
        p1_frame = yadif->cur;
        p2_frame = NULL;
        break;
    }

    /* calc combed scores */
    if ((combs[p1] = calc_combed_score(fm, p1_frame)) < fm->combpel) {
        match = p1;
        av_frame_copy(dstpic, p1_frame);
    } else {
        if (!p2_frame)
            p2_frame = gen_frame = create_weave_frame(ctx, fnum * 2, tff, yadif->prev, yadif->cur, yadif->next);
        if (p2_frame) {
            if ((combs[p2] = calc_combed_score(fm, p2_frame)) < fm->combpel) {
                match = p2;
                av_frame_copy(dstpic, p2_frame);
            }
        } else
            av_log(ctx, AV_LOG_WARNING, "Cannot create weave frame. skipped to match fields\n");
    }
    if (gen_frame)
        av_frame_free(&gen_frame);
    av_log(ctx, AV_LOG_DEBUG, "COMBS(%d): %3d %3d %3d:match=%d\n", fnum, combs[0], combs[1], combs[2], match);

    // keep the last match value in cycle
    fm->last_match[fm->fid + (fm->cycle * fnum)] = match;
    if (!fnum)
        if (++fm->fid >= fm->cycle)
            fm->fid = 0;

    if (match >= 0) /* found matched field */
        return;

    for (i = 0; i < yadif->csp->nb_components; i++) {
        int w = dstpic->width;
        int h = dstpic->height;

        if (i == 1 || i == 2) {
            w = AV_CEIL_RSHIFT(w, yadif->csp->log2_chroma_w);
            h = AV_CEIL_RSHIFT(h, yadif->csp->log2_chroma_h);
        }


        td.w       = w;
        td.h       = h;
        td.plane   = i;

        ff_filter_execute(ctx, filter_slice, &td, NULL,
                          FFMIN(h, ff_filter_get_nb_threads(ctx)));
    }
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FMDIFContext *fm = ctx->priv;
    YADIFContext *yadif = &fm->yadif;

    av_frame_free(&yadif->prev);
    av_frame_free(&yadif->cur );
    av_frame_free(&yadif->next);
    ff_ccfifo_uninit(&yadif->cc_fifo);

    av_freep(&fm->last_match);
    av_freep(&fm->cmask_data[0]);
    av_freep(&fm->c_array);
}

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_YUV420P,   AV_PIX_FMT_YUV422P,   AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV410P,   AV_PIX_FMT_YUV411P,   AV_PIX_FMT_YUV440P,
    AV_PIX_FMT_GRAY8,     AV_PIX_FMT_GRAY16,
    AV_PIX_FMT_YUVJ420P,  AV_PIX_FMT_YUVJ422P,  AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUVJ440P,
    AV_PIX_FMT_YUV420P9,  AV_PIX_FMT_YUV422P9,  AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_YUV420P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV444P14,
    AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
    AV_PIX_FMT_YUVA420P,  AV_PIX_FMT_YUVA422P,  AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_GBRP,      AV_PIX_FMT_GBRP9,     AV_PIX_FMT_GBRP10,
    AV_PIX_FMT_GBRP12,    AV_PIX_FMT_GBRP14,    AV_PIX_FMT_GBRP16,
    AV_PIX_FMT_GBRAP,
    AV_PIX_FMT_NONE
};

static int config_input(AVFilterLink *inlink)
{
    int ret;
    AVFilterContext *ctx = inlink->dst;
    FMDIFContext *fm = ctx->priv;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(inlink->format);
    const int w = inlink->w;
    const int h = inlink->h;

    if ((ret = av_image_alloc(fm->cmask_data, fm->cmask_linesize, w, h, inlink->format, 32)) < 0)
        return ret;

    fm->hsub[INPUT_MAIN] = pix_desc->log2_chroma_w;
    fm->vsub[INPUT_MAIN] = pix_desc->log2_chroma_h;
    fm->c_array = av_malloc_array((((w + fm->blockx/2)/fm->blockx)+1) *
                            (((h + fm->blocky/2)/fm->blocky)+1),
                            4 * sizeof(*fm->c_array));
    if (!fm->c_array)
        return AVERROR(ENOMEM);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    FMDIFContext *fmdif = ctx->priv;
    YADIFContext *s = &fmdif->yadif;
    const AVFilterLink *inlink = ctx->inputs[INPUT_MAIN];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    int i, ret;

    fmdif->bpc          = (desc->comp[0].depth + 7) / 8;
    fmdif->fid          = 0;
    fmdif->last_match   = av_malloc_array(fmdif->cycle * 2, sizeof(int));
    for (i = 0; i < fmdif->cycle * 2; i++)
        fmdif->last_match[i] = -1;

    ret = ff_yadif_config_output_common(outlink);
    if (ret < 0)
        return ret;

    s->csp = av_pix_fmt_desc_get(outlink->format);
    s->filter = filter;
    if (s->csp->comp[0].depth > 8) {
        s->filter_line  = filter_line_c_16bit;
        s->filter_edges = filter_edges_16bit;
    } else {
        s->filter_line  = filter_line_c;
        s->filter_edges = filter_edges;
    }

#if ARCH_X86
    ff_yadif_init_x86(s);
#endif

    return 0;
}

#define OFFSET(x) offsetof(YADIFContext, x)
#define OFFSET_FMDIF(x) offsetof(FMDIFContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

#define CONST(name, help, val, u) { name, help, 0, AV_OPT_TYPE_CONST, {.i64=val}, INT_MIN, INT_MAX, FLAGS, .unit = u }

const AVOption fmdif_options[] = {
    { "mode",   "specify the interlacing mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=YADIF_MODE_SEND_FRAME}, 0, 3, FLAGS, .unit = "mode"},
    CONST("send_frame",           "send one frame for each frame",                                     YADIF_MODE_SEND_FRAME,           "mode"),
    CONST("send_field",           "send one frame for each field",                                     YADIF_MODE_SEND_FIELD,           "mode"),
    CONST("send_frame_nospatial", "send one frame for each frame, but skip spatial interlacing check", YADIF_MODE_SEND_FRAME_NOSPATIAL, "mode"),
    CONST("send_field_nospatial", "send one frame for each field, but skip spatial interlacing check", YADIF_MODE_SEND_FIELD_NOSPATIAL, "mode"),

    { "parity", "specify the assumed picture field parity", OFFSET(parity), AV_OPT_TYPE_INT, {.i64=YADIF_PARITY_AUTO}, -1, 1, FLAGS, .unit = "parity" },
    CONST("tff",  "assume top field first",    YADIF_PARITY_TFF,  "parity"),
    CONST("bff",  "assume bottom field first", YADIF_PARITY_BFF,  "parity"),
    CONST("auto", "auto detect parity",        YADIF_PARITY_AUTO, "parity"),

    { "deint", "specify which frames to deinterlace", OFFSET(deint), AV_OPT_TYPE_INT, {.i64=YADIF_DEINT_ALL}, 0, 1, FLAGS, .unit = "deint" },
    CONST("all",        "deinterlace all frames",                       YADIF_DEINT_ALL,         "deint"),
    CONST("interlaced", "only deinterlace frames marked as interlaced", YADIF_DEINT_INTERLACED,  "deint"),

    { "cthresh", "set the area combing threshold used for combed frame detection",       OFFSET_FMDIF(cthresh), AV_OPT_TYPE_INT, {.i64=10}, -1, 0xff, FLAGS },
    { "chroma",  "set whether or not chroma is considered in the combed frame decision", OFFSET_FMDIF(chroma),  AV_OPT_TYPE_BOOL,{.i64= 1},  0,    1, FLAGS },
    { "blockx",  "set the x-axis size of the window used during combed frame detection", OFFSET_FMDIF(blockx),  AV_OPT_TYPE_INT, {.i64=16},  4, 1<<9, FLAGS },
    { "blocky",  "set the y-axis size of the window used during combed frame detection", OFFSET_FMDIF(blocky),  AV_OPT_TYPE_INT, {.i64=32},  4, 1<<9, FLAGS },
    { "combpel", "set the number of combed pixels inside any of the blocky by blockx size blocks on the frame for the frame to be detected as combed", OFFSET_FMDIF(combpel), AV_OPT_TYPE_INT, {.i64=160}, 0, INT_MAX, FLAGS },

    { "cycle",   "Set the number of frames you want to keep the rhythm", OFFSET_FMDIF(cycle), AV_OPT_TYPE_INT, {.i64 = 5}, 2, 25, FLAGS },

    { NULL }
};

AVFILTER_DEFINE_CLASS(fmdif);

static const AVFilterPad avfilter_vf_fmdif_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .filter_frame  = ff_yadif_filter_frame,
        .config_props  = config_input,
    },
};

static const AVFilterPad avfilter_vf_fmdif_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = ff_yadif_request_frame,
        .config_props  = config_output,
    },
};

const AVFilter ff_vf_fmdif = {
    .name          = "fmdif",
    .description   = NULL_IF_CONFIG_SMALL("Detelecine/Deinterlace the input image."),
    .priv_size     = sizeof(FMDIFContext),
    .priv_class    = &fmdif_class,
    .uninit        = uninit,
    FILTER_INPUTS(avfilter_vf_fmdif_inputs),
    FILTER_OUTPUTS(avfilter_vf_fmdif_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL | AVFILTER_FLAG_SLICE_THREADS,
};
