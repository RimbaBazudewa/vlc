/*****************************************************************************
 * audio.c: transcoding stream output module (audio)
 *****************************************************************************
 * Copyright (C) 2003-2009 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Gildas Bazin <gbazin@videolan.org>
 *          Jean-Paul Saman <jpsaman #_at_# m2x dot nl>
 *          Antoine Cellerier <dionoea at videolan dot org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_input.h>
#include <vlc_meta.h>
#include <vlc_modules.h>
#include <vlc_sout.h>

#include "transcode.h"

static const int pi_channels_maps[9] =
{
    0,
    AOUT_CHAN_CENTER,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT,
    AOUT_CHAN_LFE  | AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_REARLEFT
     | AOUT_CHAN_REARRIGHT,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
     | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
     | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT | AOUT_CHAN_LFE,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
     | AOUT_CHAN_REARCENTER | AOUT_CHAN_MIDDLELEFT
     | AOUT_CHAN_MIDDLERIGHT | AOUT_CHAN_LFE,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER | AOUT_CHAN_REARLEFT
     | AOUT_CHAN_REARRIGHT | AOUT_CHAN_MIDDLELEFT | AOUT_CHAN_MIDDLERIGHT
     | AOUT_CHAN_LFE,
};

static int audio_update_format( decoder_t *p_dec )
{
    struct decoder_owner *p_owner = dec_get_owner( p_dec );
    sout_stream_id_sys_t *id = p_owner->id;

    p_dec->fmt_out.audio.i_format = p_dec->fmt_out.i_codec;
    aout_FormatPrepare( &p_dec->fmt_out.audio );

    vlc_mutex_lock(&id->fifo.lock);
    id->audio_dec_out = p_dec->fmt_out.audio;
    vlc_mutex_unlock(&id->fifo.lock);

    return ( p_dec->fmt_out.audio.i_bitspersample > 0 ) ? 0 : -1;
}

static int transcode_audio_filters_init( sout_stream_t *p_stream, sout_stream_id_sys_t *id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    /* Load user specified audio filters */
    /* XXX: These variable names come kinda out of nowhere... */
    var_Create( p_stream, "audio-time-stretch", VLC_VAR_BOOL );
    var_Create( p_stream, "audio-filter", VLC_VAR_STRING );
    if( p_sys->psz_af )
        var_SetString( p_stream, "audio-filter", p_sys->psz_af );
    id->p_af_chain = aout_FiltersNew( p_stream, &id->audio_dec_out,
                                      &id->p_encoder->fmt_in.audio, NULL, NULL );
    var_Destroy( p_stream, "audio-filter" );
    var_Destroy( p_stream, "audio-time-stretch" );
    return ( id->p_af_chain != NULL ) ? VLC_SUCCESS : VLC_EGENERIC;
}

static int transcode_audio_encoder_open( sout_stream_t *p_stream, sout_stream_id_sys_t *id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    id->p_encoder->p_module = module_need( id->p_encoder, "encoder", p_sys->psz_aenc, true );
    /* p_sys->i_acodec = 0 if there isn't acodec defined */
    if( !id->p_encoder->p_module )
    {
        msg_Err( p_stream, "cannot find audio encoder (module:%s fourcc:%4.4s). "
                           "Take a look few lines earlier to see possible reason.",
                           p_sys->psz_aenc ? p_sys->psz_aenc : "any",
                           (char *)&p_sys->i_acodec );
        return VLC_EGENERIC;
    }

    id->downstream_id = sout_StreamIdAdd( p_stream->p_next, &id->p_encoder->fmt_out );
    if ( !id->downstream_id )
    {
        msg_Err( p_stream, "cannot add this stream" );
        module_unneed( id->p_encoder, id->p_encoder->p_module );
        id->p_encoder->p_module = NULL;
        return VLC_EGENERIC;
    }

    id->p_encoder->fmt_out.i_codec =
            vlc_fourcc_GetCodec( AUDIO_ES, id->p_encoder->fmt_out.i_codec );

    return VLC_SUCCESS;
}

