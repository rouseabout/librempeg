/*
 * AAC decoder
 * Copyright (c) 2005-2006 Oded Shimon ( ods15 ods15 dyndns org )
 * Copyright (c) 2006-2007 Maxim Gavrilov ( maxim.gavrilov gmail com )
 * Copyright (c) 2008-2013 Alex Converse <alex.converse@gmail.com>
 *
 * AAC LATM decoder
 * Copyright (c) 2008-2010 Paul Kendall <paul@kcbbs.gen.nz>
 * Copyright (c) 2010      Janne Grunau <janne-libav@jannau.net>
 *
 * AAC decoder fixed-point implementation
 * Copyright (c) 2013
 *      MIPS Technologies, Inc., California.
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

#include "libavcodec/aacdec.h"
#include "libavcodec/lpc_functions.h"

#include "libavcodec/aactab.h"

/**
 * Convert integer scalefactors to the decoder's native expected
 * scalefactor values.
 */
static void AAC_RENAME(dequant_scalefactors)(SingleChannelElement *sce)
{
    IndividualChannelStream *ics = &sce->ics;
    const enum BandType *band_type = sce->band_type;
    const int *band_type_run_end = sce->band_type_run_end;
    const int *sfo = sce->sfo;
    INTFLOAT *sf = sce->AAC_RENAME(sf);

    int g, i, idx = 0;
    for (g = 0; g < ics->num_window_groups; g++) {
        for (i = 0; i < ics->max_sfb;) {
            int run_end = band_type_run_end[idx];
            switch (band_type[idx]) {
            case ZERO_BT:
                for (; i < run_end; i++, idx++)
                    sf[idx] = FIXR(0.);
                break;
            case INTENSITY_BT: /* fallthrough */
            case INTENSITY_BT2:
                for (; i < run_end; i++, idx++) {
#if USE_FIXED
                    sf[idx] = 100 - sfo[idx];
#else
                    sf[idx] = ff_aac_pow2sf_tab[-sfo[idx] + POW_SF2_ZERO];
#endif /* USE_FIXED */
                }
                break;
            case NOISE_BT:
                for (; i < run_end; i++, idx++) {
#if USE_FIXED
                    sf[idx] = -(100 + sfo[idx]);
#else
                    sf[idx] = -ff_aac_pow2sf_tab[sfo[idx] + POW_SF2_ZERO];
#endif /* USE_FIXED */
                }
                break;
            default:
                for (; i < run_end; i++, idx++) {
#if USE_FIXED
                    sf[idx] = -sfo[idx];
#else
                    sf[idx] = -ff_aac_pow2sf_tab[sfo[idx] - 100 + POW_SF2_ZERO];
#endif /* USE_FIXED */
                }
                break;
            }
        }
    }
}

/**
 * Mid/Side stereo decoding; reference: 4.6.8.1.3.
 */
