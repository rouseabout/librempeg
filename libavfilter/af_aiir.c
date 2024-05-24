/*
 * Copyright (c) 2018 Paul B Mahol
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

#include <float.h>

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/xga_font_data.h"
#include "audio.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;

typedef struct Pair {
    int a, b;
} Pair;

typedef struct BiquadContext {
    double a[3];
    double b[3];
    double w1, w2;
} BiquadContext;

typedef struct IIRChannel {
    int nb_ab[2];
    double *ab[2];
    double g;
    double *cache[2];
    double fir;
    BiquadContext *biquads;
    int clippings;
} IIRChannel;

typedef struct AudioIIRContext {
    const AVClass *class;
    char **a_str;
    unsigned nb_a_str;
    char **b_str;
    unsigned nb_b_str;
    double dry_gain, wet_gain;
    double mix;
    double *gains;
    unsigned nb_gains;
    int normalize;
    int format;
    int process;
    int precision;

    IIRChannel *iir;
    int channels;
    enum AVSampleFormat sample_format;

    int (*iir_channel)(AVFilterContext *ctx, void *arg, int ch, int nb_jobs);
} AudioIIRContext;

static int query_formats(AVFilterContext *ctx)
{
    AudioIIRContext *s = ctx->priv;
    enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_DBLP,
        AV_SAMPLE_FMT_NONE
    };
    int ret;

    ret = ff_set_common_all_channel_counts(ctx);
    if (ret < 0)
        return ret;

    sample_fmts[0] = s->sample_format;
    ret = ff_set_common_formats_from_list(ctx, sample_fmts);
    if (ret < 0)
        return ret;

    return ff_set_common_all_samplerates(ctx);
}

#define IIR_CH(name, type, min, max, need_clipping)                     \
static int iir_ch_## name(AVFilterContext *ctx, void *arg, int ch, int nb_jobs)  \
{                                                                       \
    AudioIIRContext *s = ctx->priv;                                     \
    const double ig = s->dry_gain;                                      \
    const double og = s->wet_gain;                                      \
    const double mix = s->mix;                                          \
    ThreadData *td = arg;                                               \
    AVFrame *in = td->in, *out = td->out;                               \
    const type *src = (const type *)in->extended_data[ch];              \
    double *oc = (double *)s->iir[ch].cache[0];                         \
    double *ic = (double *)s->iir[ch].cache[1];                         \
    const int nb_a = s->iir[ch].nb_ab[0];                               \
    const int nb_b = s->iir[ch].nb_ab[1];                               \
    const double *a = s->iir[ch].ab[0];                                 \
    const double *b = s->iir[ch].ab[1];                                 \
    const double g = s->iir[ch].g;                                      \
    int *clippings = &s->iir[ch].clippings;                             \
    type *dst = (type *)out->extended_data[ch];                         \
    int n;                                                              \
                                                                        \
    for (n = 0; n < in->nb_samples; n++) {                              \
        double sample = 0.;                                             \
        int x;                                                          \
                                                                        \
        memmove(&ic[1], &ic[0], (nb_b - 1) * sizeof(*ic));              \
        memmove(&oc[1], &oc[0], (nb_a - 1) * sizeof(*oc));              \
        ic[0] = src[n] * ig;                                            \
        for (x = 0; x < nb_b; x++)                                      \
            sample += b[x] * ic[x];                                     \
                                                                        \
        for (x = 1; x < nb_a; x++)                                      \
            sample -= a[x] * oc[x];                                     \
                                                                        \
        oc[0] = sample;                                                 \
        sample *= og * g;                                               \
        sample = sample * mix + ic[0] * (1. - mix);                     \
        if (need_clipping && sample < min) {                            \
            (*clippings)++;                                             \
            dst[n] = min;                                               \
        } else if (need_clipping && sample > max) {                     \
            (*clippings)++;                                             \
            dst[n] = max;                                               \
        } else {                                                        \
            dst[n] = sample;                                            \
        }                                                               \
    }                                                                   \
                                                                        \
    return 0;                                                           \
}

IIR_CH(s16p, int16_t, INT16_MIN, INT16_MAX, 1)
IIR_CH(s32p, int32_t, INT32_MIN, INT32_MAX, 1)
IIR_CH(fltp, float,         -1.,        1., 0)
IIR_CH(dblp, double,        -1.,        1., 0)

#define SERIAL_IIR_CH(name, type, min, max, need_clipping)              \
static int iir_ch_serial_## name(AVFilterContext *ctx, void *arg,       \
                                 int ch, int nb_jobs)                   \
{                                                                       \
    AudioIIRContext *s = ctx->priv;                                     \
    const double ig = s->dry_gain;                                      \
    const double og = s->wet_gain;                                      \
    const double mix = s->mix;                                          \
    const double imix = 1. - mix;                                       \
    ThreadData *td = arg;                                               \
    AVFrame *in = td->in, *out = td->out;                               \
    const type *src = (const type *)in->extended_data[ch];              \
    type *dst = (type *)out->extended_data[ch];                         \
    IIRChannel *iir = &s->iir[ch];                                      \
    const double g = iir->g;                                            \
    int *clippings = &iir->clippings;                                   \
    int nb_biquads = (FFMAX(iir->nb_ab[0], iir->nb_ab[1]) + 1) / 2;     \
    int n, i;                                                           \
                                                                        \
    for (i = nb_biquads - 1; i >= 0; i--) {                             \
        const double a1 = -iir->biquads[i].a[1];                        \
        const double a2 = -iir->biquads[i].a[2];                        \
        const double b0 = iir->biquads[i].b[0];                         \
        const double b1 = iir->biquads[i].b[1];                         \
        const double b2 = iir->biquads[i].b[2];                         \
        double w1 = iir->biquads[i].w1;                                 \
        double w2 = iir->biquads[i].w2;                                 \
                                                                        \
        for (n = 0; n < in->nb_samples; n++) {                          \
            double i0 = ig * (i ? dst[n] : src[n]);                     \
            double o0 = i0 * b0 + w1;                                   \
                                                                        \
            w1 = b1 * i0 + w2 + a1 * o0;                                \
            w2 = b2 * i0 + a2 * o0;                                     \
            o0 *= og * g;                                               \
                                                                        \
            o0 = o0 * mix + imix * i0;                                  \
            if (need_clipping && o0 < min) {                            \
                (*clippings)++;                                         \
                dst[n] = min;                                           \
            } else if (need_clipping && o0 > max) {                     \
                (*clippings)++;                                         \
                dst[n] = max;                                           \
            } else {                                                    \
                dst[n] = o0;                                            \
            }                                                           \
        }                                                               \
        iir->biquads[i].w1 = w1;                                        \
        iir->biquads[i].w2 = w2;                                        \
    }                                                                   \
                                                                        \
    return 0;                                                           \
}

SERIAL_IIR_CH(s16p, int16_t, INT16_MIN, INT16_MAX, 1)
SERIAL_IIR_CH(s32p, int32_t, INT32_MIN, INT32_MAX, 1)
SERIAL_IIR_CH(fltp, float,         -1.,        1., 0)
SERIAL_IIR_CH(dblp, double,        -1.,        1., 0)

#define PARALLEL_IIR_CH(name, type, min, max, need_clipping)            \
static int iir_ch_parallel_## name(AVFilterContext *ctx, void *arg,     \
                                   int ch, int nb_jobs)                 \
{                                                                       \
    AudioIIRContext *s = ctx->priv;                                     \
    const double ig = s->dry_gain;                                      \
    const double og = s->wet_gain;                                      \
    const double mix = s->mix;                                          \
    const double imix = 1. - mix;                                       \
    ThreadData *td = arg;                                               \
    AVFrame *in = td->in, *out = td->out;                               \
    const type *src = (const type *)in->extended_data[ch];              \
    type *dst = (type *)out->extended_data[ch];                         \
    IIRChannel *iir = &s->iir[ch];                                      \
    const double g = iir->g;                                            \
    const double fir = iir->fir;                                        \
    int *clippings = &iir->clippings;                                   \
    int nb_biquads = (FFMAX(iir->nb_ab[0], iir->nb_ab[1]) + 1) / 2;     \
    int n, i;                                                           \
                                                                        \
    for (i = 0; i < nb_biquads; i++) {                                  \
        const double a1 = -iir->biquads[i].a[1];                        \
        const double a2 = -iir->biquads[i].a[2];                        \
        const double b1 = iir->biquads[i].b[1];                         \
        const double b2 = iir->biquads[i].b[2];                         \
        double w1 = iir->biquads[i].w1;                                 \
        double w2 = iir->biquads[i].w2;                                 \
                                                                        \
        for (n = 0; n < in->nb_samples; n++) {                          \
            double i0 = ig * src[n];                                    \
            double o0 = w1;                                             \
                                                                        \
            w1 = b1 * i0 + w2 + a1 * o0;                                \
            w2 = b2 * i0 + a2 * o0;                                     \
            o0 *= og * g;                                               \
            o0 += dst[n];                                               \
                                                                        \
            if (need_clipping && o0 < min) {                            \
                (*clippings)++;                                         \
                dst[n] = min;                                           \
            } else if (need_clipping && o0 > max) {                     \
                (*clippings)++;                                         \
                dst[n] = max;                                           \
            } else {                                                    \
                dst[n] = o0;                                            \
            }                                                           \
        }                                                               \
        iir->biquads[i].w1 = w1;                                        \
        iir->biquads[i].w2 = w2;                                        \
    }                                                                   \
                                                                        \
    for (n = 0; n < in->nb_samples; n++) {                              \
        dst[n] += fir * src[n];                                         \
        dst[n] = dst[n] * mix + imix * src[n];                          \
    }                                                                   \
                                                                        \
    return 0;                                                           \
}

PARALLEL_IIR_CH(s16p, int16_t, INT16_MIN, INT16_MAX, 1)
PARALLEL_IIR_CH(s32p, int32_t, INT32_MIN, INT32_MAX, 1)
PARALLEL_IIR_CH(fltp, float,         -1.,        1., 0)
PARALLEL_IIR_CH(dblp, double,        -1.,        1., 0)

#define LATTICE_IIR_CH(name, type, min, max, need_clipping)             \
static int iir_ch_lattice_## name(AVFilterContext *ctx, void *arg,      \
                                  int ch, int nb_jobs)                  \
{                                                                       \
    AudioIIRContext *s = ctx->priv;                                     \
    const double ig = s->dry_gain;                                      \
    const double og = s->wet_gain;                                      \
    const double mix = s->mix;                                          \
    ThreadData *td = arg;                                               \
    AVFrame *in = td->in, *out = td->out;                               \
    const type *src = (const type *)in->extended_data[ch];              \
    double n0, n1, p0, *x = (double *)s->iir[ch].cache[0];              \
    const int nb_stages = s->iir[ch].nb_ab[1];                          \
    const double *v = s->iir[ch].ab[0];                                 \
    const double *k = s->iir[ch].ab[1];                                 \
    const double g = s->iir[ch].g;                                      \
    int *clippings = &s->iir[ch].clippings;                             \
    type *dst = (type *)out->extended_data[ch];                         \
    int n;                                                              \
                                                                        \
    for (n = 0; n < in->nb_samples; n++) {                              \
        const double in = src[n] * ig;                                  \
        double out = 0.;                                                \
                                                                        \
        n1 = in;                                                        \
        for (int i = nb_stages - 1; i >= 0; i--) {                      \
            n0 = n1 - k[i] * x[i];                                      \
            p0 = n0 * k[i] + x[i];                                      \
            out += p0 * v[i+1];                                         \
            x[i] = p0;                                                  \
            n1 = n0;                                                    \
        }                                                               \
                                                                        \
        out += n1 * v[0];                                               \
        memmove(&x[1], &x[0], nb_stages * sizeof(*x));                  \
        x[0] = n1;                                                      \
        out *= og * g;                                                  \
        out = out * mix + in * (1. - mix);                              \
        if (need_clipping && out < min) {                               \
            (*clippings)++;                                             \
            dst[n] = min;                                               \
        } else if (need_clipping && out > max) {                        \
            (*clippings)++;                                             \
            dst[n] = max;                                               \
        } else {                                                        \
            dst[n] = out;                                               \
        }                                                               \
    }                                                                   \
                                                                        \
    return 0;                                                           \
}

LATTICE_IIR_CH(s16p, int16_t, INT16_MIN, INT16_MAX, 1)
LATTICE_IIR_CH(s32p, int32_t, INT32_MIN, INT32_MAX, 1)
LATTICE_IIR_CH(fltp, float,         -1.,        1., 0)
LATTICE_IIR_CH(dblp, double,        -1.,        1., 0)

static void count_coefficients(const char *item_str, int *nb_items)
{
    const char *p;

    if (!item_str)
        return;

    *nb_items = 1;
    for (p = item_str; *p && *p != '|'; p++) {
        if (*p == ' ')
            (*nb_items)++;
    }
}

static void read_gains(AVFilterContext *ctx, const double *gains,
                       const unsigned nb_gains, const int max_nb_gains)
{
    AudioIIRContext *s = ctx->priv;
    int n = 0;

    for (; n < nb_gains; n++) {
        if (n >= max_nb_gains)
            break;

        s->iir[n].g = gains[n];
    }

    for (; n < max_nb_gains; n++)
        s->iir[n].g = s->iir[n-1].g;
}

static int read_tf_coefficients(AVFilterContext *ctx, const char *item_str, int nb_items, double *dst)
{
    char *p, *arg, *old_str, *saveptr = NULL;
    int i;

    p = old_str = av_strdup(item_str);
    if (!p)
        return AVERROR(ENOMEM);
    for (i = 0; i < nb_items; i++) {
        if (!(arg = av_strtok(p, " ", &saveptr)))
            break;

        p = NULL;
        if (av_sscanf(arg, "%lf", &dst[i]) != 1) {
            av_log(ctx, AV_LOG_ERROR, "Invalid coefficients supplied: %s\n", arg);
            av_freep(&old_str);
            return AVERROR(EINVAL);
        }
    }

    av_freep(&old_str);

    return 0;
}

static int read_zp_coefficients(AVFilterContext *ctx, const char *item_str, int nb_items, double *dst, const char *format)
{
    char *p, *arg, *old_str, *saveptr = NULL;
    int i;

    p = old_str = av_strdup(item_str);
    if (!p)
        return AVERROR(ENOMEM);
    for (i = 0; i < nb_items; i++) {
        if (!(arg = av_strtok(p, " ", &saveptr)))
            break;

        p = NULL;
        if (av_sscanf(arg, format, &dst[i*2], &dst[i*2+1]) != 2) {
            av_log(ctx, AV_LOG_ERROR, "Invalid coefficients supplied: %s\n", arg);
            av_freep(&old_str);
            return AVERROR(EINVAL);
        }
    }

    av_freep(&old_str);

    return 0;
}

static const char *const format[] = { "%lf", "%lf %lfi", "%lf %lfr", "%lf %lfd", "%lf %lfi" };

static int read_channels(AVFilterContext *ctx, const char **const item_str, const unsigned nb_items,
                         const int channels, int ab)
{
    AudioIIRContext *s = ctx->priv;
    int i, ret;

    for (i = 0; i < channels; i++) {
        const int n = FFMIN(nb_items, channels-1);
        IIRChannel *iir = &s->iir[i];
        const char *arg = item_str[n];

        count_coefficients(arg, &iir->nb_ab[ab]);

        iir->cache[ab] = av_calloc(iir->nb_ab[ab] + 1, sizeof(double));
        iir->ab[ab] = av_calloc(iir->nb_ab[ab] * (!!s->format + 1), sizeof(double));
        if (!iir->ab[ab] || !iir->cache[ab])
            return AVERROR(ENOMEM);

        if (s->format > 0) {
            ret = read_zp_coefficients(ctx, arg, iir->nb_ab[ab], iir->ab[ab], format[s->format]);
        } else {
            ret = read_tf_coefficients(ctx, arg, iir->nb_ab[ab], iir->ab[ab]);
        }
        if (ret < 0)
            return ret;
    }

    return 0;
}

static void cmul(double re, double im, double re2, double im2, double *RE, double *IM)
{
    *RE = re * re2 - im * im2;
    *IM = re * im2 + re2 * im;
}

static int expand(AVFilterContext *ctx, double *pz, int n, double *coefs)
{
    coefs[2 * n] = 1.0;

    for (int i = 1; i <= n; i++) {
        for (int j = n - i; j < n; j++) {
            double re, im;

            cmul(coefs[2 * (j + 1)], coefs[2 * (j + 1) + 1],
                 pz[2 * (i - 1)], pz[2 * (i - 1) + 1], &re, &im);

            coefs[2 * j]     -= re;
            coefs[2 * j + 1] -= im;
        }
    }

    for (int i = 0; i < n + 1; i++) {
        if (fabs(coefs[2 * i + 1]) > FLT_EPSILON) {
            av_log(ctx, AV_LOG_ERROR, "coefs: %f of z^%d is not real; poles/zeros are not complex conjugates.\n",
                   coefs[2 * i + 1], i);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static void normalize_coeffs(AVFilterContext *ctx, int ch)
{
    AudioIIRContext *s = ctx->priv;
    IIRChannel *iir = &s->iir[ch];
    double sum_den = 0.;

    if (!s->normalize)
        return;

    for (int i = 0; i < iir->nb_ab[1]; i++) {
        sum_den += iir->ab[1][i];
    }

    if (sum_den > 1e-6) {
        double factor, sum_num = 0.;

        for (int i = 0; i < iir->nb_ab[0]; i++) {
            sum_num += iir->ab[0][i];
        }

        factor = sum_num / sum_den;

        for (int i = 0; i < iir->nb_ab[1]; i++) {
            iir->ab[1][i] *= factor;
        }
    }
}

static int convert_zp2tf(AVFilterContext *ctx, int channels)
{
    AudioIIRContext *s = ctx->priv;
    int ch, i, j, ret = 0;

    for (ch = 0; ch < channels; ch++) {
        IIRChannel *iir = &s->iir[ch];
        double *topc, *botc;

        topc = av_calloc((iir->nb_ab[1] + 1) * 2, sizeof(*topc));
        botc = av_calloc((iir->nb_ab[0] + 1) * 2, sizeof(*botc));
        if (!topc || !botc) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = expand(ctx, iir->ab[0], iir->nb_ab[0], botc);
        if (ret < 0) {
            goto fail;
        }

        ret = expand(ctx, iir->ab[1], iir->nb_ab[1], topc);
        if (ret < 0) {
            goto fail;
        }

        for (j = 0, i = iir->nb_ab[1]; i >= 0; j++, i--) {
            iir->ab[1][j] = topc[2 * i];
        }
        iir->nb_ab[1]++;

        for (j = 0, i = iir->nb_ab[0]; i >= 0; j++, i--) {
            iir->ab[0][j] = botc[2 * i];
        }
        iir->nb_ab[0]++;

        normalize_coeffs(ctx, ch);

fail:
        av_free(topc);
        av_free(botc);
        if (ret < 0)
            break;
    }

    return ret;
}

static int decompose_zp2biquads(AVFilterContext *ctx, int channels)
{
    AudioIIRContext *s = ctx->priv;
    int ch, ret;

    for (ch = 0; ch < channels; ch++) {
        IIRChannel *iir = &s->iir[ch];
        int nb_biquads = (FFMAX(iir->nb_ab[0], iir->nb_ab[1]) + 1) / 2;
        int current_biquad = 0;

        iir->biquads = av_calloc(nb_biquads, sizeof(BiquadContext));
        if (!iir->biquads)
            return AVERROR(ENOMEM);

        while (nb_biquads--) {
            Pair outmost_pole = { -1, -1 };
            Pair nearest_zero = { -1, -1 };
            double zeros[4] = { 0 };
            double poles[4] = { 0 };
            double b[6] = { 0 };
            double a[6] = { 0 };
            double min_distance = DBL_MAX;
            double max_mag = 0;
            double factor;
            int i;

            for (i = 0; i < iir->nb_ab[0]; i++) {
                double mag;

                if (isnan(iir->ab[0][2 * i]) || isnan(iir->ab[0][2 * i + 1]))
                    continue;
                mag = hypot(iir->ab[0][2 * i], iir->ab[0][2 * i + 1]);

                if (mag > max_mag) {
                    max_mag = mag;
                    outmost_pole.a = i;
                }
            }

            for (i = 0; i < iir->nb_ab[0]; i++) {
                if (isnan(iir->ab[0][2 * i]) || isnan(iir->ab[0][2 * i + 1]))
                    continue;

                if (iir->ab[0][2 * i    ] ==  iir->ab[0][2 * outmost_pole.a    ] &&
                    iir->ab[0][2 * i + 1] == -iir->ab[0][2 * outmost_pole.a + 1]) {
                    outmost_pole.b = i;
                    break;
                }
            }

            av_log(ctx, AV_LOG_VERBOSE, "outmost_pole is %d.%d\n", outmost_pole.a, outmost_pole.b);

            if (outmost_pole.a < 0 || outmost_pole.b < 0)
                return AVERROR(EINVAL);

            for (i = 0; i < iir->nb_ab[1]; i++) {
                double distance;

                if (isnan(iir->ab[1][2 * i]) || isnan(iir->ab[1][2 * i + 1]))
                    continue;
                distance = hypot(iir->ab[0][2 * outmost_pole.a    ] - iir->ab[1][2 * i    ],
                                 iir->ab[0][2 * outmost_pole.a + 1] - iir->ab[1][2 * i + 1]);

                if (distance < min_distance) {
                    min_distance = distance;
                    nearest_zero.a = i;
                }
            }

            for (i = 0; i < iir->nb_ab[1]; i++) {
                if (isnan(iir->ab[1][2 * i]) || isnan(iir->ab[1][2 * i + 1]))
                    continue;

                if (iir->ab[1][2 * i    ] ==  iir->ab[1][2 * nearest_zero.a    ] &&
                    iir->ab[1][2 * i + 1] == -iir->ab[1][2 * nearest_zero.a + 1]) {
                    nearest_zero.b = i;
                    break;
                }
            }

            av_log(ctx, AV_LOG_VERBOSE, "nearest_zero is %d.%d\n", nearest_zero.a, nearest_zero.b);

            if (nearest_zero.a < 0 || nearest_zero.b < 0)
                return AVERROR(EINVAL);

            poles[0] = iir->ab[0][2 * outmost_pole.a    ];
            poles[1] = iir->ab[0][2 * outmost_pole.a + 1];

            zeros[0] = iir->ab[1][2 * nearest_zero.a    ];
            zeros[1] = iir->ab[1][2 * nearest_zero.a + 1];

            if (nearest_zero.a == nearest_zero.b && outmost_pole.a == outmost_pole.b) {
                zeros[2] = 0;
                zeros[3] = 0;

                poles[2] = 0;
                poles[3] = 0;
            } else {
                poles[2] = iir->ab[0][2 * outmost_pole.b    ];
                poles[3] = iir->ab[0][2 * outmost_pole.b + 1];

                zeros[2] = iir->ab[1][2 * nearest_zero.b    ];
                zeros[3] = iir->ab[1][2 * nearest_zero.b + 1];
            }

            ret = expand(ctx, zeros, 2, b);
            if (ret < 0)
                return ret;

            ret = expand(ctx, poles, 2, a);
            if (ret < 0)
                return ret;

            iir->ab[0][2 * outmost_pole.a] = iir->ab[0][2 * outmost_pole.a + 1] = NAN;
            iir->ab[0][2 * outmost_pole.b] = iir->ab[0][2 * outmost_pole.b + 1] = NAN;
            iir->ab[1][2 * nearest_zero.a] = iir->ab[1][2 * nearest_zero.a + 1] = NAN;
            iir->ab[1][2 * nearest_zero.b] = iir->ab[1][2 * nearest_zero.b + 1] = NAN;

            iir->biquads[current_biquad].a[0] = 1.;
            iir->biquads[current_biquad].a[1] = a[2] / a[4];
            iir->biquads[current_biquad].a[2] = a[0] / a[4];
            iir->biquads[current_biquad].b[0] = b[4] / a[4];
            iir->biquads[current_biquad].b[1] = b[2] / a[4];
            iir->biquads[current_biquad].b[2] = b[0] / a[4];

            if (s->normalize &&
                fabs(iir->biquads[current_biquad].b[0] +
                     iir->biquads[current_biquad].b[1] +
                     iir->biquads[current_biquad].b[2]) > 1e-6) {
                factor = (iir->biquads[current_biquad].a[0] +
                          iir->biquads[current_biquad].a[1] +
                          iir->biquads[current_biquad].a[2]) /
                         (iir->biquads[current_biquad].b[0] +
                          iir->biquads[current_biquad].b[1] +
                          iir->biquads[current_biquad].b[2]);

                av_log(ctx, AV_LOG_VERBOSE, "factor=%f\n", factor);

                iir->biquads[current_biquad].b[0] *= factor;
                iir->biquads[current_biquad].b[1] *= factor;
                iir->biquads[current_biquad].b[2] *= factor;
            }

            iir->biquads[current_biquad].b[0] *= (current_biquad ? 1.0 : iir->g);
            iir->biquads[current_biquad].b[1] *= (current_biquad ? 1.0 : iir->g);
            iir->biquads[current_biquad].b[2] *= (current_biquad ? 1.0 : iir->g);

            av_log(ctx, AV_LOG_VERBOSE, "a=%f %f %f:b=%f %f %f\n",
                   iir->biquads[current_biquad].a[0],
                   iir->biquads[current_biquad].a[1],
                   iir->biquads[current_biquad].a[2],
                   iir->biquads[current_biquad].b[0],
                   iir->biquads[current_biquad].b[1],
                   iir->biquads[current_biquad].b[2]);

            current_biquad++;
        }
    }

    return 0;
}

static void biquad_process(double *x, double *y, int length,
                           double b0, double b1, double b2,
                           double a1, double a2)
{
    double w1 = 0., w2 = 0.;

    a1 = -a1;
    a2 = -a2;

    for (int n = 0; n < length; n++) {
        double out, in = x[n];

        y[n] = out = in * b0 + w1;
        w1 = b1 * in + w2 + a1 * out;
        w2 = b2 * in + a2 * out;
    }
}

static void solve(double *matrix, double *vector, int n, double *y, double *x, double *lu)
{
    double sum = 0.;

    for (int i = 0; i < n; i++) {
        for (int j = i; j < n; j++) {
            sum = 0.;
            for (int k = 0; k < i; k++)
                sum += lu[i * n + k] * lu[k * n + j];
            lu[i * n + j] = matrix[j * n + i] - sum;
        }
        for (int j = i + 1; j < n; j++) {
            sum = 0.;
            for (int k = 0; k < i; k++)
                sum += lu[j * n + k] * lu[k * n + i];
            lu[j * n + i] = (1. / lu[i * n + i]) * (matrix[i * n + j] - sum);
        }
    }

    for (int i = 0; i < n; i++) {
        sum = 0.;
        for (int k = 0; k < i; k++)
            sum += lu[i * n + k] * y[k];
        y[i] = vector[i] - sum;
    }

    for (int i = n - 1; i >= 0; i--) {
        sum = 0.;
        for (int k = i + 1; k < n; k++)
            sum += lu[i * n + k] * x[k];
        x[i] = (1 / lu[i * n + i]) * (y[i] - sum);
    }
}

static int convert_serial2parallel(AVFilterContext *ctx, int channels)
{
    AudioIIRContext *s = ctx->priv;

    for (int ch = 0; ch < channels; ch++) {
        IIRChannel *iir = &s->iir[ch];
        int nb_biquads = (FFMAX(iir->nb_ab[0], iir->nb_ab[1]) + 1) / 2;
        int length = nb_biquads * 2 + 1;
        double *impulse = av_calloc(length, sizeof(*impulse));
        double *y = av_calloc(length, sizeof(*y));
        double *resp = av_calloc(length, sizeof(*resp));
        double *M = av_calloc((length - 1) * nb_biquads, 2 * 2 * sizeof(*M));
        double *W;

        if (!impulse || !y || !resp || !M) {
            av_free(impulse);
            av_free(y);
            av_free(resp);
            av_free(M);
            return AVERROR(ENOMEM);
        }
        W = M + (length - 1) * 2 * nb_biquads;

        impulse[0] = 1.;

        for (int n = 0; n < nb_biquads; n++) {
            BiquadContext *biquad = &iir->biquads[n];

            biquad_process(n ? y : impulse, y, length,
                           biquad->b[0], biquad->b[1], biquad->b[2],
                           biquad->a[1], biquad->a[2]);
        }

        for (int n = 0; n < nb_biquads; n++) {
            BiquadContext *biquad = &iir->biquads[n];

            biquad_process(impulse, resp, length - 1,
                           1., 0., 0., biquad->a[1], biquad->a[2]);

            memcpy(M + n * 2 * (length - 1), resp, sizeof(*resp) * (length - 1));
            memcpy(M + n * 2 * (length - 1) + length, resp, sizeof(*resp) * (length - 2));
            memset(resp, 0, length * sizeof(*resp));
        }

        solve(M, &y[1], length - 1, &impulse[1], resp, W);

        iir->fir = y[0];

        for (int n = 0; n < nb_biquads; n++) {
            BiquadContext *biquad = &iir->biquads[n];

            biquad->b[0] = 0.;
            biquad->b[1] = resp[n * 2 + 0];
            biquad->b[2] = resp[n * 2 + 1];
        }

        av_free(impulse);
        av_free(y);
        av_free(resp);
        av_free(M);
    }

    return 0;
}

static void convert_pr2zp(AVFilterContext *ctx, int channels)
{
    AudioIIRContext *s = ctx->priv;
    int ch;

    for (ch = 0; ch < channels; ch++) {
        IIRChannel *iir = &s->iir[ch];
        int n;

        for (n = 0; n < iir->nb_ab[0]; n++) {
            double r = iir->ab[0][2*n];
            double angle = iir->ab[0][2*n+1];

            iir->ab[0][2*n]   = r * cos(angle);
            iir->ab[0][2*n+1] = r * sin(angle);
        }

        for (n = 0; n < iir->nb_ab[1]; n++) {
            double r = iir->ab[1][2*n];
            double angle = iir->ab[1][2*n+1];

            iir->ab[1][2*n]   = r * cos(angle);
            iir->ab[1][2*n+1] = r * sin(angle);
        }
    }
}

static void convert_sp2zp(AVFilterContext *ctx, int channels)
{
    AudioIIRContext *s = ctx->priv;
    int ch;

    for (ch = 0; ch < channels; ch++) {
        IIRChannel *iir = &s->iir[ch];
        int n;

        for (n = 0; n < iir->nb_ab[0]; n++) {
            double sr = iir->ab[0][2*n];
            double si = iir->ab[0][2*n+1];

            iir->ab[0][2*n]   = exp(sr) * cos(si);
            iir->ab[0][2*n+1] = exp(sr) * sin(si);
        }

        for (n = 0; n < iir->nb_ab[1]; n++) {
            double sr = iir->ab[1][2*n];
            double si = iir->ab[1][2*n+1];

            iir->ab[1][2*n]   = exp(sr) * cos(si);
            iir->ab[1][2*n+1] = exp(sr) * sin(si);
        }
    }
}

static double fact(double i)
{
    if (i <= 0.)
        return 1.;
    return i * fact(i - 1.);
}

static double coef_sf2zf(double *a, int N, int n)
{
    double z = 0.;

    for (int i = 0; i <= N; i++) {
        double acc = 0.;

        for (int k = FFMAX(n - N + i, 0); k <= FFMIN(i, n); k++) {
            acc += ((fact(i) * fact(N - i)) /
                    (fact(k) * fact(i - k) * fact(n - k) * fact(N - i - n + k))) *
                   ((k & 1) ? -1. : 1.);
        }

        z += a[i] * pow(2., i) * acc;
    }

    return z;
}

static void convert_sf2tf(AVFilterContext *ctx, int channels)
{
    AudioIIRContext *s = ctx->priv;
    int ch;

    for (ch = 0; ch < channels; ch++) {
        IIRChannel *iir = &s->iir[ch];
        double *temp0 = av_calloc(iir->nb_ab[0], sizeof(*temp0));
        double *temp1 = av_calloc(iir->nb_ab[1], sizeof(*temp1));

        if (!temp0 || !temp1)
            goto next;

        memcpy(temp0, iir->ab[0], iir->nb_ab[0] * sizeof(*temp0));
        memcpy(temp1, iir->ab[1], iir->nb_ab[1] * sizeof(*temp1));

        for (int n = 0; n < iir->nb_ab[0]; n++)
            iir->ab[0][n] = coef_sf2zf(temp0, iir->nb_ab[0] - 1, n);

        for (int n = 0; n < iir->nb_ab[1]; n++)
            iir->ab[1][n] = coef_sf2zf(temp1, iir->nb_ab[1] - 1, n);

next:
        av_free(temp0);
        av_free(temp1);
    }
}

static void convert_pd2zp(AVFilterContext *ctx, int channels)
{
    AudioIIRContext *s = ctx->priv;
    int ch;

    for (ch = 0; ch < channels; ch++) {
        IIRChannel *iir = &s->iir[ch];
        int n;

        for (n = 0; n < iir->nb_ab[0]; n++) {
            double r = iir->ab[0][2*n];
            double angle = M_PI*iir->ab[0][2*n+1]/180.;

            iir->ab[0][2*n]   = r * cos(angle);
            iir->ab[0][2*n+1] = r * sin(angle);
        }

        for (n = 0; n < iir->nb_ab[1]; n++) {
            double r = iir->ab[1][2*n];
            double angle = M_PI*iir->ab[1][2*n+1]/180.;

            iir->ab[1][2*n]   = r * cos(angle);
            iir->ab[1][2*n+1] = r * sin(angle);
        }
    }
}

static void check_stability(AVFilterContext *ctx, int channels)
{
    AudioIIRContext *s = ctx->priv;
    int ch;

    for (ch = 0; ch < channels; ch++) {
        IIRChannel *iir = &s->iir[ch];

        for (int n = 0; n < iir->nb_ab[0]; n++) {
            double pr = hypot(iir->ab[0][2*n], iir->ab[0][2*n+1]);

            if (pr >= 1.) {
                av_log(ctx, AV_LOG_WARNING, "pole %d at channel %d is unstable\n", n, ch);
                break;
            }
        }
    }
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioIIRContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    int ch, ret, i;

    s->channels = inlink->ch_layout.nb_channels;
    s->iir = av_calloc(s->channels, sizeof(*s->iir));
    if (!s->iir)
        return AVERROR(ENOMEM);

    read_gains(ctx, s->gains, s->nb_gains, inlink->ch_layout.nb_channels);
    ret = read_channels(ctx, (const char **const)s->a_str, s->nb_a_str, inlink->ch_layout.nb_channels, 0);
    if (ret < 0)
        return ret;

    ret = read_channels(ctx, (const char **const)s->b_str, s->nb_b_str, inlink->ch_layout.nb_channels, 1);
    if (ret < 0)
        return ret;

    if (s->format == -1) {
        convert_sf2tf(ctx, inlink->ch_layout.nb_channels);
        s->format = 0;
    } else if (s->format == 2) {
        convert_pr2zp(ctx, inlink->ch_layout.nb_channels);
    } else if (s->format == 3) {
        convert_pd2zp(ctx, inlink->ch_layout.nb_channels);
    } else if (s->format == 4) {
        convert_sp2zp(ctx, inlink->ch_layout.nb_channels);
    }
    if (s->format > 0) {
        check_stability(ctx, inlink->ch_layout.nb_channels);
    }

    if (s->format == 0)
        av_log(ctx, AV_LOG_WARNING, "transfer function coefficients format is not recommended for too high number of zeros/poles.\n");

    if (s->format > 0 && s->process == 0) {
        av_log(ctx, AV_LOG_WARNING, "Direct processing is not recommended for zp coefficients format.\n");

        ret = convert_zp2tf(ctx, inlink->ch_layout.nb_channels);
        if (ret < 0)
            return ret;
    } else if (s->format == -2 && s->process > 0) {
        av_log(ctx, AV_LOG_ERROR, "Only direct processing is implemented for lattice-ladder function.\n");
        return AVERROR_PATCHWELCOME;
    } else if (s->format <= 0 && s->process == 1) {
        av_log(ctx, AV_LOG_ERROR, "Serial processing is not implemented for transfer function.\n");
        return AVERROR_PATCHWELCOME;
    } else if (s->format <= 0 && s->process == 2) {
        av_log(ctx, AV_LOG_ERROR, "Parallel processing is not implemented for transfer function.\n");
        return AVERROR_PATCHWELCOME;
    } else if (s->format > 0 && s->process == 1) {
        ret = decompose_zp2biquads(ctx, inlink->ch_layout.nb_channels);
        if (ret < 0)
            return ret;
    } else if (s->format > 0 && s->process == 2) {
        if (s->precision > 1)
            av_log(ctx, AV_LOG_WARNING, "Parallel processing is not recommended for fixed-point precisions.\n");
        ret = decompose_zp2biquads(ctx, inlink->ch_layout.nb_channels);
        if (ret < 0)
            return ret;
        ret = convert_serial2parallel(ctx, inlink->ch_layout.nb_channels);
        if (ret < 0)
            return ret;
    }

    for (ch = 0; s->format == -2 && ch < inlink->ch_layout.nb_channels; ch++) {
        IIRChannel *iir = &s->iir[ch];

        if (iir->nb_ab[0] != iir->nb_ab[1] + 1) {
            av_log(ctx, AV_LOG_ERROR, "Number of ladder coefficients must be one more than number of reflection coefficients.\n");
            return AVERROR(EINVAL);
        }
    }

    for (ch = 0; s->format == 0 && ch < inlink->ch_layout.nb_channels; ch++) {
        IIRChannel *iir = &s->iir[ch];

        for (i = 1; i < iir->nb_ab[0]; i++) {
            iir->ab[0][i] /= iir->ab[0][0];
        }

        iir->ab[0][0] = 1.0;
        for (i = 0; i < iir->nb_ab[1]; i++) {
            iir->ab[1][i] *= iir->g;
        }

        normalize_coeffs(ctx, ch);
    }

    switch (inlink->format) {
    case AV_SAMPLE_FMT_DBLP: s->iir_channel = s->process == 2 ? iir_ch_parallel_dblp : s->process == 1 ? iir_ch_serial_dblp : iir_ch_dblp; break;
    case AV_SAMPLE_FMT_FLTP: s->iir_channel = s->process == 2 ? iir_ch_parallel_fltp : s->process == 1 ? iir_ch_serial_fltp : iir_ch_fltp; break;
    case AV_SAMPLE_FMT_S32P: s->iir_channel = s->process == 2 ? iir_ch_parallel_s32p : s->process == 1 ? iir_ch_serial_s32p : iir_ch_s32p; break;
    case AV_SAMPLE_FMT_S16P: s->iir_channel = s->process == 2 ? iir_ch_parallel_s16p : s->process == 1 ? iir_ch_serial_s16p : iir_ch_s16p; break;
    }

    if (s->format == -2) {
        switch (inlink->format) {
        case AV_SAMPLE_FMT_DBLP: s->iir_channel = iir_ch_lattice_dblp; break;
        case AV_SAMPLE_FMT_FLTP: s->iir_channel = iir_ch_lattice_fltp; break;
        case AV_SAMPLE_FMT_S32P: s->iir_channel = iir_ch_lattice_s32p; break;
        case AV_SAMPLE_FMT_S16P: s->iir_channel = iir_ch_lattice_s16p; break;
        }
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AudioIIRContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ThreadData td;
    AVFrame *out;
    int ch;

    if (av_frame_is_writable(in) && s->process != 2) {
        out = in;
    } else {
        out = ff_get_audio_buffer(outlink, in->nb_samples);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    td.in  = in;
    td.out = out;
    ff_filter_execute(ctx, s->iir_channel, &td, NULL, outlink->ch_layout.nb_channels);

    for (ch = 0; ch < outlink->ch_layout.nb_channels; ch++) {
        if (s->iir[ch].clippings > 0)
            av_log(ctx, AV_LOG_WARNING, "Channel %d clipping %d times. Please reduce gain.\n",
                   ch, s->iir[ch].clippings);
        s->iir[ch].clippings = 0;
    }

    if (in != out)
        av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

static av_cold int init(AVFilterContext *ctx)
{
    AudioIIRContext *s = ctx->priv;

    switch (s->precision) {
    case 0: s->sample_format = AV_SAMPLE_FMT_DBLP; break;
    case 1: s->sample_format = AV_SAMPLE_FMT_FLTP; break;
    case 2: s->sample_format = AV_SAMPLE_FMT_S32P; break;
    case 3: s->sample_format = AV_SAMPLE_FMT_S16P; break;
    default: return AVERROR_BUG;
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioIIRContext *s = ctx->priv;
    int ch;

    if (s->iir) {
        for (ch = 0; ch < s->channels; ch++) {
            IIRChannel *iir = &s->iir[ch];
            av_freep(&iir->ab[0]);
            av_freep(&iir->ab[1]);
            av_freep(&iir->cache[0]);
            av_freep(&iir->cache[1]);
            av_freep(&iir->biquads);
        }
    }
    av_freep(&s->iir);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_output,
    },
};

#define OFFSET(x) offsetof(AudioIIRContext, x)
#define AF AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define VF AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define AR AV_OPT_TYPE_FLAG_ARRAY

static const AVOptionArrayDef def_gains = {.def="1|1",.size_min=1,.sep='|'};
static const AVOptionArrayDef def_ab = {.def="1+0i 1-0i",.size_min=1,.sep='|'};

static const AVOption aiir_options[] = {
    { "zeros", "set B/numerator/zeros/reflection coefficients", OFFSET(b_str), AV_OPT_TYPE_STRING|AR, {.arr=&def_ab}, 0, 0, AF },
    { "z", "set B/numerator/zeros/reflection coefficients",     OFFSET(b_str), AV_OPT_TYPE_STRING|AR, {.arr=&def_ab}, 0, 0, AF },
    { "poles", "set A/denominator/poles/ladder coefficients",   OFFSET(a_str), AV_OPT_TYPE_STRING|AR, {.arr=&def_ab}, 0, 0, AF },
    { "p", "set A/denominator/poles/ladder coefficients",       OFFSET(a_str), AV_OPT_TYPE_STRING|AR, {.arr=&def_ab}, 0, 0, AF },
    { "gains", "set channels gains",               OFFSET(gains), AV_OPT_TYPE_DOUBLE|AR, {.arr=&def_gains}, -10, 10, AF },
    { "k", "set channels gains",                   OFFSET(gains), AV_OPT_TYPE_DOUBLE|AR, {.arr=&def_gains}, -10, 10, AF },
    { "dry", "set dry gain",                       OFFSET(dry_gain), AV_OPT_TYPE_DOUBLE, {.dbl=1},     0, 1, AF },
    { "wet", "set wet gain",                       OFFSET(wet_gain), AV_OPT_TYPE_DOUBLE, {.dbl=1},     0, 1, AF },
    { "format", "set coefficients format",         OFFSET(format),   AV_OPT_TYPE_INT,    {.i64=1},    -2, 4, AF, .unit = "format" },
    { "f", "set coefficients format",              OFFSET(format),   AV_OPT_TYPE_INT,    {.i64=1},    -2, 4, AF, .unit = "format" },
    { "ll", "lattice-ladder function",             0,                AV_OPT_TYPE_CONST,  {.i64=-2},    0, 0, AF, .unit = "format" },
    { "sf", "analog transfer function",            0,                AV_OPT_TYPE_CONST,  {.i64=-1},    0, 0, AF, .unit = "format" },
    { "tf", "digital transfer function",           0,                AV_OPT_TYPE_CONST,  {.i64=0},     0, 0, AF, .unit = "format" },
    { "zp", "Z-plane zeros/poles",                 0,                AV_OPT_TYPE_CONST,  {.i64=1},     0, 0, AF, .unit = "format" },
    { "pr", "Z-plane zeros/poles (polar radians)", 0,                AV_OPT_TYPE_CONST,  {.i64=2},     0, 0, AF, .unit = "format" },
    { "pd", "Z-plane zeros/poles (polar degrees)", 0,                AV_OPT_TYPE_CONST,  {.i64=3},     0, 0, AF, .unit = "format" },
    { "sp", "S-plane zeros/poles",                 0,                AV_OPT_TYPE_CONST,  {.i64=4},     0, 0, AF, .unit = "format" },
    { "process", "set kind of processing",         OFFSET(process),  AV_OPT_TYPE_INT,    {.i64=1},     0, 2, AF, .unit = "process" },
    { "r", "set kind of processing",               OFFSET(process),  AV_OPT_TYPE_INT,    {.i64=1},     0, 2, AF, .unit = "process" },
    { "d", "direct",                               0,                AV_OPT_TYPE_CONST,  {.i64=0},     0, 0, AF, .unit = "process" },
    { "s", "serial",                               0,                AV_OPT_TYPE_CONST,  {.i64=1},     0, 0, AF, .unit = "process" },
    { "p", "parallel",                             0,                AV_OPT_TYPE_CONST,  {.i64=2},     0, 0, AF, .unit = "process" },
    { "precision", "set filtering precision",      OFFSET(precision),AV_OPT_TYPE_INT,    {.i64=0},     0, 3, AF, .unit = "precision" },
    { "e", "set precision",                        OFFSET(precision),AV_OPT_TYPE_INT,    {.i64=0},     0, 3, AF, .unit = "precision" },
    { "dbl", "double-precision floating-point",    0,                AV_OPT_TYPE_CONST,  {.i64=0},     0, 0, AF, .unit = "precision" },
    { "flt", "single-precision floating-point",    0,                AV_OPT_TYPE_CONST,  {.i64=1},     0, 0, AF, .unit = "precision" },
    { "i32", "32-bit integers",                    0,                AV_OPT_TYPE_CONST,  {.i64=2},     0, 0, AF, .unit = "precision" },
    { "i16", "16-bit integers",                    0,                AV_OPT_TYPE_CONST,  {.i64=3},     0, 0, AF, .unit = "precision" },
    { "normalize", "normalize coefficients",       OFFSET(normalize),AV_OPT_TYPE_BOOL,   {.i64=1},     0, 1, AF },
    { "n", "normalize coefficients",               OFFSET(normalize),AV_OPT_TYPE_BOOL,   {.i64=1},     0, 1, AF },
    { "mix", "set mix",                            OFFSET(mix),      AV_OPT_TYPE_DOUBLE, {.dbl=1},     0, 1, AF },
    { NULL },
};

AVFILTER_DEFINE_CLASS(aiir);

const AVFilter ff_af_aiir = {
    .name          = "aiir",
    .description   = NULL_IF_CONFIG_SMALL("Apply Infinite Impulse Response filter with supplied coefficients."),
    .priv_size     = sizeof(AudioIIRContext),
    .priv_class    = &aiir_class,
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC(query_formats),
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC |
                     AVFILTER_FLAG_SLICE_THREADS,
};
