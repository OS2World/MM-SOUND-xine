/* 
 * Copyright (C) 2000-2002 the xine project
 * 
 * This file is part of xine, a free video player.
 * 
 * xine is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * xine is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 * $Id: xine_decoder.c,v 1.41 2003/01/31 18:29:47 miguelfreitas Exp $
 *
 * stuff needed to turn libmad into a xine decoder plugin
 */

#include <stdlib.h>
#include <string.h>

#include "xine_internal.h"
#include "audio_out.h"
#include "buffer.h"
#include "frame.h"
#include "synth.h"
#include "xineutils.h"

/*
#define LOG
*/

#define INPUT_BUF_SIZE  16384

typedef struct {
  audio_decoder_class_t   decoder_class;
} mad_class_t;

typedef struct mad_decoder_s {
  audio_decoder_t   audio_decoder;

  xine_stream_t    *xstream;

  int64_t           pts;

  struct mad_synth  synth; 
  struct mad_stream stream;
  struct mad_frame  frame;

  int               output_sampling_rate;
  int               output_open;
  int               output_mode;

  uint8_t           buffer[INPUT_BUF_SIZE];
  int               bytes_in_buffer;
  int               preview_mode;

} mad_decoder_t;

static void mad_reset (audio_decoder_t *this_gen) {

  mad_decoder_t *this = (mad_decoder_t *) this_gen;

  mad_synth_finish (&this->synth);
  mad_frame_finish (&this->frame);
  mad_stream_finish(&this->stream);

  this->pts = 0;
  this->bytes_in_buffer = 0;
  this->preview_mode = 0;

  mad_synth_init  (&this->synth);
  mad_stream_init (&this->stream);
  mad_frame_init  (&this->frame);
}


static void mad_discontinuity (audio_decoder_t *this_gen) {

  mad_decoder_t *this = (mad_decoder_t *) this_gen;
  
  this->pts = 0;
}

/* utility to scale and round samples to 16 bits */

static inline
signed int scale(mad_fixed_t sample)
{
  /* round */
  sample += (1L << (MAD_F_FRACBITS - 16));

  /* clip */
  if (sample >= MAD_F_ONE)
    sample = MAD_F_ONE - 1;
  else if (sample < -MAD_F_ONE)
    sample = -MAD_F_ONE;

  /* quantize */
  return sample >> (MAD_F_FRACBITS + 1 - 16);
}

/*
static int head_check(mad_decoder_t *this) {

  if( (this->header & 0xffe00000) != 0xffe00000)
    return 0;
  if(!((this->header>>17)&3))
    return 0;
  if( ((this->header>>12)&0xf) == 0xf)
    return 0;
  if( ((this->header>>10)&0x3) == 0x3 )
    return 0;
  return 1;
}
*/

