/*
** FAAD - Freeware Advanced Audio Decoder
** Copyright (C) 2002 M. Bakker
**  
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
** 
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
** 
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software 
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** $Id: decoder.c,v 1.5 2003/08/25 21:51:41 f1rmb Exp $
**/

#include "common.h"
#include "structs.h"

#include <stdlib.h>
#include <string.h>

#include "decoder.h"
#include "mp4.h"
#include "syntax.h"
#include "specrec.h"
#include "data.h"
#include "tns.h"
#include "pns.h"
#include "is.h"
#include "ms.h"
#include "ic_predict.h"
#include "lt_predict.h"
#include "drc.h"
#include "error.h"
#include "output.h"
#include "dither.h"
#ifdef SSR_DEC
#include "ssr.h"
#include "ssr_fb.h"
#endif

#ifdef ANALYSIS
uint16_t dbg_count;
#endif

int8_t* FAADAPI faacDecGetErrorMessage(uint8_t errcode)
{
    return err_msg[errcode];
}

uint32_t FAADAPI faacDecGetCapabilities(void)
{
    uint32_t cap = 0;

    /* can't do without it */
    cap += LC_DEC_CAP;

#ifdef MAIN_DEC
    cap += MAIN_DEC_CAP;
#endif
#ifdef LTP_DEC
    cap += LTP_DEC_CAP;
#endif
#ifdef LD_DEC
    cap += LD_DEC_CAP;
#endif
#ifdef ERROR_RESILIENCE
    cap += ERROR_RESILIENCE_CAP;
#endif
#ifdef FIXED_POINT
    cap += FIXED_POINT_CAP;
#endif

    return cap;
}

faacDecHandle FAADAPI faacDecOpen(void)
{
    uint8_t i;
    faacDecHandle hDecoder = NULL;

    if ((hDecoder = (faacDecHandle)malloc(sizeof(faacDecStruct))) == NULL)
        return NULL;

    memset(hDecoder, 0, sizeof(faacDecStruct));

    hDecoder->config.outputFormat  = FAAD_FMT_16BIT;
    hDecoder->config.defObjectType = MAIN;
    hDecoder->config.defSampleRate = 44100; /* Default: 44.1kHz */
    hDecoder->adts_header_present = 0;
    hDecoder->adif_header_present = 0;
#ifdef ERROR_RESILIENCE
    hDecoder->aacSectionDataResilienceFlag = 0;
    hDecoder->aacScalefactorDataResilienceFlag = 0;
    hDecoder->aacSpectralDataResilienceFlag = 0;
#endif
    hDecoder->frameLength = 1024;

    hDecoder->frame = 0;
    hDecoder->sample_buffer = NULL;

    for (i = 0; i < MAX_CHANNELS; i++)
    {
        hDecoder->window_shape_prev[i] = 0;
        hDecoder->time_out[i] = NULL;
#ifdef SSR_DEC
        hDecoder->ssr_overlap[i] = NULL;
        hDecoder->prev_fmd[i] = NULL;
#endif
#ifdef MAIN_DEC
        hDecoder->pred_stat[i] = NULL;
#endif
#ifdef LTP_DEC
        hDecoder->ltp_lag[i] = 0;
        hDecoder->lt_pred_stat[i] = NULL;
#endif
    }

    hDecoder->drc = drc_init(REAL_CONST(1.0), REAL_CONST(1.0));

#if POW_TABLE_SIZE
    hDecoder->pow2_table = (real_t*)malloc(POW_TABLE_SIZE*sizeof(real_t));
    build_tables(hDecoder->pow2_table);
#endif

    return hDecoder;
}

faacDecConfigurationPtr FAADAPI faacDecGetCurrentConfiguration(faacDecHandle hDecoder)
{
    faacDecConfigurationPtr config = &(hDecoder->config);

    return config;
}

uint8_t FAADAPI faacDecSetConfiguration(faacDecHandle hDecoder,
                                    faacDecConfigurationPtr config)
{
    hDecoder->config.defObjectType = config->defObjectType;
    hDecoder->config.defSampleRate = config->defSampleRate;
    hDecoder->config.outputFormat  = config->outputFormat;

    /* OK */
    return 1;
}

