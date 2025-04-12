/*
 * Field Match Deinterlacing Filter
 * Copyright (C) 2024 Tsunehisa Kazawa <digitune+ffmpeg@gmail.com>
 *
 * Based on BobWeaver Deinterlacing Filter
 * Copyright (C) 2016 Thomas Mundt <loudmax@yahoo.de>
 *
 * Based on YADIF (Yet Another Deinterlacing Filter)
 * Copyright (C) 2006-2011 Michael Niedermayer <michaelni@gmx.at>
 *               2010      James Darnley <james.darnley@gmail.com>
 *
 * With use of Weston 3 Field Deinterlacing Filter algorithm
 * Copyright (C) 2012 British Broadcasting Corporation, All Rights Reserved
 * Author of de-interlace algorithm: Jim Easterbrook for BBC R&D
 * Based on the process described by Martin Weston for BBC R&D
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
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "bwdifdsp.h"
#include "ccfifo.h"
#include "filters.h"
#include "video.h"
#include "yadif.h"

#define INPUT_MAIN     0

typedef struct BWDIFContext {
    YADIFContext yadif;
    BWDIFDSPContext dsp;
} BWDIFContext;

typedef struct FMDIF2Context {
    BWDIFContext bwdif;
    int hsub[1], vsub[1];           ///< chroma subsampling values
    int bpc;                        ///< bytes per component
    int *last_match;                ///< last values of match
    int fid;                        ///< current frame id
    int cur_combed_score;           ///< comb score of current frame
    AVFrame *weaved_frame;          ///< weaved frame with prev/next
    int wf_combed_score;            ///< comb score of weaved frame

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
} FMDIF2Context;

typedef struct ThreadData {
    AVFrame *frame;
    int plane;
    int w, h;
    int parity;
    int tff;
} ThreadData;

// Round job start line down to multiple of 4 so that if filter_line3 exists
// and the frame is a multiple of 4 high then filter_line will never be called
static inline int job_start(const int jobnr, const int nb_jobs, const int h)
{
    return jobnr >= nb_jobs ? h : ((h * jobnr) / nb_jobs) & ~3;
}

static int filter_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    FMDIF2Context *fm = ctx->priv;
    BWDIFContext *s = &fm->bwdif;
    YADIFContext *yadif = &s->yadif;
    ThreadData *td  = arg;
    int linesize = yadif->cur->linesize[td->plane];
    int clip_max = (1 << (yadif->csp->comp[td->plane].depth)) - 1;
    int df = (yadif->csp->comp[td->plane].depth + 7) / 8;
    int refs = linesize / df;
    int slice_start = job_start(jobnr, nb_jobs, td->h);
    int slice_end   = job_start(jobnr + 1, nb_jobs, td->h);
    int y;

    for (y = slice_start; y < slice_end; y++) {
        if ((y ^ td->parity) & 1) {
            uint8_t *prev = &yadif->prev->data[td->plane][y * linesize];
            uint8_t *cur  = &yadif->cur ->data[td->plane][y * linesize];
            uint8_t *next = &yadif->next->data[td->plane][y * linesize];
            uint8_t *dst  = &td->frame->data[td->plane][y * td->frame->linesize[td->plane]];
            if (yadif->current_field == YADIF_FIELD_END) {
                s->dsp.filter_intra(dst, cur, td->w, (y + df) < td->h ? refs : -refs,
                                y > (df - 1) ? -refs : refs,
                                (y + 3*df) < td->h ? 3 * refs : -refs,
                                y > (3*df - 1) ? -3 * refs : refs,
                                td->parity ^ td->tff, clip_max);
            } else if ((y < 4) || ((y + 5) > td->h)) {
                s->dsp.filter_edge(dst, prev, cur, next, td->w,
                               (y + df) < td->h ? refs : -refs,
                               y > (df - 1) ? -refs : refs,
                               refs << 1, -(refs << 1),
                               td->parity ^ td->tff, clip_max,
                               (y < 2) || ((y + 3) > td->h) ? 0 : 1);
            } else if (s->dsp.filter_line3 && y + 2 < slice_end && y + 6 < td->h) {
                s->dsp.filter_line3(dst, td->frame->linesize[td->plane],
                                prev, cur, next, linesize, td->w,
                                td->parity ^ td->tff, clip_max);
                y += 2;
            } else {
                s->dsp.filter_line(dst, prev, cur, next, td->w,
                               refs, -refs, refs << 1, -(refs << 1),
                               3 * refs, -3 * refs, refs << 2, -(refs << 2),
                               td->parity ^ td->tff, clip_max);
            }
        } else {
            memcpy(&td->frame->data[td->plane][y * td->frame->linesize[td->plane]],
                   &yadif->cur->data[td->plane][y * linesize], td->w * df);
        }
    }
    return 0;
}

/* ================ field match ================ */