static int transcode_audio_encoder_configure( sout_stream_t *p_stream, sout_stream_id_sys_t *id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    audio_format_t *p_enc_in = &id->p_encoder->fmt_in.audio;
    audio_format_t *p_enc_out = &id->p_encoder->fmt_out.audio;

    /* Complete destination format */
    id->p_encoder->fmt_out.i_codec = p_sys->i_acodec;
    id->p_encoder->fmt_out.audio.i_format = p_sys->i_acodec;
    id->p_encoder->fmt_out.i_bitrate = p_sys->i_abitrate;
    p_enc_out->i_rate = p_sys->i_sample_rate ? p_sys->i_sample_rate
                                             : id->audio_dec_out.i_rate;
    p_enc_out->i_bitspersample = id->audio_dec_out.i_bitspersample;
    p_enc_out->i_channels = p_sys->i_channels ? p_sys->i_channels
                                              : id->audio_dec_out.i_channels;
    aout_FormatPrepare( p_enc_out );
    assert(p_enc_out->i_channels > 0);
    if( p_enc_out->i_channels >= ARRAY_SIZE(pi_channels_maps) )
        p_enc_out->i_channels = ARRAY_SIZE(pi_channels_maps) - 1;

    p_enc_in->i_physical_channels =
    p_enc_out->i_physical_channels = pi_channels_maps[p_enc_out->i_channels];

    /* Initialization of encoder format structures */
    es_format_Init( &id->p_encoder->fmt_in, AUDIO_ES, id->audio_dec_out.i_format );
    p_enc_in->i_format = id->audio_dec_out.i_format;
    p_enc_in->i_rate = p_enc_out->i_rate;
    p_enc_in->i_physical_channels = p_enc_out->i_physical_channels;
    aout_FormatPrepare( p_enc_in );

    id->p_encoder->p_cfg = p_sys->p_audio_cfg;

    /* Fix input format */
    p_enc_in->i_format = id->p_encoder->fmt_in.i_codec;
    if( !p_enc_in->i_physical_channels )
    {
        if( p_enc_in->i_channels < ARRAY_SIZE(pi_channels_maps) )
            p_enc_in->i_physical_channels = pi_channels_maps[p_enc_in->i_channels];
    }
    aout_FormatPrepare( p_enc_in );

    id->fmt_input_audio.i_rate = id->audio_dec_out.i_rate;
    id->fmt_input_audio.i_physical_channels = id->audio_dec_out.i_physical_channels;

    return VLC_SUCCESS;
}