int32_t FAADAPI faacDecInit(faacDecHandle hDecoder, uint8_t *buffer,
                            uint32_t buffer_size,
                            uint32_t *samplerate, uint8_t *channels)
{
    uint32_t bits = 0;
    bitfile ld;
    adif_header adif;
    adts_header adts;

    hDecoder->sf_index = get_sr_index(hDecoder->config.defSampleRate);
    hDecoder->object_type = hDecoder->config.defObjectType;
    *samplerate = sample_rates[hDecoder->sf_index];
    *channels = 1;

    if (buffer != NULL)
    {
        faad_initbits(&ld, buffer, buffer_size);

        /* Check if an ADIF header is present */
        if ((buffer[0] == 'A') && (buffer[1] == 'D') &&
            (buffer[2] == 'I') && (buffer[3] == 'F'))
        {
            hDecoder->adif_header_present = 1;

            get_adif_header(&adif, &ld);
            faad_byte_align(&ld);

            hDecoder->sf_index = adif.pce.sf_index;
            hDecoder->object_type = adif.pce.object_type;

            *samplerate = sample_rates[hDecoder->sf_index];
            *channels = adif.pce.channels;

            bits = bit2byte(faad_get_processed_bits(&ld));

        /* Check if an ADTS header is present */
        } else if (faad_showbits(&ld, 12) == 0xfff) {
            hDecoder->adts_header_present = 1;

            adts_frame(&adts, &ld);

            hDecoder->sf_index = adts.sf_index;
            hDecoder->object_type = adts.profile;

            *samplerate = sample_rates[hDecoder->sf_index];
            *channels = (adts.channel_configuration > 6) ?
                2 : adts.channel_configuration;
        }

        if (ld.error)
        {
            faad_endbits(&ld);
            return -1;
        }
        faad_endbits(&ld);
    }
    hDecoder->channelConfiguration = *channels;

    /* must be done before frameLength is divided by 2 for LD */
#ifdef SSR_DEC
    if (hDecoder->object_type == SSR)
        hDecoder->fb = ssr_filter_bank_init(hDecoder->frameLength/SSR_BANDS);
    else
#endif
        hDecoder->fb = filter_bank_init(hDecoder->frameLength);

#ifdef LD_DEC
    if (hDecoder->object_type == LD)
        hDecoder->frameLength >>= 1;
#endif

    if (can_decode_ot(hDecoder->object_type) < 0)
        return -1;

#ifndef FIXED_POINT
    if (hDecoder->config.outputFormat >= FAAD_FMT_DITHER_LOWEST)
        Init_Dither(16, hDecoder->config.outputFormat - FAAD_FMT_DITHER_LOWEST);
#endif

    return bits;
}