enum { mP, mC, mN };

static int get_width(const FMDIF2Context *fm, const AVFrame *f, int plane)
{
    return plane ? AV_CEIL_RSHIFT(f->width, fm->hsub[INPUT_MAIN]) : f->width;
}

static int get_height(const FMDIF2Context *fm, const AVFrame *f, int plane)
{
    return plane ? AV_CEIL_RSHIFT(f->height, fm->vsub[INPUT_MAIN]) : f->height;
}

static void copy_fields(const FMDIF2Context *fm, AVFrame *dst,
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
    FMDIF2Context *fm = ctx->priv;

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

static int calc_combed_score(const FMDIF2Context *fm, const AVFrame *src)
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
    FMDIF2Context *fm = ctx->priv;
    BWDIFContext *bwdif = &fm->bwdif;
    YADIFContext *yadif = &bwdif->yadif;
    ThreadData td = { .frame = dstpic, .parity = parity, .tff = tff };
    int combs[] = { -1, -1, -1 };
    AVFrame *p1_frame;
    AVFrame *p2_frame;
    int i, match = -1, p1, p2, *last_match;
    int is_second = parity ^ !tff;

    /* prepare weaved frame and calc combed score */
    if (is_second) {
        if (fm->weaved_frame)
            av_frame_free(&fm->weaved_frame);
        fm->weaved_frame = create_weave_frame(ctx, mN, tff, yadif->prev, yadif->cur, yadif->next);
        if (fm->weaved_frame)
            combs[mN] = fm->wf_combed_score = calc_combed_score(fm, fm->weaved_frame);
        combs[mC] = fm->cur_combed_score;
    } else {
        if (!fm->weaved_frame) {
            fm->weaved_frame = create_weave_frame(ctx, mP, tff, yadif->prev, yadif->cur, yadif->next);
            if (fm->weaved_frame)
                combs[mP] = calc_combed_score(fm, fm->weaved_frame);
        } else {
            combs[mP] = fm->wf_combed_score;
        }
        combs[mC] = fm->cur_combed_score = calc_combed_score(fm, yadif->cur);
    }

    /* the last matched frame is priority */
    last_match = &fm->last_match[fm->fid + (fm->cycle * is_second)];
    switch (*last_match) {
    case mP:
    case mN:
        p1 = is_second ? mN : mP;
        p2 = mC;
        p1_frame = fm->weaved_frame;
        p2_frame = yadif->cur;
        if (p1_frame)
            break;
        /* continue if failing create weave frame */
    case mC:
    default:
        p1 = mC;
        p2 = is_second ? mN : mP;
        p1_frame = yadif->cur;
        p2_frame = fm->weaved_frame;
        break;
    }

    /* evaluate combed scores */
    if (combs[p1] < fm->combpel && *last_match >= 0) {
        match = p1;
        av_frame_copy(dstpic, p1_frame);
    } else {
        if (p2_frame) {
            /* if the last is unmatched, combpel should be half */
            int combpel = fm->combpel / (*last_match < 0 ? 2 : 1);
            /* if both are no comb, lower is better */
            if (combs[p1] < combpel && combs[p1] <= combs[p2]) {
                match = p1;
                av_frame_copy(dstpic, p1_frame);
            } else if (combs[p2] < combpel) {
                match = p2;
                av_frame_copy(dstpic, p2_frame);
            }
        } else
            av_log(ctx, AV_LOG_WARNING, "Cannot create weave frame. skipped to match fields\n");
    }
    av_log(ctx, AV_LOG_DEBUG, "COMBS(%d): %3d %3d %3d:match=%d\n", is_second, combs[0], combs[1], combs[2], match);

    /* keep the last match value in cycle */
    *last_match = match;
    if (!is_second)
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

        td.w     = w;
        td.h     = h;
        td.plane = i;

        ff_filter_execute(ctx, filter_slice, &td, NULL,
                          FFMIN((h+3)/4, ff_filter_get_nb_threads(ctx)));
    }
    if (yadif->current_field == YADIF_FIELD_END) {
        yadif->current_field = YADIF_FIELD_NORMAL;
    }
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FMDIF2Context *fm = ctx->priv;
    BWDIFContext *bw = &fm->bwdif;
    YADIFContext *yadif = &bw->yadif;

    av_frame_free(&yadif->prev);
    av_frame_free(&yadif->cur );
    av_frame_free(&yadif->next);
    ff_ccfifo_uninit(&yadif->cc_fifo);

    if (fm->weaved_frame)
        av_frame_free(&fm->weaved_frame);
    av_freep(&fm->last_match);
    av_freep(&fm->cmask_data[0]);
    av_freep(&fm->c_array);
}

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUVJ411P, AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_YUV420P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV444P14,
    AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
    AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_YUVA420P9, AV_PIX_FMT_YUVA422P9, AV_PIX_FMT_YUVA444P9,
    AV_PIX_FMT_YUVA420P10, AV_PIX_FMT_YUVA422P10, AV_PIX_FMT_YUVA444P10,
    AV_PIX_FMT_YUVA420P16, AV_PIX_FMT_YUVA422P16, AV_PIX_FMT_YUVA444P16,
    AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRP9, AV_PIX_FMT_GBRP10,
    AV_PIX_FMT_GBRP12, AV_PIX_FMT_GBRP14, AV_PIX_FMT_GBRP16,
    AV_PIX_FMT_GBRAP, AV_PIX_FMT_GBRAP16,
    AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY16,
    AV_PIX_FMT_NONE
};