static int transcode_audio_encoder_test( sout_stream_t *p_stream,
                                         const audio_format_t *p_dec_in,
                                         const audio_format_t *p_dec_out,
                                         vlc_fourcc_t i_codec_in,
                                         config_chain_t *p_cfg,
                                         const es_format_t *p_enc_fmtout,
                                         es_format_t *p_enc_wanted_in )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    encoder_t *p_encoder = sout_EncoderCreate( p_stream );
    if( !p_encoder )
        return VLC_EGENERIC;

    p_encoder->p_cfg = p_cfg;

    es_format_Init( &p_encoder->fmt_in, AUDIO_ES, i_codec_in );
    p_encoder->fmt_in.audio = *p_dec_out;
    es_format_Init( &p_encoder->fmt_out, AUDIO_ES, 0 );
    es_format_Copy( &p_encoder->fmt_out, p_enc_fmtout );

    audio_format_t *p_afmt_out = &p_encoder->fmt_out.audio;

    p_encoder->fmt_in.audio.i_format = i_codec_in;
    p_encoder->fmt_out.i_codec = p_afmt_out->i_format =  p_sys->i_acodec;
    p_encoder->fmt_out.i_bitrate = p_sys->i_abitrate;

    p_afmt_out->i_rate = FIRSTVALID( p_sys->i_sample_rate, p_dec_out->i_rate, p_dec_in->i_rate );
    p_afmt_out->i_channels = p_sys->i_channels ? p_sys->i_channels
                                               : p_dec_out->i_channels;
    if( p_afmt_out->i_channels == 0 )
    {
        p_afmt_out->i_channels = 2;
        p_afmt_out->i_physical_channels = AOUT_CHANS_STEREO;
    }
    else
    {
        if( p_afmt_out->i_physical_channels )
            p_afmt_out->i_physical_channels = p_afmt_out->i_physical_channels;
        else
            p_afmt_out->i_physical_channels = p_dec_out->i_physical_channels;
    }
    aout_FormatPrepare( p_afmt_out );

    module_t *p_module = module_need( p_encoder, "encoder", p_sys->psz_aenc, true );
    if( !p_module )
    {
        msg_Err( p_stream, "cannot find audio encoder (module:%s fourcc:%4.4s). "
                           "Take a look few lines earlier to see possible reason.",
                           p_sys->psz_aenc ? p_sys->psz_aenc : "any",
                           (char *)&p_sys->i_acodec );
    }
    else
    {
        /* Close the encoder.
         * We'll open it only when we have the first frame. */
        module_unneed( p_encoder, p_module );
    }

    /* copy our requested format */
    es_format_Copy( p_enc_wanted_in, &p_encoder->fmt_in );

    es_format_Clean( &p_encoder->fmt_in );
    es_format_Clean( &p_encoder->fmt_out );

    vlc_object_release( p_encoder );

    return p_module != NULL ? VLC_SUCCESS : VLC_EGENERIC;
}

static void decoder_queue_audio( decoder_t *p_dec, block_t *p_audio )
{
    struct decoder_owner *p_owner = dec_get_owner( p_dec );
    sout_stream_id_sys_t *id = p_owner->id;

    vlc_mutex_lock(&id->fifo.lock);
    *id->fifo.audio.last = p_audio;
    id->fifo.audio.last = &p_audio->p_next;
    vlc_mutex_unlock(&id->fifo.lock);
}

static block_t *transcode_dequeue_all_audios( sout_stream_id_sys_t *id )
{
    vlc_mutex_lock(&id->fifo.lock);
    block_t *p_audio_bufs = id->fifo.audio.first;
    id->fifo.audio.first = NULL;
    id->fifo.audio.last = &id->fifo.audio.first;
    vlc_mutex_unlock(&id->fifo.lock);

    return p_audio_bufs;
}

