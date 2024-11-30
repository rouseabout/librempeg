/*
 * Copyright (C) 2001-2010 Krzysztof Foltman, Markus Schmidt, Thor Harald Johansen and others
 * Copyright (c) 2015 Paul B Mahol
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

/**
 * @file
 * Audio (Sidechain) Compressor filter
 */

#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "audio.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"

enum LinkMode {
    LINK_NONE,
    LINK_AVG,
    LINK_MAX,
    NB_LINK
};

typedef struct AudioCompressorContext {
    const AVClass *class;

    double level_in;
    double level_sc;
    double attack, attack_coeff;
    double release, release_coeff;
    double ratio;
    double threshold;
    double makeup;
    double mix;
    double thres;
    double knee;
    double knee_start;
    double knee_stop;
    double lin_knee_start;
    double lin_knee_stop;
    double adj_knee_start;
    double adj_knee_stop;
    double compressed_knee_start;
    double compressed_knee_stop;
    int link;
    int detection;
    int mode;
    int sidechain;

    void *lin_slope;

    AVFrame *in, *sc;

    void (*compress)(AVFilterContext *ctx, AVFrame *out, const int nb_samples,
                     AVFilterLink *inlink, AVFilterLink *sclink);
} AudioCompressorContext;

#define OFFSET(x) offsetof(AudioCompressorContext, x)
#define AFR AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM
#define AF AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption acompressor_options[] = {
    { "level_in",  "set input gain",     OFFSET(level_in),  AV_OPT_TYPE_DOUBLE, {.dbl=1},        0.015625,   64, AFR },
    { "mode",      "set mode",           OFFSET(mode),      AV_OPT_TYPE_INT,    {.i64=0},               0,    1, AFR, .unit = "mode" },
    {   "downward",0,                    0,                 AV_OPT_TYPE_CONST,  {.i64=0},               0,    0, AFR, .unit = "mode" },
    {   "upward",  0,                    0,                 AV_OPT_TYPE_CONST,  {.i64=1},               0,    0, AFR, .unit = "mode" },
    { "threshold", "set threshold",      OFFSET(threshold), AV_OPT_TYPE_DOUBLE, {.dbl=0.125}, 0.000976563,    1, AFR },
    { "ratio",     "set ratio",          OFFSET(ratio),     AV_OPT_TYPE_DOUBLE, {.dbl=2},               1,   20, AFR },
    { "attack",    "set attack",         OFFSET(attack),    AV_OPT_TYPE_DOUBLE, {.dbl=20},           0.01, 2000, AFR },
    { "release",   "set release",        OFFSET(release),   AV_OPT_TYPE_DOUBLE, {.dbl=250},          0.01, 9000, AFR },
    { "makeup",    "set make up gain",   OFFSET(makeup),    AV_OPT_TYPE_DOUBLE, {.dbl=1},               1,   64, AFR },
    { "knee",      "set knee",           OFFSET(knee),      AV_OPT_TYPE_DOUBLE, {.dbl=2.82843},         1,    8, AFR },
    { "link","set channels linking type",OFFSET(link),      AV_OPT_TYPE_INT,    {.i64=0},               0, NB_LINK-1, AFR, .unit = "link" },
    {   "none",    0,                    0,                 AV_OPT_TYPE_CONST,  {.i64=LINK_NONE},       0,    0, AFR, .unit = "link" },
    {   "average", 0,                    0,                 AV_OPT_TYPE_CONST,  {.i64=LINK_AVG},        0,    0, AFR, .unit = "link" },
    {   "maximum", 0,                    0,                 AV_OPT_TYPE_CONST,  {.i64=LINK_MAX},        0,    0, AFR, .unit = "link" },
    { "detection", "set detection",      OFFSET(detection), AV_OPT_TYPE_INT,    {.i64=1},               0,    1, AFR, .unit = "detection" },
    {   "peak",    0,                    0,                 AV_OPT_TYPE_CONST,  {.i64=0},               0,    0, AFR, .unit = "detection" },
    {   "rms",     0,                    0,                 AV_OPT_TYPE_CONST,  {.i64=1},               0,    0, AFR, .unit = "detection" },
    { "level_sc",  "set sidechain gain", OFFSET(level_sc),  AV_OPT_TYPE_DOUBLE, {.dbl=1},        0.015625,   64, AFR },
    { "mix",       "set mix",            OFFSET(mix),       AV_OPT_TYPE_DOUBLE, {.dbl=1},               0,    1, AFR },
    { "sidechain", "enable sidechain input",OFFSET(sidechain),AV_OPT_TYPE_BOOL, {.i64=0},               0,    1, AF },
    { NULL }
};