static int config_input(AVFilterLink *inlink)
{
    int ret;
    AVFilterContext *ctx = inlink->dst;
    FMDIF2Context *fm = ctx->priv;
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
    FMDIF2Context *fm = ctx->priv;
    BWDIFContext *bw = &fm->bwdif;
    YADIFContext *s = &bw->yadif;
    const AVFilterLink *inlink = ctx->inputs[INPUT_MAIN];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    int i, ret;

    ret = ff_yadif_config_output_common(outlink);
    if (ret < 0)
        return AVERROR(EINVAL);

    s->csp = av_pix_fmt_desc_get(outlink->format);
    s->filter = filter;

    if (AV_CEIL_RSHIFT(outlink->w, s->csp->log2_chroma_w) < 3 || AV_CEIL_RSHIFT(outlink->h, s->csp->log2_chroma_h) < 4) {
        av_log(ctx, AV_LOG_ERROR, "Video with planes less than 3 columns or 4 lines is not supported\n");
        return AVERROR(EINVAL);
    }

    ff_bwdif_init_filter_line(&bw->dsp, s->csp->comp[0].depth);

    fm->bpc          = (desc->comp[0].depth + 7) / 8;
    fm->fid          = 0;
    fm->last_match   = av_malloc_array(fm->cycle * 2, sizeof(int));
    for (i = 0; i < fm->cycle * 2; i++)
        fm->last_match[i] = -1;
    fm->cur_combed_score = 0;
    fm->weaved_frame = NULL;
    fm->wf_combed_score = 0;

#if ARCH_X86
    ff_yadif_init_x86(s);
#endif

    return 0;
}