static int transcode_audio_new( sout_stream_t *p_stream,
                                sout_stream_id_sys_t *id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    /*
     * Open decoder
     */
    dec_get_owner( id->p_decoder )->id = id;

    static const struct decoder_owner_callbacks dec_cbs =
    {
        .audio = {
            audio_update_format,
            decoder_queue_audio,
        },
    };
    id->p_decoder->cbs = &dec_cbs;

    id->p_decoder->pf_decode = NULL;
    /* id->p_decoder->p_cfg = p_sys->p_audio_cfg; */
    id->p_decoder->p_module =
        module_need_var( id->p_decoder, "audio decoder", "codec" );
    if( !id->p_decoder->p_module )
    {
        msg_Err( p_stream, "cannot find audio decoder" );
        return VLC_EGENERIC;
    }

    vlc_mutex_lock(&id->fifo.lock);

    id->audio_dec_out = id->p_decoder->fmt_out.audio;
    /* The decoder fmt_out can be uninitialized here (since it can initialized
     * asynchronously). Fix audio_dec_out with default values in that case.
     * This should be enough to initialize the encoder for the first time (it
     * will be reloaded when all informations from the decoder are available).
     * */
    id->audio_dec_out.i_format = FIRSTVALID( id->audio_dec_out.i_format,
                                             id->p_decoder->fmt_out.i_codec,
                                             VLC_CODEC_FL32 );
    id->audio_dec_out.i_rate = FIRSTVALID( id->audio_dec_out.i_rate,
                                           id->p_decoder->fmt_in.audio.i_rate,
                                           48000 );
    id->audio_dec_out.i_physical_channels =
            FIRSTVALID( id->audio_dec_out.i_physical_channels,
                        id->p_decoder->fmt_in.audio.i_physical_channels,
                        AOUT_CHANS_STEREO );
    aout_FormatPrepare( &id->audio_dec_out );

    /* Initialization of encoder format structures */
    es_format_Init( &id->p_encoder->fmt_in, id->p_decoder->fmt_in.i_cat,
                    id->p_decoder->fmt_out.i_codec );
    /* Should be the same format until encoder loads */
    es_format_Init( &id->encoder_tested_fmt_in, id->p_decoder->fmt_in.i_cat,
                    id->p_decoder->fmt_out.i_codec );

    /* The decoder fmt_out can be uninitialized here (since it can initialized
     * asynchronously). Fix audio_dec_out with default values in that case.
     * This should be enough to initialize the encoder for the first time (it
     * will be reloaded when all informations from the decoder are available).
     * */
    if( transcode_audio_encoder_test( p_stream, &id->p_decoder->fmt_in.audio,
                                                &id->audio_dec_out,
                                                id->p_decoder->fmt_out.i_codec,
                                                p_sys->p_audio_cfg,
                                                &id->p_encoder->fmt_out,
                                                &id->encoder_tested_fmt_in ) != VLC_SUCCESS )
    {
        vlc_mutex_unlock(&id->fifo.lock);
        module_unneed( id->p_decoder, id->p_decoder->p_module );
        id->p_decoder->p_module = NULL;
        es_format_Clean( &id->encoder_tested_fmt_in );
        return VLC_EGENERIC;
    }

    vlc_mutex_unlock(&id->fifo.lock);

    return VLC_SUCCESS;
}

void transcode_audio_close( sout_stream_id_sys_t *id )
{
    /* Close decoder */
    if( id->p_decoder->p_module )
        module_unneed( id->p_decoder, id->p_decoder->p_module );
    id->p_decoder->p_module = NULL;

    if( id->p_decoder->p_description )
        vlc_meta_Delete( id->p_decoder->p_description );
    id->p_decoder->p_description = NULL;

    /* Close encoder */
    if( id->p_encoder->p_module )
        module_unneed( id->p_encoder, id->p_encoder->p_module );
    id->p_encoder->p_module = NULL;

    /* Close filters */
    if( id->p_af_chain != NULL )
        aout_FiltersDelete( (vlc_object_t *)NULL, id->p_af_chain );
}