/* Init the library using a DecoderSpecificInfo */
int8_t FAADAPI faacDecInit2(faacDecHandle hDecoder, uint8_t *pBuffer,
                            uint32_t SizeOfDecoderSpecificInfo,
                            uint32_t *samplerate, uint8_t *channels)
{
    int8_t rc;
    mp4AudioSpecificConfig mp4ASC;

    hDecoder->adif_header_present = 0;
    hDecoder->adts_header_present = 0;

    if((hDecoder == NULL)
        || (pBuffer == NULL)
        || (SizeOfDecoderSpecificInfo < 2)
        || (samplerate == NULL)
        || (channels == NULL))
    {
        return -1;
    }

    /* decode the audio specific config */
    rc = AudioSpecificConfig(pBuffer, SizeOfDecoderSpecificInfo, &mp4ASC);

    /* copy the relevant info to the decoder handle */
    *samplerate = mp4ASC.samplingFrequency;
    *channels = mp4ASC.channelsConfiguration;
    hDecoder->sf_index = mp4ASC.samplingFrequencyIndex;
    hDecoder->object_type = mp4ASC.objectTypeIndex;
    hDecoder->aacSectionDataResilienceFlag = mp4ASC.aacSectionDataResilienceFlag;
    hDecoder->aacScalefactorDataResilienceFlag = mp4ASC.aacScalefactorDataResilienceFlag;
    hDecoder->aacSpectralDataResilienceFlag = mp4ASC.aacSpectralDataResilienceFlag;

    if (hDecoder->object_type < 5)
        hDecoder->object_type--; /* For AAC differs from MPEG-4 */
    if (rc != 0)
    {
        return rc;
    }
    hDecoder->channelConfiguration = *channels;
    if (mp4ASC.frameLengthFlag)
        hDecoder->frameLength = 960;

    /* must be done before frameLength is divided by 2 for LD */
#ifdef SSR_DEC
    if (hDecoder->object_type == SSR)
        hDecoder->fb = ssr_filter_bank_init(hDecoder->frameLength/SSR_BANDS);
    else
#endif
        hDecoder->fb = filter_bank_init(hDecoder->frameLength);

#ifdef LD_DEC
    if (hDecoder->object_type == LD)
        hDecoder->frameLength >>= 1;
#endif

#ifndef FIXED_POINT
    if (hDecoder->config.outputFormat >= FAAD_FMT_DITHER_LOWEST)
        Init_Dither(16, hDecoder->config.outputFormat - FAAD_FMT_DITHER_LOWEST);
#endif

    return 0;
}

int8_t FAADAPI faacDecInitDRM(faacDecHandle hDecoder, uint32_t samplerate,
                              uint8_t channels)
{
    /* Special object type defined for DRM */
    hDecoder->config.defObjectType = DRM_ER_LC;

    hDecoder->config.defSampleRate = samplerate;
    hDecoder->aacSectionDataResilienceFlag = 1; /* VCB11 */
    hDecoder->aacScalefactorDataResilienceFlag = 0; /* no RVLC */
    hDecoder->aacSpectralDataResilienceFlag = 1; /* HCR */
    hDecoder->frameLength = 960;
    hDecoder->sf_index = get_sr_index(hDecoder->config.defSampleRate);
    hDecoder->object_type = hDecoder->config.defObjectType;
    hDecoder->channelConfiguration = channels;

    /* must be done before frameLength is divided by 2 for LD */
    hDecoder->fb = filter_bank_init(hDecoder->frameLength);

#ifndef FIXED_POINT
    if (hDecoder->config.outputFormat >= FAAD_FMT_DITHER_LOWEST)
        Init_Dither(16, hDecoder->config.outputFormat - FAAD_FMT_DITHER_LOWEST);
#endif

    return 0;
}

void FAADAPI faacDecClose(faacDecHandle hDecoder)
{
    uint8_t i;

    if (hDecoder == NULL)
        return;

    for (i = 0; i < MAX_CHANNELS; i++)
    {
        if (hDecoder->time_out[i]) free(hDecoder->time_out[i]);
#ifdef SSR_DEC
        if (hDecoder->ssr_overlap[i]) free(hDecoder->ssr_overlap[i]);
        if (hDecoder->prev_fmd[i]) free(hDecoder->prev_fmd[i]);
#endif
#ifdef MAIN_DEC
        if (hDecoder->pred_stat[i]) free(hDecoder->pred_stat[i]);
#endif
#ifdef LTP_DEC
        if (hDecoder->lt_pred_stat[i]) free(hDecoder->lt_pred_stat[i]);
#endif
    }

#ifdef SSR_DEC
    if (hDecoder->object_type == SSR)
        ssr_filter_bank_end(hDecoder->fb);
    else
#endif
        filter_bank_end(hDecoder->fb);

    drc_end(hDecoder->drc);

#ifndef FIXED_POINT
#if POW_TABLE_SIZE
    if (hDecoder->pow2_table) free(hDecoder->pow2_table);
#endif
#endif

    if (hDecoder->sample_buffer) free(hDecoder->sample_buffer);

    if (hDecoder) free(hDecoder);
}

