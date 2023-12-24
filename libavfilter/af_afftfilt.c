/*
 * Copyright (c) 2016 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License,
 * or (at your option) any later version.
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

#include "libavutil/avstring.h"
#include "libavfilter/internal.h"
#include "libavutil/common.h"
#include "libavutil/cpu.h"
#include "libavutil/opt.h"
#include "libavutil/eval.h"
#include "libavutil/tx.h"
#include "audio.h"
#include "filters.h"
#include "window_func.h"

typedef struct AFFTFiltContext {
    const AVClass *class;
    char *real_str;
    char *img_str;
    int tx_size;

    AVTXContext **tx, **itx;
    av_tx_fn tx_fn, itx_fn;
    AVFrame *tx_in, *tx_out, *tx_temp;
    int nb_exprs;
    int channels;
    int win_size;
    AVExpr **real;
    AVExpr **imag;
    int hop_size;
    float overlap;
    AVFrame *window;
    AVFrame *buffer;
    AVFrame *out;
    int win_func;
    double win_gain;
    float *window_func_lut;

    int (*tx_channels)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs);
    int (*filter_channels)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs);
} AFFTFiltContext;

static const char *const var_names[] = {            "sr",     "b",       "nb",        "ch",        "chs",   "pts",     "re",     "im", NULL };
enum                                   { VAR_SAMPLE_RATE, VAR_BIN, VAR_NBBINS, VAR_CHANNEL, VAR_CHANNELS, VAR_PTS, VAR_REAL, VAR_IMAG, VAR_VARS_NB };

#define OFFSET(x) offsetof(AFFTFiltContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption afftfilt_options[] = {
    { "real", "set channels real expressions",       OFFSET(real_str), AV_OPT_TYPE_STRING, {.str = "re" }, 0, 0, A },
    { "imag", "set channels imaginary expressions",  OFFSET(img_str),  AV_OPT_TYPE_STRING, {.str = "im" }, 0, 0, A },
    { "win_size", "set window size", OFFSET(tx_size), AV_OPT_TYPE_INT, {.i64=4096}, 16, 131072, A },
    WIN_FUNC_OPTION("win_func", OFFSET(win_func), A, WFUNC_HANNING),
    { "overlap", "set window overlap", OFFSET(overlap), AV_OPT_TYPE_FLOAT, {.dbl=0.75}, 0,  1, A },
    { NULL },
};

AVFILTER_DEFINE_CLASS(afftfilt);

static const char *const func2_names[]    = { "real", "imag", NULL };

#define DEPTH 32
#include "afftfilt_template.c"

#undef DEPTH
#define DEPTH 64
#include "afftfilt_template.c"

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    AFFTFiltContext *s = ctx->priv;
    char *saveptr = NULL;
    enum AVTXType tx_type;
    float overlap;
    char *args;
    const char *last_expr = "1";
    float scale_float = 1.f;
    double scale_double = 1.0;
    void *scale_ptr;
    int buf_size, ret = 0;

    switch (inlink->format) {
    case AV_SAMPLE_FMT_FLTP:
        scale_ptr = &scale_float;
        tx_type = AV_TX_FLOAT_RDFT;
        s->tx_channels = tx_channels_float;
        s->filter_channels = filter_channels_float;
        break;
    case AV_SAMPLE_FMT_DBLP:
        scale_ptr = &scale_double;
        tx_type = AV_TX_DOUBLE_RDFT;
        s->tx_channels = tx_channels_double;
        s->filter_channels = filter_channels_double;
        break;
    }

    s->channels = inlink->ch_layout.nb_channels;
    s->tx  = av_calloc(s->channels, sizeof(*s->tx));
    s->itx = av_calloc(s->channels, sizeof(*s->itx));
    if (!s->tx || !s->itx)
        return AVERROR(ENOMEM);

    for (int ch = 0; ch < s->channels; ch++) {
        ret = av_tx_init(&s->tx[ch], &s->tx_fn, tx_type, 0, s->tx_size, scale_ptr, 0);
        if (ret < 0)
            return ret;

        ret = av_tx_init(&s->itx[ch], &s->itx_fn, tx_type, 1, s->tx_size, scale_ptr, 0);
        if (ret < 0)
            return ret;
    }

    s->win_size = s->tx_size;
    buf_size = FFALIGN(s->win_size + 2, av_cpu_max_align());

    s->tx_in = ff_get_audio_buffer(inlink, buf_size);
    s->tx_out = ff_get_audio_buffer(inlink, buf_size);
    s->tx_temp = ff_get_audio_buffer(inlink, buf_size);
    if (!s->tx_in || !s->tx_out || !s->tx_temp)
        return AVERROR(ENOMEM);

    s->real = av_calloc(inlink->ch_layout.nb_channels, sizeof(*s->real));
    if (!s->real)
        return AVERROR(ENOMEM);

    s->imag = av_calloc(inlink->ch_layout.nb_channels, sizeof(*s->imag));
    if (!s->imag)
        return AVERROR(ENOMEM);

    args = av_strdup(s->real_str);
    if (!args)
        return AVERROR(ENOMEM);

    for (int ch = 0; ch < inlink->ch_layout.nb_channels; ch++) {
        char *arg = av_strtok(ch == 0 ? args : NULL, "|", &saveptr);

        ret = av_expr_parse(&s->real[ch], arg ? arg : last_expr, var_names,
                            NULL, NULL, func2_names,
                            inlink->format == AV_SAMPLE_FMT_FLTP ? func2_float : func2_double,
                            0, ctx);
        if (ret < 0)
            goto fail;
        if (arg)
            last_expr = arg;
        s->nb_exprs++;
    }

    av_freep(&args);

    args = av_strdup(s->img_str ? s->img_str : s->real_str);
    if (!args)
        return AVERROR(ENOMEM);

    saveptr = NULL;
    last_expr = "1";
    for (int ch = 0; ch < inlink->ch_layout.nb_channels; ch++) {
        char *arg = av_strtok(ch == 0 ? args : NULL, "|", &saveptr);

        ret = av_expr_parse(&s->imag[ch], arg ? arg : last_expr, var_names,
                            NULL, NULL, func2_names,
                            inlink->format == AV_SAMPLE_FMT_FLTP ? func2_float : func2_double,
                            0, ctx);
        if (ret < 0)
            goto fail;
        if (arg)
            last_expr = arg;
    }

    av_freep(&args);

    s->window_func_lut = av_realloc_f(s->window_func_lut, s->win_size,
                                      sizeof(*s->window_func_lut));
    if (!s->window_func_lut)
        return AVERROR(ENOMEM);
    generate_window_func(s->window_func_lut, s->win_size, s->win_func, &overlap);
    if (s->overlap == 1)
        s->overlap = overlap;

    s->hop_size = FFMAX(1, s->win_size * (1.f - s->overlap));

    s->window = ff_get_audio_buffer(inlink, s->win_size + 2);
    if (!s->window)
        return AVERROR(ENOMEM);

    s->buffer = ff_get_audio_buffer(inlink, s->win_size * 2);
    if (!s->buffer)
        return AVERROR(ENOMEM);

    {
        float max = 0.f, *temp_lut = av_calloc(s->win_size, sizeof(*temp_lut));
        if (!temp_lut)
            return AVERROR(ENOMEM);

        for (int j = 0; j < s->win_size; j += s->hop_size) {
            for (int i = 0; i < s->win_size; i++)
                temp_lut[(i + j) % s->win_size] += s->window_func_lut[i];
        }

        for (int i = 0; i < s->win_size; i++)
            max = fmaxf(temp_lut[i], max);
        av_freep(&temp_lut);

        s->win_gain = 1.0 / (max * s->win_size);
    }

fail:
    av_freep(&args);

    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AFFTFiltContext *s = ctx->priv;
    const int win_size = s->win_size;
    double values[VAR_VARS_NB];
    AVFrame *out;

    out = ff_get_audio_buffer(outlink, s->hop_size);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    values[VAR_PTS]         = in->pts;
    values[VAR_SAMPLE_RATE] = inlink->sample_rate;
    values[VAR_NBBINS]      = win_size / 2 + 1;
    values[VAR_CHANNELS]    = inlink->ch_layout.nb_channels;

    ff_filter_execute(ctx, s->tx_channels, in, NULL,
                      FFMIN(s->channels, ff_filter_get_nb_threads(ctx)));

    av_frame_copy_props(out, in);
    out->nb_samples = in->nb_samples;
    out->pts -= av_rescale_q(s->tx_size - s->hop_size, av_make_q(1, outlink->sample_rate), outlink->time_base);
    s->out = out;
    av_frame_free(&in);

    ff_filter_execute(ctx, s->filter_channels, values, NULL,
                      FFMIN(s->channels, ff_filter_get_nb_threads(ctx)));
    s->out = NULL;

    return ff_filter_frame(outlink, out);
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AFFTFiltContext *s = ctx->priv;
    AVFrame *in = NULL;
    int ret = 0, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    ret = ff_inlink_consume_samples(inlink, s->hop_size, s->hop_size, &in);
    if (ret < 0)
        return ret;

    if (ret > 0)
        ret = filter_frame(inlink, in);
    if (ret < 0)
        return ret;

    if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        ff_outlink_set_status(outlink, status, pts);
        return 0;
    }

    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AFFTFiltContext *s = ctx->priv;

    for (int i = 0; i < s->channels; i++) {
        if (s->itx)
            av_tx_uninit(&s->itx[i]);
        if (s->tx)
            av_tx_uninit(&s->tx[i]);
    }

    av_freep(&s->tx);
    av_freep(&s->itx);

    av_frame_free(&s->tx_in);
    av_frame_free(&s->tx_out);
    av_frame_free(&s->tx_temp);

    for (int i = 0; i < s->nb_exprs; i++) {
        av_expr_free(s->real[i]);
        av_expr_free(s->imag[i]);
    }

    av_freep(&s->real);
    av_freep(&s->imag);
    av_frame_free(&s->buffer);
    av_frame_free(&s->window);
    av_freep(&s->window_func_lut);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
    },
};

const AVFilter ff_af_afftfilt = {
    .name            = "afftfilt",
    .description     = NULL_IF_CONFIG_SMALL("Apply arbitrary expressions to samples in frequency domain."),
    .priv_size       = sizeof(AFFTFiltContext),
    .priv_class      = &afftfilt_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_DBLP),
    .activate        = activate,
    .uninit          = uninit,
    .flags           = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL |
                       AVFILTER_FLAG_SLICE_THREADS,
};