int transcode_audio_process( sout_stream_t *p_stream,
                                    sout_stream_id_sys_t *id,
                                    block_t *in, block_t **out )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    *out = NULL;

    int ret = id->p_decoder->pf_decode( id->p_decoder, in );
    if( ret != VLCDEC_SUCCESS )
        return VLC_EGENERIC;

    block_t *p_audio_bufs = transcode_dequeue_all_audios( id );
    if( p_audio_bufs == NULL )
        goto end;

    do
    {
        block_t *p_audio_buf = p_audio_bufs;
        p_audio_bufs = p_audio_bufs->p_next;
        p_audio_buf->p_next = NULL;

        if( id->b_error )
        {
            block_Release( p_audio_buf );
            continue;
        }

        vlc_mutex_lock(&id->fifo.lock);

        if( p_audio_buf &&
            ( unlikely(id->p_encoder->p_module == NULL) ||
              id->audio_dec_out.i_rate != id->fmt_input_audio.i_rate ||
              id->audio_dec_out.i_physical_channels != id->fmt_input_audio.i_physical_channels ) )
        {
            if( id->p_encoder->p_module == NULL )
            {
                transcode_audio_encoder_configure( p_stream, id );
            }
            else
            {
                /* Check if audio format has changed, and filters need reinit */
                msg_Info( p_stream, "Audio changed, trying to reinitialize filters" );
                if( id->p_af_chain != NULL )
                {
                    aout_FiltersDelete( (vlc_object_t *)NULL, id->p_af_chain );
                    id->p_af_chain = NULL;
                }
            }

            if( transcode_audio_filters_init( p_stream, id ) )
            {
                vlc_mutex_unlock(&id->fifo.lock);
                goto error;
            }

            date_Init( &id->next_input_pts, id->audio_dec_out.i_rate, 1 );
            date_Set( &id->next_input_pts, p_audio_buf->i_pts );

            if( id->p_encoder->p_module == NULL &&
                transcode_audio_encoder_open( p_stream, id ) )
            {
                vlc_mutex_unlock(&id->fifo.lock);
                goto error;
            }
        }

        vlc_mutex_unlock(&id->fifo.lock);

        if( p_sys->b_master_sync )
        {
            vlc_tick_t i_pts = date_Get( &id->next_input_pts );
            vlc_tick_t i_drift = 0;

            if( likely( p_audio_buf->i_pts != VLC_TICK_INVALID ) )
                i_drift = p_audio_buf->i_pts - i_pts;

            if ( unlikely(i_drift > MASTER_SYNC_MAX_DRIFT
                 || i_drift < -MASTER_SYNC_MAX_DRIFT) )
            {
                msg_Dbg( p_stream,
                    "audio drift is too high (%"PRId64"), resetting master sync",
                    i_drift );
                date_Set( &id->next_input_pts, p_audio_buf->i_pts );
                if( likely(p_audio_buf->i_pts != VLC_TICK_INVALID ) )
                    i_drift = 0;
            }
            p_sys->i_master_drift = i_drift;
            date_Increment( &id->next_input_pts, p_audio_buf->i_nb_samples );
        }

        p_audio_buf->i_dts = p_audio_buf->i_pts;

        /* Run filter chain */
        p_audio_buf = aout_FiltersPlay( id->p_af_chain, p_audio_buf, 1.f );
        if( !p_audio_buf )
            goto error;

        p_audio_buf->i_dts = p_audio_buf->i_pts;

        block_t *p_block = id->p_encoder->pf_encode_audio( id->p_encoder, p_audio_buf );

        block_ChainAppend( out, p_block );
        block_Release( p_audio_buf );
        continue;
error:
        if( p_audio_buf )
            block_Release( p_audio_buf );
        id->b_error = true;
    } while( p_audio_bufs );

end:
    /* Drain encoder */
    if( unlikely( !id->b_error && in == NULL ) && id->p_encoder->p_module )
    {
        block_t *p_block;
        do {
            p_block = id->p_encoder->pf_encode_audio(id->p_encoder, NULL );
            block_ChainAppend( out, p_block );
        } while( p_block );
    }

    return id->b_error ? VLC_EGENERIC : VLC_SUCCESS;
}

bool transcode_audio_add( sout_stream_t *p_stream, const es_format_t *p_fmt,
            sout_stream_id_sys_t *id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    msg_Dbg( p_stream,
             "creating audio transcoding from fcc=`%4.4s' to fcc=`%4.4s'",
             (char*)&p_fmt->i_codec, (char*)&p_sys->i_acodec );

    id->fifo.audio.first = NULL;
    id->fifo.audio.last = &id->fifo.audio.first;

    /* Build decoder -> filter -> encoder chain */
    if( transcode_audio_new( p_stream, id ) == VLC_EGENERIC )
    {
        msg_Err( p_stream, "cannot create audio chain" );
        return false;
    }

    /* Open output stream */
    id->b_transcode = true;

    return true;
}