void* FAADAPI faacDecDecode(faacDecHandle hDecoder,
                            faacDecFrameInfo *hInfo,
                            uint8_t *buffer, uint32_t buffer_size)
{
    int32_t i;
    uint8_t ch;
    adts_header adts;
    uint8_t channels = 0, ch_ele = 0;
    bitfile *ld = (bitfile*)malloc(sizeof(bitfile));

    /* local copys of globals */
    uint8_t sf_index       =  hDecoder->sf_index;
    uint8_t object_type    =  hDecoder->object_type;
    uint8_t channelConfiguration = hDecoder->channelConfiguration;
#ifdef MAIN_DEC
    pred_state **pred_stat =  hDecoder->pred_stat;
#endif
#ifdef LTP_DEC
    real_t **lt_pred_stat  =  hDecoder->lt_pred_stat;
#endif
#ifndef FIXED_POINT
#if POW_TABLE_SIZE
    real_t *pow2_table     =  hDecoder->pow2_table;
#else
    real_t *pow2_table     =  NULL;
#endif
#endif
    uint8_t *window_shape_prev = hDecoder->window_shape_prev;
    real_t **time_out      =  hDecoder->time_out;
#ifdef SSR_DEC
    real_t **ssr_overlap   =  hDecoder->ssr_overlap;
    real_t **prev_fmd      =  hDecoder->prev_fmd;
#endif
    fb_info *fb            =  hDecoder->fb;
    drc_info *drc          =  hDecoder->drc;
    uint8_t outputFormat   =  hDecoder->config.outputFormat;
#ifdef LTP_DEC
    uint16_t *ltp_lag      =  hDecoder->ltp_lag;
#endif

    program_config pce;
    element *syntax_elements[MAX_SYNTAX_ELEMENTS];
    element **elements;
    int16_t *spec_data[MAX_CHANNELS];
    real_t *spec_coef[MAX_CHANNELS];

    uint16_t frame_len = hDecoder->frameLength;

    void *sample_buffer;


    memset(hInfo, 0, sizeof(faacDecFrameInfo));

    /* initialize the bitstream */
    faad_initbits(ld, buffer, buffer_size);

#ifdef DRM
    if (object_type == DRM_ER_LC)
    {
        faad_getbits(ld, 8
            DEBUGVAR(1,1,"faacDecDecode(): skip CRC"));
    }
#endif

    if (hDecoder->adts_header_present)
    {
        if ((hInfo->error = adts_frame(&adts, ld)) > 0)
            goto error;

        /* MPEG2 does byte_alignment() here,
         * but ADTS header is always multiple of 8 bits in MPEG2
         * so not needed to actually do it.
         */
    }

#ifdef ANALYSIS
    dbg_count = 0;
#endif

    elements = syntax_elements;

    /* decode the complete bitstream */
    elements = raw_data_block(hDecoder, hInfo, ld, syntax_elements,
        spec_data, spec_coef, &pce, drc);

    ch_ele = hDecoder->fr_ch_ele;
    channels = hDecoder->fr_channels;

    if (hInfo->error > 0)
        goto error;


    /* no more bit reading after this */
    hInfo->bytesconsumed = bit2byte(faad_get_processed_bits(ld));
    if (ld->error)
    {
        hInfo->error = 14;
        goto error;
    }
    faad_endbits(ld);
    if (ld) free(ld);
    ld = NULL;

    /* number of samples in this frame */
    hInfo->samples = frame_len*channels;
    /* number of channels in this frame */
    hInfo->channels = channels;
    /* samplerate */
    hInfo->samplerate = sample_rates[hDecoder->sf_index];

    /* check if frame has channel elements */
    if (channels == 0)
    {
        hDecoder->frame++;
        return NULL;
    }

    if (hDecoder->sample_buffer == NULL)
    {
        if (hDecoder->config.outputFormat == FAAD_FMT_DOUBLE)
            hDecoder->sample_buffer = malloc(frame_len*channels*sizeof(double));
        else
            hDecoder->sample_buffer = malloc(frame_len*channels*sizeof(real_t));
    }

    sample_buffer = hDecoder->sample_buffer;

    /* noiseless coding is done, the rest of the tools come now */
    for (ch = 0; ch < channels; ch++)
    {
        ic_stream *ics;

        /* find the syntax element to which this channel belongs */
        for (i = 0; i < ch_ele; i++)
        {
            if (syntax_elements[i]->channel == ch)
                ics = &(syntax_elements[i]->ics1);
            else if (syntax_elements[i]->paired_channel == ch)
                ics = &(syntax_elements[i]->ics2);
        }

        /* inverse quantization */
        inverse_quantization(spec_coef[ch], spec_data[ch], frame_len);

        /* apply scalefactors */
#ifdef FIXED_POINT
        apply_scalefactors(ics, spec_coef[ch], frame_len);
#else
        apply_scalefactors(ics, spec_coef[ch], pow2_table, frame_len);
#endif

        /* deinterleave short block grouping */
        if (ics->window_sequence == EIGHT_SHORT_SEQUENCE)
            quant_to_spec(ics, spec_coef[ch], frame_len);
    }

    /* Because for ms, is and pns both channels spectral coefficients are needed
       we have to restart running through all channels here.
    */
    for (ch = 0; ch < channels; ch++)
    {
        int16_t pch = -1;
        uint8_t right_channel;
        ic_stream *ics, *icsr;
        ltp_info *ltp;

        /* find the syntax element to which this channel belongs */
        for (i = 0; i < ch_ele; i++)
        {
            if (syntax_elements[i]->channel == ch)
            {
                ics = &(syntax_elements[i]->ics1);
                icsr = &(syntax_elements[i]->ics2);
                ltp = &(ics->ltp);
                pch = syntax_elements[i]->paired_channel;
                right_channel = 0;
            } else if (syntax_elements[i]->paired_channel == ch) {
                ics = &(syntax_elements[i]->ics2);
                if (syntax_elements[i]->common_window)
                    ltp = &(ics->ltp2);
                else
                    ltp = &(ics->ltp);
                right_channel = 1;
            }
        }

        /* pns decoding */
        if ((!right_channel) && (pch != -1) && (ics->ms_mask_present))
            pns_decode(ics, icsr, spec_coef[ch], spec_coef[pch], frame_len, 1);
        else if ((pch == -1) || ((pch != -1) && (!ics->ms_mask_present)))
            pns_decode(ics, NULL, spec_coef[ch], NULL, frame_len, 0);

        if (!right_channel && (pch != -1))
        {
            /* mid/side decoding */
            ms_decode(ics, icsr, spec_coef[ch], spec_coef[pch], frame_len);

            /* intensity stereo decoding */
            is_decode(ics, icsr, spec_coef[ch], spec_coef[pch], frame_len);
        }

#ifdef MAIN_DEC
        /* MAIN object type prediction */
        if (object_type == MAIN)
        {
            /* allocate the state only when needed */
            if (pred_stat[ch] == NULL)
            {
                pred_stat[ch] = (pred_state*)malloc(frame_len * sizeof(pred_state));
                reset_all_predictors(pred_stat[ch], frame_len);
            }

            /* intra channel prediction */
            ic_prediction(ics, spec_coef[ch], pred_stat[ch], frame_len);

            /* In addition, for scalefactor bands coded by perceptual
               noise substitution the predictors belonging to the
               corresponding spectral coefficients are reset.
            */
            pns_reset_pred_state(ics, pred_stat[ch]);
        }
#endif
#ifdef LTP_DEC
        if ((object_type == LTP)
#ifdef ERROR_RESILIENCE
            || (object_type == ER_LTP)
#endif
#ifdef LD_DEC
            || (object_type == LD)
#endif
            )
        {
#ifdef LD_DEC
            if (object_type == LD)
            {
                if (ltp->data_present)
                {
                    if (ltp->lag_update)
                        ltp_lag[ch] = ltp->lag;
                }
                ltp->lag = ltp_lag[ch];
            }
#endif

            /* allocate the state only when needed */
            if (lt_pred_stat[ch] == NULL)
            {
                lt_pred_stat[ch] = (real_t*)malloc(frame_len*4 * sizeof(real_t));
                memset(lt_pred_stat[ch], 0, frame_len*4 * sizeof(real_t));
            }

            /* long term prediction */
            lt_prediction(ics, ltp, spec_coef[ch], lt_pred_stat[ch], fb,
                ics->window_shape, window_shape_prev[ch],
                sf_index, object_type, frame_len);
        }
#endif

        /* tns decoding */
        tns_decode_frame(ics, &(ics->tns), sf_index, object_type,
            spec_coef[ch], frame_len);

        /* drc decoding */
        if (drc->present)
        {
            if (!drc->exclude_mask[ch] || !drc->excluded_chns_present)
                drc_decode(drc, spec_coef[ch]);
        }

        if (time_out[ch] == NULL)
        {
            time_out[ch] = (real_t*)malloc(frame_len*2*sizeof(real_t));
            memset(time_out[ch], 0, frame_len*2*sizeof(real_t));
        }

        /* filter bank */
#ifdef SSR_DEC
        if (object_type != SSR)
        {
#endif
            ifilter_bank(fb, ics->window_sequence, ics->window_shape,
                window_shape_prev[ch], spec_coef[ch],
                time_out[ch], object_type, frame_len);
#ifdef SSR_DEC
        } else {
            if (ssr_overlap[ch] == NULL)
            {
                ssr_overlap[ch] = (real_t*)malloc(2*frame_len*sizeof(real_t));
                memset(ssr_overlap[ch], 0, 2*frame_len*sizeof(real_t));
            }
            if (prev_fmd[ch] == NULL)
            {
                uint16_t k;
                prev_fmd[ch] = (real_t*)malloc(2*frame_len*sizeof(real_t));
                for (k = 0; k < 2*frame_len; k++)
                    prev_fmd[ch][k] = REAL_CONST(-1);
            }

            ssr_decode(&(ics->ssr), fb, ics->window_sequence, ics->window_shape,
                window_shape_prev[ch], spec_coef[ch], time_out[ch],
                ssr_overlap[ch], hDecoder->ipqf_buffer[ch], prev_fmd[ch], frame_len);
        }
#endif
        /* save window shape for next frame */
        window_shape_prev[ch] = ics->window_shape;

#ifdef LTP_DEC
        if ((object_type == LTP)
#ifdef ERROR_RESILIENCE
            || (object_type == ER_LTP)
#endif
#ifdef LD_DEC
            || (object_type == LD)
#endif
            )
        {
            lt_update_state(lt_pred_stat[ch], time_out[ch], time_out[ch]+frame_len,
                frame_len, object_type);
        }
#endif
    }

    sample_buffer = output_to_PCM(time_out, sample_buffer, channels,
        frame_len, outputFormat);

    hDecoder->frame++;
#ifdef LD_DEC
    if (object_type != LD)
    {
#endif
        if (hDecoder->frame <= 1)
            hInfo->samples = 0;
#ifdef LD_DEC
    } else {
        /* LD encoders will give lower delay */
        if (hDecoder->frame <= 0)
            hInfo->samples = 0;
    }
#endif

    /* cleanup */
    for (ch = 0; ch < channels; ch++)
    {
        if (spec_coef[ch]) free(spec_coef[ch]);
        if (spec_data[ch]) free(spec_data[ch]);
    }

    for (i = 0; i < ch_ele; i++)
    {
        if (syntax_elements[i]) free(syntax_elements[i]);
    }

#ifdef ANALYSIS
    fflush(stdout);
#endif

    return sample_buffer;

error:
    /* free all memory that could have been allocated */
    faad_endbits(ld);
    if (ld) free(ld);

    /* cleanup */
    for (ch = 0; ch < channels; ch++)
    {
        if (spec_coef[ch]) free(spec_coef[ch]);
        if (spec_data[ch]) free(spec_data[ch]);
    }

    for (i = 0; i < ch_ele; i++)
    {
        if (syntax_elements[i]) free(syntax_elements[i]);
    }

#ifdef ANALYSIS
    fflush(stdout);
#endif

    return NULL;
}