#define OFFSET(x) offsetof(YADIFContext, x)
#define OFFSET_FMDIF2(x) offsetof(FMDIF2Context, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

#define CONST(name, help, val, u) { name, help, 0, AV_OPT_TYPE_CONST, {.i64=val}, INT_MIN, INT_MAX, FLAGS, .unit = u }

static const AVOption fmdif2_options[] = {
    { "mode",   "specify the interlacing mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=YADIF_MODE_SEND_FIELD}, 0, 1, FLAGS, .unit = "mode"},
    CONST("send_frame", "send one frame for each frame", YADIF_MODE_SEND_FRAME, "mode"),
    CONST("send_field", "send one frame for each field", YADIF_MODE_SEND_FIELD, "mode"),

    { "parity", "specify the assumed picture field parity", OFFSET(parity), AV_OPT_TYPE_INT, {.i64=YADIF_PARITY_AUTO}, -1, 1, FLAGS, .unit = "parity" },
    CONST("tff",  "assume top field first",    YADIF_PARITY_TFF,  "parity"),
    CONST("bff",  "assume bottom field first", YADIF_PARITY_BFF,  "parity"),
    CONST("auto", "auto detect parity",        YADIF_PARITY_AUTO, "parity"),

    { "deint", "specify which frames to deinterlace", OFFSET(deint), AV_OPT_TYPE_INT, {.i64=YADIF_DEINT_ALL}, 0, 1, FLAGS, .unit = "deint" },
    CONST("all",        "deinterlace all frames",                       YADIF_DEINT_ALL,        "deint"),
    CONST("interlaced", "only deinterlace frames marked as interlaced", YADIF_DEINT_INTERLACED, "deint"),

    { "cthresh",  "set the area combing threshold used for combed frame detection",       OFFSET_FMDIF2(cthresh), AV_OPT_TYPE_INT, {.i64= 9}, -1, 0xff, FLAGS },
    { "chroma",   "set whether or not chroma is considered in the combed frame decision", OFFSET_FMDIF2(chroma),  AV_OPT_TYPE_BOOL,{.i64= 1},  0,    1, FLAGS },
    { "blockx",   "set the x-axis size of the window used during combed frame detection", OFFSET_FMDIF2(blockx),  AV_OPT_TYPE_INT, {.i64=16},  4, 1<<9, FLAGS },
    { "blocky",   "set the y-axis size of the window used during combed frame detection", OFFSET_FMDIF2(blocky),  AV_OPT_TYPE_INT, {.i64=32},  4, 1<<9, FLAGS },
    { "combpel",  "set the number of combed pixels inside any of the blocky by blockx size blocks on the frame for the frame to be detected as combed", OFFSET_FMDIF2(combpel), AV_OPT_TYPE_INT, {.i64=200}, 0, INT_MAX, FLAGS },
    { "cycle",    "set the number of frames you want to keep the rhythm", OFFSET_FMDIF2(cycle), AV_OPT_TYPE_INT, {.i64 = 5}, 2, 25, FLAGS },

    { NULL }
};

AVFILTER_DEFINE_CLASS(fmdif2);

static const AVFilterPad avfilter_vf_fmdif2_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .filter_frame  = ff_yadif_filter_frame,
        .config_props  = config_input,
    },
};

static const AVFilterPad avfilter_vf_fmdif2_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = ff_yadif_request_frame,
        .config_props  = config_output,
    },
};

const AVFilter ff_vf_fmdif2 = {
    .name          = "fmdif2",
    .description   = NULL_IF_CONFIG_SMALL("Detelecine/Deinterlace the input image."),
    .priv_size     = sizeof(FMDIF2Context),
    .priv_class    = &fmdif2_class,
    .uninit        = uninit,
    FILTER_INPUTS(avfilter_vf_fmdif2_inputs),
    FILTER_OUTPUTS(avfilter_vf_fmdif2_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL | AVFILTER_FLAG_SLICE_THREADS,
};