AVFILTER_DEFINE_CLASS(acompressor);

#define DEPTH 32
#include "acompressor_template.c"

#undef DEPTH
#define DEPTH 64
#include "acompressor_template.c"

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    AudioCompressorContext *s = ctx->priv;
    size_t sample_size;

    outlink->time_base = inlink->time_base;

    s->thres = log(s->threshold);
    s->lin_knee_start = s->threshold / sqrt(s->knee);
    s->lin_knee_stop = s->threshold * sqrt(s->knee);
    s->adj_knee_start = s->lin_knee_start * s->lin_knee_start;
    s->adj_knee_stop = s->lin_knee_stop * s->lin_knee_stop;
    s->knee_start = log(s->lin_knee_start);
    s->knee_stop = log(s->lin_knee_stop);
    s->compressed_knee_start = (s->knee_start - s->thres) / s->ratio + s->thres;
    s->compressed_knee_stop = (s->knee_stop - s->thres) / s->ratio + s->thres;

    s->attack_coeff = FFMIN(1., 1. / (s->attack * outlink->sample_rate / 4000.));
    s->release_coeff = FFMIN(1., 1. / (s->release * outlink->sample_rate / 4000.));

    switch (outlink->format) {
    case AV_SAMPLE_FMT_FLT:
        s->compress = compress_flt;
        sample_size = sizeof(float);
        break;
    case AV_SAMPLE_FMT_DBL:
        s->compress = compress_dbl;
        sample_size = sizeof(double);
        break;
    }

    if (!s->lin_slope)
        s->lin_slope = av_calloc(inlink->ch_layout.nb_channels, sample_size);
    if (!s->lin_slope)
        return AVERROR(ENOMEM);

    return 0;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    int ret;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    config_output(ctx->outputs[0]);

    return 0;
}

static int filter_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioCompressorContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *sclink = s->sidechain ? ctx->inputs[1] : inlink;
    AVFrame *out;

    if (av_frame_is_writable(s->in)) {
        out = s->in;
    } else {
        out = ff_get_audio_buffer(outlink, s->in->nb_samples);
        if (!out) {
            av_frame_free(&s->in);
            av_frame_free(&s->sc);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, s->in);
    }

    s->compress(ctx, out, s->in->nb_samples, inlink, sclink);

    if (out != s->in)
        av_frame_free(&s->in);
    s->in = NULL;
    av_frame_free(&s->sc);
    return ff_filter_frame(outlink, out);
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterLink *inlink = ctx->inputs[0];
    AudioCompressorContext *s = ctx->priv;

    FF_FILTER_FORWARD_STATUS_BACK_ALL(outlink, ctx);

    if (!s->in) {
        int ret = ff_inlink_consume_frame(inlink, &s->in);
        if (ret < 0)
            return ret;
    }

    if (s->in) {
        if (s->sidechain && !s->sc) {
            AVFilterLink *sclink = ctx->inputs[1];
            int ret = ff_inlink_consume_samples(sclink, s->in->nb_samples,
                                                s->in->nb_samples, &s->sc);
            if (ret < 0)
                return ret;

            if (!ret) {
                FF_FILTER_FORWARD_STATUS(sclink, outlink);
                FF_FILTER_FORWARD_WANTED(outlink, sclink);
                return 0;
            }
        }

        return filter_frame(outlink);
    }

    FF_FILTER_FORWARD_STATUS(inlink, outlink);
    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

static av_cold int init(AVFilterContext *ctx)
{
    AudioCompressorContext *s = ctx->priv;

    if (s->sidechain) {
        AVFilterPad pad = { NULL };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = "sidechain";
        return ff_append_inpad(ctx, &pad);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioCompressorContext *s = ctx->priv;

    av_frame_free(&s->in);
    av_frame_free(&s->sc);

    av_freep(&s->lin_slope);
}

static const AVFilterPad outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = config_output,
    },
};

const AVFilter ff_af_acompressor = {
    .name           = "acompressor",
    .description    = NULL_IF_CONFIG_SMALL("Audio compressor."),
    .priv_class     = &acompressor_class,
    .priv_size      = sizeof(AudioCompressorContext),
    .activate       = activate,
    .init           = init,
    .uninit         = uninit,
    FILTER_INPUTS(ff_audio_default_filterpad),
    FILTER_OUTPUTS(outputs),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_DBL),
    .process_command = process_command,
    .flags          = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL |
                      AVFILTER_FLAG_DYNAMIC_INPUTS,
};