static void mad_decode_data (audio_decoder_t *this_gen, buf_element_t *buf) {

  mad_decoder_t *this = (mad_decoder_t *) this_gen;

#ifdef LOG
  printf ("libmad: decode data, decoder_flags: %d\n", buf->decoder_flags);
#endif  
  
  if (buf->size>(INPUT_BUF_SIZE-this->bytes_in_buffer)) {
    printf ("libmad: ALERT input buffer too small (%d bytes, %d avail)!\n",
	    buf->size, INPUT_BUF_SIZE-this->bytes_in_buffer);
    buf->size = INPUT_BUF_SIZE-this->bytes_in_buffer;
  }
  
  if ((buf->decoder_flags & BUF_FLAG_HEADER) == 0) {

    /* reset decoder on leaving preview mode */
    if ((buf->decoder_flags & BUF_FLAG_PREVIEW) == 0) {
      if (this->preview_mode) {
	mad_reset (this_gen);
      }
    } else {
      this->preview_mode = 1;
    }

    xine_fast_memcpy (&this->buffer[this->bytes_in_buffer], 
                        buf->content, buf->size);
    this->bytes_in_buffer += buf->size;
    
    /*
    printf ("libmad: decode data - doing it\n");
    */

    mad_stream_buffer (&this->stream, this->buffer, 
		       this->bytes_in_buffer);

    while (1) {

      if (mad_frame_decode (&this->frame, &this->stream) != 0) {

	if (this->stream.next_frame) {
	  int num_bytes =
	    this->buffer + this->bytes_in_buffer - this->stream.next_frame;
	  
	  /* printf("libmad: MAD_ERROR_BUFLEN\n"); */

	  memmove(this->buffer, this->stream.next_frame, num_bytes);
	  this->bytes_in_buffer = num_bytes;
	}

	switch (this->stream.error) {

	case MAD_ERROR_BUFLEN:
	  return;

	default: 
	  mad_stream_buffer (&this->stream, this->buffer, 
			     this->bytes_in_buffer);
	}

      } else {
	int mode = (this->frame.header.mode == MAD_MODE_SINGLE_CHANNEL) ? AO_CAP_MODE_MONO : AO_CAP_MODE_STEREO;

	if (!this->output_open 
	    || (this->output_sampling_rate != this->frame.header.samplerate)
	    || (this->output_mode != mode)) {

#ifdef LOG
	  printf ("libmad: audio sample rate %d mode %08x\n",
		  this->frame.header.samplerate,
		  mode);
#endif

	  this->xstream->stream_info[XINE_STREAM_INFO_AUDIO_BITRATE] = this->frame.header.bitrate;
	  switch (this->frame.header.layer) {
	  case MAD_LAYER_I:
	    this->xstream->meta_info[XINE_META_INFO_AUDIOCODEC] 
	      = strdup ("MPEG audio layer 1");
	    break;
	  case MAD_LAYER_II:
	    this->xstream->meta_info[XINE_META_INFO_AUDIOCODEC] 
	      = strdup ("MPEG audio layer 2");
	    break;
	  case MAD_LAYER_III:
	    this->xstream->meta_info[XINE_META_INFO_AUDIOCODEC] 
	      = strdup ("MPEG audio layer 3");
	    break;
	  default:
	    this->xstream->meta_info[XINE_META_INFO_AUDIOCODEC] 
	      = strdup ("MPEG audio");
	  }
	  
	  if (this->output_open) {
	    this->xstream->audio_out->close (this->xstream->audio_out, this->xstream);
	    this->output_open = 0;
          }
          if (!this->output_open) {
	    this->output_open = this->xstream->audio_out->open(this->xstream->audio_out,
				   this->xstream, 16,
				   this->frame.header.samplerate, 
			           mode) ;
          }
          if (!this->output_open) {
            return;
          }
	  this->output_sampling_rate = this->frame.header.samplerate;
	  this->output_mode = mode;
	}

	mad_synth_frame (&this->synth, &this->frame);

	if ( (buf->decoder_flags & BUF_FLAG_PREVIEW) == 0 ) {
        
	  unsigned int         nchannels, nsamples;
	  mad_fixed_t const   *left_ch, *right_ch;
	  struct mad_pcm      *pcm = &this->synth.pcm;
	  audio_buffer_t      *audio_buffer;
	  uint16_t            *output;

	  audio_buffer = this->xstream->audio_out->get_buffer (this->xstream->audio_out);
	  output = audio_buffer->mem;

	  nchannels = pcm->channels;
	  nsamples  = pcm->length;
	  left_ch   = pcm->samples[0];
	  right_ch  = pcm->samples[1];
	  
	  while (nsamples--) {
	    /* output sample(s) in 16-bit signed little-endian PCM */
	    
	    *output++ = scale(*left_ch++);
	    
	    if (nchannels == 2) 
	      *output++ = scale(*right_ch++);

	  }

	  audio_buffer->num_frames = pcm->length;
	  audio_buffer->vpts       = buf->pts;

	  this->xstream->audio_out->put_buffer (this->xstream->audio_out, audio_buffer, this->xstream);

	  buf->pts = 0;

	}
#ifdef LOG
	printf ("libmad: decode worked\n"); 
#endif
      }
    } 

  }
}

static void mad_dispose (audio_decoder_t *this_gen) {

  mad_decoder_t *this = (mad_decoder_t *) this_gen; 

  mad_synth_finish (&this->synth);
  mad_frame_finish (&this->frame);
  mad_stream_finish(&this->stream);

  if (this->output_open) { 
    this->xstream->audio_out->close (this->xstream->audio_out, this->xstream);
    this->output_open = 0;
  }

  free (this_gen);
}

static audio_decoder_t *open_plugin (audio_decoder_class_t *class_gen, xine_stream_t *stream) {

  mad_decoder_t *this ;

  this = (mad_decoder_t *) malloc (sizeof (mad_decoder_t));

  this->audio_decoder.decode_data         = mad_decode_data;
  this->audio_decoder.reset               = mad_reset;
  this->audio_decoder.discontinuity       = mad_discontinuity;
  this->audio_decoder.dispose             = mad_dispose;

  this->output_open     = 0;
  this->bytes_in_buffer = 0;
  this->preview_mode    = 0;

  this->xstream         = stream;

  mad_synth_init  (&this->synth);
  mad_stream_init (&this->stream);
  mad_frame_init  (&this->frame);

#ifdef LOG
  printf ("libmad: init\n"); 
#endif

  return &this->audio_decoder;
}

/*
 * mad plugin class
 */

static char *get_identifier (audio_decoder_class_t *this) {
  return "mad";
}

static char *get_description (audio_decoder_class_t *this) {
  return "libmad based mpeg audio layer 1/2/3 decoder plugin";
}

static void dispose_class (audio_decoder_class_t *this) {
  free (this);
}

static void *init_plugin (xine_t *xine, void *data) {

  mad_class_t *this;
  
  this = (mad_class_t *) malloc (sizeof (mad_class_t));

  this->decoder_class.open_plugin     = open_plugin;
  this->decoder_class.get_identifier  = get_identifier;
  this->decoder_class.get_description = get_description;
  this->decoder_class.dispose         = dispose_class;

  return this;
}

static uint32_t audio_types[] = { 
  BUF_AUDIO_MPEG, 0
};

static decoder_info_t dec_info_audio = {
  audio_types,         /* supported types */
  6                    /* priority        */
};

plugin_info_t xine_plugin_info[] = {
  /* type, API, "name", version, special_info, init_function */  
  { PLUGIN_AUDIO_DECODER, 13, "mad", XINE_VERSION_CODE, &dec_info_audio, init_plugin },
  { PLUGIN_NONE, 0, "", 0, NULL, NULL }
};