static void AAC_RENAME(apply_mid_side_stereo)(AACDecContext *ac, ChannelElement *cpe)
{
    const IndividualChannelStream *ics = &cpe->ch[0].ics;
    INTFLOAT *ch0 = cpe->ch[0].AAC_RENAME(coeffs);
    INTFLOAT *ch1 = cpe->ch[1].AAC_RENAME(coeffs);
    int g, i, group, idx = 0;
    const uint16_t *offsets = ics->swb_offset;
    for (g = 0; g < ics->num_window_groups; g++) {
        for (i = 0; i < ics->max_sfb; i++, idx++) {
            if (cpe->ms_mask[idx] &&
                cpe->ch[0].band_type[idx] < NOISE_BT &&
                cpe->ch[1].band_type[idx] < NOISE_BT) {
#if USE_FIXED
                for (group = 0; group < ics->group_len[g]; group++) {
                    ac->fdsp->butterflies_fixed(ch0 + group * 128 + offsets[i],
                                                ch1 + group * 128 + offsets[i],
                                                offsets[i+1] - offsets[i]);
#else
                for (group = 0; group < ics->group_len[g]; group++) {
                    ac->fdsp->butterflies_float(ch0 + group * 128 + offsets[i],
                                               ch1 + group * 128 + offsets[i],
                                               offsets[i+1] - offsets[i]);
#endif /* USE_FIXED */
                }
            }
        }
        ch0 += ics->group_len[g] * 128;
        ch1 += ics->group_len[g] * 128;
    }
}

/**
 * intensity stereo decoding; reference: 4.6.8.2.3
 *
 * @param   ms_present  Indicates mid/side stereo presence. [0] mask is all 0s;
 *                      [1] mask is decoded from bitstream; [2] mask is all 1s;
 *                      [3] reserved for scalable AAC
 */
static void AAC_RENAME(apply_intensity_stereo)(AACDecContext *ac,
                                               ChannelElement *cpe, int ms_present)
{
    const IndividualChannelStream *ics = &cpe->ch[1].ics;
    SingleChannelElement         *sce1 = &cpe->ch[1];
    INTFLOAT *coef0 = cpe->ch[0].AAC_RENAME(coeffs), *coef1 = cpe->ch[1].AAC_RENAME(coeffs);
    const uint16_t *offsets = ics->swb_offset;
    int g, group, i, idx = 0;
    int c;
    INTFLOAT scale;
    for (g = 0; g < ics->num_window_groups; g++) {
        for (i = 0; i < ics->max_sfb;) {
            if (sce1->band_type[idx] == INTENSITY_BT ||
                sce1->band_type[idx] == INTENSITY_BT2) {
                const int bt_run_end = sce1->band_type_run_end[idx];
                for (; i < bt_run_end; i++, idx++) {
                    c = -1 + 2 * (sce1->band_type[idx] - 14);
                    if (ms_present)
                        c *= 1 - 2 * cpe->ms_mask[idx];
                    scale = c * sce1->AAC_RENAME(sf)[idx];
                    for (group = 0; group < ics->group_len[g]; group++)
#if USE_FIXED
                        ac->subband_scale(coef1 + group * 128 + offsets[i],
                                      coef0 + group * 128 + offsets[i],
                                      scale,
                                      23,
                                      offsets[i + 1] - offsets[i] ,ac->avctx);
#else
                        ac->fdsp->vector_fmul_scalar(coef1 + group * 128 + offsets[i],
                                                    coef0 + group * 128 + offsets[i],
                                                    scale,
                                                    offsets[i + 1] - offsets[i]);
#endif /* USE_FIXED */
                }
            } else {
                int bt_run_end = sce1->band_type_run_end[idx];
                idx += bt_run_end - i;
                i    = bt_run_end;
            }
        }
        coef0 += ics->group_len[g] * 128;
        coef1 += ics->group_len[g] * 128;
    }
}

/**
 * Decode Temporal Noise Shaping filter coefficients and apply all-pole filters; reference: 4.6.9.3.
 *
 * @param   decode  1 if tool is used normally, 0 if tool is used in LTP.
 * @param   coef    spectral coefficients
 */
static void AAC_RENAME(apply_tns)(void *_coef_param, TemporalNoiseShaping *tns,
                                  IndividualChannelStream *ics, int decode)
{
    const int mmm = FFMIN(ics->tns_max_bands, ics->max_sfb);
    int w, filt, m, i;
    int bottom, top, order, start, end, size, inc;
    INTFLOAT *coef_param = _coef_param;
    INTFLOAT lpc[TNS_MAX_ORDER];
    INTFLOAT tmp[TNS_MAX_ORDER+1];
    UINTFLOAT *coef = coef_param;

    if(!mmm)
        return;

    for (w = 0; w < ics->num_windows; w++) {
        bottom = ics->num_swb;
        for (filt = 0; filt < tns->n_filt[w]; filt++) {
            top    = bottom;
            bottom = FFMAX(0, top - tns->length[w][filt]);
            order  = tns->order[w][filt];
            if (order == 0)
                continue;

            // tns_decode_coef
            compute_lpc_coefs(tns->AAC_RENAME(coef)[w][filt], order, lpc, 0, 0, 0);

            start = ics->swb_offset[FFMIN(bottom, mmm)];
            end   = ics->swb_offset[FFMIN(   top, mmm)];
            if ((size = end - start) <= 0)
                continue;
            if (tns->direction[w][filt]) {
                inc = -1;
                start = end - 1;
            } else {
                inc = 1;
            }
            start += w * 128;

            if (decode) {
                // ar filter
                for (m = 0; m < size; m++, start += inc)
                    for (i = 1; i <= FFMIN(m, order); i++)
                        coef[start] -= AAC_MUL26((INTFLOAT)coef[start - i * inc], lpc[i - 1]);
            } else {
                // ma filter
                for (m = 0; m < size; m++, start += inc) {
                    tmp[0] = coef[start];
                    for (i = 1; i <= FFMIN(m, order); i++)
                        coef[start] += AAC_MUL26(tmp[i], lpc[i - 1]);
                    for (i = order; i > 0; i--)
                        tmp[i] = tmp[i - 1];
                }
            }
        }
    }
}

/**
 * Apply the long term prediction
 */
static void AAC_RENAME(apply_ltp)(AACDecContext *ac, SingleChannelElement *sce)
{
    const LongTermPrediction *ltp = &sce->ics.ltp;
    const uint16_t *offsets = sce->ics.swb_offset;
    int i, sfb;

    if (sce->ics.window_sequence[0] != EIGHT_SHORT_SEQUENCE) {
        INTFLOAT *predTime = sce->AAC_RENAME(output);
        INTFLOAT *predFreq = ac->AAC_RENAME(buf_mdct);
        int16_t num_samples = 2048;

        if (ltp->lag < 1024)
            num_samples = ltp->lag + 1024;
        for (i = 0; i < num_samples; i++)
            predTime[i] = AAC_MUL30(sce->AAC_RENAME(ltp_state)[i + 2048 - ltp->lag], ltp->AAC_RENAME(coef));
        memset(&predTime[i], 0, (2048 - i) * sizeof(*predTime));

        ac->AAC_RENAME(windowing_and_mdct_ltp)(ac, predFreq, predTime, &sce->ics);

        if (sce->tns.present)
            AAC_RENAME(apply_tns)(predFreq, &sce->tns, &sce->ics, 0);

        for (sfb = 0; sfb < FFMIN(sce->ics.max_sfb, MAX_LTP_LONG_SFB); sfb++)
            if (ltp->used[sfb])
                for (i = offsets[sfb]; i < offsets[sfb + 1]; i++)
                    sce->AAC_RENAME(coeffs)[i] += (UINTFLOAT)predFreq[i];
    }
}

/**
 * Update the LTP buffer for next frame
 */
static void AAC_RENAME(update_ltp)(AACDecContext *ac, SingleChannelElement *sce)
{
    IndividualChannelStream *ics = &sce->ics;
    INTFLOAT *saved     = sce->AAC_RENAME(saved);
    INTFLOAT *saved_ltp = sce->AAC_RENAME(coeffs);
    const INTFLOAT *lwindow = ics->use_kb_window[0] ? AAC_RENAME2(aac_kbd_long_1024) : AAC_RENAME2(sine_1024);
    const INTFLOAT *swindow = ics->use_kb_window[0] ? AAC_RENAME2(aac_kbd_short_128) : AAC_RENAME2(sine_128);
    int i;

    if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
        memcpy(saved_ltp,       saved, 512 * sizeof(*saved_ltp));
        memset(saved_ltp + 576, 0,     448 * sizeof(*saved_ltp));
        ac->fdsp->vector_fmul_reverse(saved_ltp + 448, ac->AAC_RENAME(buf_mdct) + 960,     &swindow[64],      64);

        for (i = 0; i < 64; i++)
            saved_ltp[i + 512] = AAC_MUL31(ac->AAC_RENAME(buf_mdct)[1023 - i], swindow[63 - i]);
    } else if (1 && ics->window_sequence[0] == LONG_START_SEQUENCE) {
        memcpy(saved_ltp,       ac->AAC_RENAME(buf_mdct) + 512, 448 * sizeof(*saved_ltp));
        memset(saved_ltp + 576, 0,                  448 * sizeof(*saved_ltp));
        ac->fdsp->vector_fmul_reverse(saved_ltp + 448, ac->AAC_RENAME(buf_mdct) + 960,     &swindow[64],      64);

        for (i = 0; i < 64; i++)
            saved_ltp[i + 512] = AAC_MUL31(ac->AAC_RENAME(buf_mdct)[1023 - i], swindow[63 - i]);
    } else if (1) { // LONG_STOP or ONLY_LONG
        ac->fdsp->vector_fmul_reverse(saved_ltp, ac->AAC_RENAME(buf_mdct) + 512,     &lwindow[512],     512);

        for (i = 0; i < 512; i++)
            saved_ltp[i + 512] = AAC_MUL31(ac->AAC_RENAME(buf_mdct)[1023 - i], lwindow[511 - i]);
    }

    memcpy(sce->AAC_RENAME(ltp_state),      sce->AAC_RENAME(ltp_state)+1024,
           1024 * sizeof(*sce->AAC_RENAME(ltp_state)));
    memcpy(sce->AAC_RENAME(ltp_state) + 1024, sce->AAC_RENAME(output),
           1024 * sizeof(*sce->AAC_RENAME(ltp_state)));
    memcpy(sce->AAC_RENAME(ltp_state) + 2048, saved_ltp,
           1024 * sizeof(*sce->AAC_RENAME(ltp_state)));
}

const AACDecDSP AAC_RENAME(aac_dsp) = {
    .init_tables = &AAC_RENAME(init_tables),

    .dequant_scalefactors = &AAC_RENAME(dequant_scalefactors),
    .apply_mid_side_stereo = &AAC_RENAME(apply_mid_side_stereo),
    .apply_intensity_stereo = &AAC_RENAME(apply_intensity_stereo),
    .apply_tns = &AAC_RENAME(apply_tns),
    .apply_ltp = &AAC_RENAME(apply_ltp),
    .update_ltp = &AAC_RENAME(update_ltp),
};
