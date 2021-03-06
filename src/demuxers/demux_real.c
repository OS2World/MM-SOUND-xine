/*
 * Copyright (C) 2000-2003 the xine project
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
 */

/*
 * Real Media File Demuxer by Mike Melanson (melanson@pcisys.net)
 * For more information regarding the Real file format, visit:
 *   http://www.pcisys.net/~melanson/codecs/
 *
 * video packet sub-demuxer ported from mplayer code (www.mplayerhq.hu):
 *   Real parser & demuxer
 *   
 *   (C) Alex Beregszaszi <alex@naxine.org>
 *   
 *   Based on FFmpeg's libav/rm.c.
 *
 * $Id: demux_real.c,v 1.65 2003/07/25 21:02:05 miguelfreitas Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/********** logging **********/
#define LOG_MODULE "demux_real"
/* #define LOG_VERBOSE */
/* #define LOG */

#include "xine_internal.h"
#include "xineutils.h"
#include "compat.h"
#include "demux.h"
#include "bswap.h"

#define FOURCC_TAG( ch0, ch1, ch2, ch3 ) \
        ( (long)(unsigned char)(ch3) | \
        ( (long)(unsigned char)(ch2) << 8 ) | \
        ( (long)(unsigned char)(ch1) << 16 ) | \
        ( (long)(unsigned char)(ch0) << 24 ) )

#define RMF_TAG   FOURCC_TAG('.', 'R', 'M', 'F')
#define PROP_TAG  FOURCC_TAG('P', 'R', 'O', 'P')
#define MDPR_TAG  FOURCC_TAG('M', 'D', 'P', 'R')
#define CONT_TAG  FOURCC_TAG('C', 'O', 'N', 'T')
#define DATA_TAG  FOURCC_TAG('D', 'A', 'T', 'A')
#define INDX_TAG  FOURCC_TAG('I', 'N', 'D', 'X')
#define RA_TAG    FOURCC_TAG('.', 'r', 'a', 0xfd)
#define VIDO_TAG  FOURCC_TAG('V', 'I', 'D', 'O')

#define PREAMBLE_SIZE 8
#define REAL_SIGNATURE_SIZE 4
#define DATA_CHUNK_HEADER_SIZE 10
#define DATA_PACKET_HEADER_SIZE 12
#define INDEX_CHUNK_HEADER_SIZE 20
#define INDEX_RECORD_SIZE 14

#define PN_KEYFRAME_FLAG 0x0002

#define MAX_VIDEO_STREAMS 10
#define MAX_AUDIO_STREAMS 8

typedef struct {
  uint16_t   object_version;

  uint16_t   stream_number;
  uint32_t   max_bit_rate;
  uint32_t   avg_bit_rate;
  uint32_t   max_packet_size;
  uint32_t   avg_packet_size;
  uint32_t   start_time;
  uint32_t   preroll;
  uint32_t   duration;
  char       stream_name_size;
  char      *stream_name;
  char       mime_type_size;
  char      *mime_type;
  uint32_t   type_specific_len;
  char      *type_specific_data;
} mdpr_t;

typedef struct {
  unsigned int   timestamp;
  unsigned int   offset;
  unsigned int   packetno;
} real_index_entry_t;

typedef struct {
  uint32_t             fourcc;
  uint32_t             buf_type;
  
  real_index_entry_t  *index;
  int                  index_entries;
  
  mdpr_t              *mdpr;
} real_stream_t;

typedef struct {
  demux_plugin_t       demux_plugin;

  xine_stream_t       *stream;
  fifo_buffer_t       *video_fifo;
  fifo_buffer_t       *audio_fifo;
  input_plugin_t      *input;
  int                  status;

  off_t                index_start;
  off_t                data_start;
  off_t                data_size;
  unsigned int         duration;

  real_stream_t        video_streams[MAX_VIDEO_STREAMS];
  int                  num_video_streams;
  real_stream_t       *video_stream;

  real_stream_t        audio_streams[MAX_AUDIO_STREAMS];
  int                  num_audio_streams;
  real_stream_t       *audio_stream;
  int                  audio_need_keyframe;

  unsigned int         current_data_chunk_packet_count;
  unsigned int         next_data_chunk_offset;
  unsigned int         data_chunk_size;

  int                  old_seqnum;
  int                  packet_size_cur;

  off_t                avg_bitrate;

  int64_t              last_pts[2];
  int                  send_newpts;
  int                  buf_flag_seek;

  int                  fragment_size; /* video sub-demux */

  int                  reference_mode;
} demux_real_t ;

typedef struct {
  demux_class_t     demux_class;
} demux_real_class_t;


static void real_parse_index(demux_real_t *this) {

  off_t                next_index_chunk = this->index_start;
  off_t                original_pos     = this->input->get_current_pos(this->input);
  unsigned char        index_chunk_header[INDEX_CHUNK_HEADER_SIZE];
  unsigned char        index_record[INDEX_RECORD_SIZE];
  int                  i, entries, stream_num;
  real_index_entry_t **index;
  
  while(next_index_chunk) {
    lprintf("reading index chunk at %llX\n", next_index_chunk);

    /* Seek to index chunk */
    this->input->seek(this->input, next_index_chunk, SEEK_SET);

    /* Read index chunk header */
    if(this->input->read(this->input, index_chunk_header, INDEX_CHUNK_HEADER_SIZE)
       != INDEX_CHUNK_HEADER_SIZE) {
      lprintf("index chunk header not read\n");
      break;
    }

    /* Check chunk is actually an index chunk */
    if(BE_32(&index_chunk_header[0]) == INDX_TAG) {

      /* Check version */
      if(BE_16(&index_chunk_header[8]) != 0) {
        lprintf("unknown index chunk version %d\n",
                BE_16(&index_chunk_header[8]));
        break;
      }

      /* Read data from header */
      entries          = BE_32(&index_chunk_header[10]);
      stream_num       = BE_16(&index_chunk_header[14]);
      next_index_chunk = BE_32(&index_chunk_header[16]);

      /* Find which stream this index is for */
      index = NULL;
      for(i = 0; i < this->num_video_streams; i++) {
        if(stream_num == this->video_streams[i].mdpr->stream_number) {
          index = &this->video_streams[i].index;
          this->video_streams[i].index_entries = entries;
          lprintf("found index chunk for video stream with num %d\n", stream_num);
          break;
        }
      }

      if(!index) {
        for(i = 0; i < this->num_audio_streams; i++) {
          if(stream_num == this->audio_streams[i].mdpr->stream_number) {
            index = &this->audio_streams[i].index;
            this->audio_streams[i].index_entries = entries;
            lprintf("found index chunk for audio stream with num %d\n", stream_num);
            break;
          }
        }
      }

      if(index && entries) {
        /* Allocate memory for index */
        *index = xine_xmalloc(entries * sizeof(real_index_entry_t));
        
        /* Read index */
        for(i = 0; i < entries; i++) {
          if(this->input->read(this->input, index_record, INDEX_RECORD_SIZE)
             != INDEX_RECORD_SIZE) {
            lprintf("index record not read\n");
            free(*index);
            *index = NULL;
            break;
          }

          (*index)[i].timestamp = BE_32(&index_record[2]);
          (*index)[i].offset    = BE_32(&index_record[6]);
          (*index)[i].packetno  = BE_32(&index_record[10]);
        }
      } else {
        lprintf("unused index chunk with %d entries for stream num %d\n",
                entries, stream_num);
      }
    } else {
      lprintf("expected index chunk found chunk type: %.4s\n", &index_chunk_header[0]);
      break;
    }
  }

  /* Seek back to position before index reading */
  this->input->seek(this->input, original_pos, SEEK_SET);
}

static mdpr_t *real_parse_mdpr(const char *data) {
  mdpr_t *mdpr=malloc(sizeof(mdpr_t));

  mdpr->object_version=BE_16(&data[0]);

  if (mdpr->object_version != 0) {
    lprintf("warning: unknown object version in MDPR: 0x%04x\n",
            mdpr->object_version);
  }

  mdpr->stream_number=BE_16(&data[2]);
  mdpr->max_bit_rate=BE_32(&data[4]);
  mdpr->avg_bit_rate=BE_32(&data[8]);
  mdpr->max_packet_size=BE_32(&data[12]);
  mdpr->avg_packet_size=BE_32(&data[16]);
  mdpr->start_time=BE_32(&data[20]);
  mdpr->preroll=BE_32(&data[24]);
  mdpr->duration=BE_32(&data[28]);

  mdpr->stream_name_size=data[32];
  mdpr->stream_name=malloc(sizeof(char)*(mdpr->stream_name_size+1));
  memcpy(mdpr->stream_name, &data[33], mdpr->stream_name_size);
  mdpr->stream_name[(int)mdpr->stream_name_size]=0;

  mdpr->mime_type_size=data[33+mdpr->stream_name_size];
  mdpr->mime_type=malloc(sizeof(char)*(mdpr->mime_type_size+1));
  memcpy(mdpr->mime_type, &data[34+mdpr->stream_name_size], mdpr->mime_type_size);
  mdpr->mime_type[(int)mdpr->mime_type_size]=0;

  mdpr->type_specific_len=BE_32(&data[34+mdpr->stream_name_size+mdpr->mime_type_size]);
  mdpr->type_specific_data=malloc(sizeof(char)*(mdpr->type_specific_len));
  memcpy(mdpr->type_specific_data,
      &data[38+mdpr->stream_name_size+mdpr->mime_type_size], mdpr->type_specific_len);

  lprintf("MDPR: stream number: %i\n", mdpr->stream_number);
  lprintf("MDPR: maximal bit rate: %i\n", mdpr->max_bit_rate);
  lprintf("MDPR: average bit rate: %i\n", mdpr->avg_bit_rate);
  lprintf("MDPR: largest packet size: %i bytes\n", mdpr->max_packet_size);
  lprintf("MDPR: average packet size: %i bytes\n", mdpr->avg_packet_size);
  lprintf("MDPR: start time: %i\n", mdpr->start_time);
  lprintf("MDPR: pre-buffer: %i ms\n", mdpr->preroll);
  lprintf("MDPR: duration of stream: %i ms\n", mdpr->duration);
  lprintf("MDPR: stream name: %s\n", mdpr->stream_name);
  lprintf("MDPR: mime type: %s\n", mdpr->mime_type);
  lprintf("MDPR: type specific data:\n");
#ifdef LOG
  xine_hexdump(mdpr->type_specific_data, mdpr->type_specific_len);
#endif

  return mdpr;
}

static void real_free_mdpr (mdpr_t *mdpr) {
  free (mdpr->stream_name);
  free (mdpr->mime_type);
  free (mdpr->type_specific_data);
  free (mdpr);
}


static void real_parse_headers (demux_real_t *this) {

  char           preamble[PREAMBLE_SIZE];
  unsigned int   chunk_type = 0;
  unsigned int   chunk_size;
  unsigned char *chunk_buffer;
  int            field_size;
  int            stream_ptr;
  unsigned char  data_chunk_header[DATA_CHUNK_HEADER_SIZE];
  unsigned char  signature[REAL_SIGNATURE_SIZE];

  this->data_start = 0;
  this->data_size = 0;
  this->num_video_streams = 0;
  this->num_audio_streams = 0;

  if (INPUT_IS_SEEKABLE(this->input))
    this->input->seek (this->input, 0, SEEK_SET);

  if (this->input->read(this->input, signature, REAL_SIGNATURE_SIZE) !=
      REAL_SIGNATURE_SIZE) {

    lprintf ("signature not read\n");
    this->status = DEMUX_FINISHED;
    return;
  }

  if (BE_32(signature) != RMF_TAG) {
    this->status = DEMUX_FINISHED;
    lprintf ("signature not found '%.4s'\n", signature);
    return;
  }

  /* skip to the start of the first chunk (the first chunk is 0x12 bytes
   * long) and start traversing */
  this->input->seek(this->input, 14, SEEK_CUR);

  /* iterate through chunks and gather information until the first DATA
   * chunk is located */
  while (chunk_type != DATA_TAG) {

    if (this->input->read(this->input, preamble, PREAMBLE_SIZE) !=
	PREAMBLE_SIZE) {
      this->status = DEMUX_FINISHED;
      return;
    }
    chunk_type = BE_32(&preamble[0]);
    chunk_size = BE_32(&preamble[4]);

    lprintf ("chunktype %.4s len %d\n", (char *) &chunk_type, chunk_size);
    switch (chunk_type) {

    case PROP_TAG:
    case MDPR_TAG:
    case CONT_TAG:

      chunk_size -= PREAMBLE_SIZE;
      chunk_buffer = xine_xmalloc(chunk_size);
      if (this->input->read(this->input, chunk_buffer, chunk_size) !=
	  chunk_size) {
	free (chunk_buffer);
	this->status = DEMUX_FINISHED;
	return;
      }

      if (chunk_type == PROP_TAG) {

        this->duration      = BE_32(&chunk_buffer[22]);
        this->index_start   = BE_32(&chunk_buffer[30]);
        this->data_start    = BE_32(&chunk_buffer[34]);
	this->avg_bitrate   = BE_32(&chunk_buffer[6]);

        lprintf("PROP: duration: %d ms\n", this->duration);
        lprintf("PROP: index start: %llX\n", this->index_start);
        lprintf("PROP: data start: %llX\n", this->data_start);
        lprintf("PROP: average bit rate: %lld\n", this->avg_bitrate);

	if (this->avg_bitrate<1)
	  this->avg_bitrate = 1;

	this->stream->stream_info[XINE_STREAM_INFO_BITRATE] = this->avg_bitrate;

      } else if (chunk_type == MDPR_TAG) {

        mdpr_t   *mdpr;
        uint32_t  fourcc;

        mdpr = real_parse_mdpr (chunk_buffer);

        lprintf ("parsing type specific data...\n");

        if(BE_32(mdpr->type_specific_data) == RA_TAG) {
          int version;

          if(this->num_audio_streams == MAX_AUDIO_STREAMS) {
            xprintf(this->stream->xine, XINE_VERBOSITY_DEBUG,
                    "maximum number of audio stream exceeded\n");
            goto unknown;
          }
          
          version = BE_16(mdpr->type_specific_data + 4);

          lprintf("audio version %d detected\n", version);

          if(version == 4) {
            int str_len;

            str_len = *(mdpr->type_specific_data + 56);
            fourcc = ME_32(mdpr->type_specific_data + 58 + str_len);
          } else if(version == 5) {
            fourcc = ME_32(mdpr->type_specific_data + 66);
          } else {
            lprintf("unsupported audio header version %d\n", version);

            goto unknown;
          }

          lprintf("fourcc = %.4s\n", (char *) &fourcc);

          this->audio_streams[this->num_audio_streams].fourcc = fourcc;
          this->audio_streams[this->num_audio_streams].buf_type = formattag_to_buf_audio(fourcc);
          this->audio_streams[this->num_audio_streams].index = NULL;
          this->audio_streams[this->num_audio_streams].mdpr = mdpr;

          this->num_audio_streams++;

        } else if(BE_32(mdpr->type_specific_data + 4) == VIDO_TAG) {

          if(this->num_video_streams == MAX_VIDEO_STREAMS) {
            xprintf(this->stream->xine, XINE_VERBOSITY_DEBUG,
                    "maximum number of video stream exceeded\n");
            goto unknown;
          }
          
          lprintf ("video detected\n");
          fourcc = *(uint32_t *) (mdpr->type_specific_data + 8);
          lprintf("fourcc = %.4s\n", (char *) &fourcc);

          this->video_streams[this->num_video_streams].fourcc = fourcc;
          this->video_streams[this->num_video_streams].buf_type = fourcc_to_buf_video(fourcc);
          this->video_streams[this->num_video_streams].index = NULL;
          this->video_streams[this->num_video_streams].mdpr = mdpr;

          this->num_video_streams++;

        } else {
          lprintf("unrecognised type specific data\n");

unknown:
          real_free_mdpr(mdpr);
        }

      } else if (chunk_type == CONT_TAG) {

        stream_ptr = 2;

        /* load the title string */
        field_size = BE_16(&chunk_buffer[stream_ptr]);
        stream_ptr += 2;
        this->stream->meta_info[XINE_META_INFO_TITLE] =
          realloc (this->stream->meta_info[XINE_META_INFO_TITLE],
                   field_size + 1);
        strncpy(this->stream->meta_info[XINE_META_INFO_TITLE],
          &chunk_buffer[stream_ptr], field_size);
        this->stream->meta_info[XINE_META_INFO_TITLE][field_size] = '\0';
        stream_ptr += field_size;

        /* load the author string */
        field_size = BE_16(&chunk_buffer[stream_ptr]);
        stream_ptr += 2;
        this->stream->meta_info[XINE_META_INFO_ARTIST] =
          realloc (this->stream->meta_info[XINE_META_INFO_ARTIST],
                   field_size + 1);
        strncpy(this->stream->meta_info[XINE_META_INFO_ARTIST],
          &chunk_buffer[stream_ptr], field_size);
        this->stream->meta_info[XINE_META_INFO_ARTIST][field_size] = '\0';
        stream_ptr += field_size;

        /* load the copyright string as the year */
        field_size = BE_16(&chunk_buffer[stream_ptr]);
        stream_ptr += 2;
        this->stream->meta_info[XINE_META_INFO_YEAR] =
          realloc (this->stream->meta_info[XINE_META_INFO_YEAR],
                   field_size + 1);
        strncpy(this->stream->meta_info[XINE_META_INFO_YEAR],
          &chunk_buffer[stream_ptr], field_size);
        this->stream->meta_info[XINE_META_INFO_YEAR][field_size] = '\0';
        stream_ptr += field_size;

        /* load the comment string */
        field_size = BE_16(&chunk_buffer[stream_ptr]);
        stream_ptr += 2;
        this->stream->meta_info[XINE_META_INFO_COMMENT] =
          realloc (this->stream->meta_info[XINE_META_INFO_COMMENT],
                   field_size + 1);
        strncpy(this->stream->meta_info[XINE_META_INFO_COMMENT],
          &chunk_buffer[stream_ptr], field_size);
        this->stream->meta_info[XINE_META_INFO_COMMENT][field_size] = '\0';
        stream_ptr += field_size;
      }

      free(chunk_buffer);
      break;

    case DATA_TAG:
      if (this->input->read(this->input, data_chunk_header, 
			    DATA_CHUNK_HEADER_SIZE) != DATA_CHUNK_HEADER_SIZE) {
	this->status = DEMUX_FINISHED;
        return ;
      }

      this->current_data_chunk_packet_count = BE_32(&data_chunk_header[2]);
      this->next_data_chunk_offset = BE_32(&data_chunk_header[6]);
      this->data_chunk_size = chunk_size;
      break;

    default:
      /* this should not occur, but in case it does, skip the chunk */
      lprintf("skipping a chunk!\n");
      this->input->seek(this->input, chunk_size - PREAMBLE_SIZE, SEEK_CUR);
      break;

    }
  }

  /* Read index tables */
  if(INPUT_IS_SEEKABLE(this->input))
    real_parse_index(this);
  
  /* Simple stream selection case - 0/1 audio/video streams */
  if(this->num_video_streams == 1)
    this->video_stream = &this->video_streams[0];
  else
    this->video_stream = NULL;
    
  if(this->num_audio_streams == 1)
    this->audio_stream = &this->audio_streams[0];
  else
    this->audio_stream = NULL;

  /* In the case of multiple audio/video streams select the first
     streams found in the file */
  if((this->num_video_streams > 1) || (this->num_audio_streams > 1)) {
    int   len, offset;
    off_t original_pos = 0;
    char  search_buffer[MAX_PREVIEW_SIZE];

    /* Get data to search through for stream chunks */
    if(INPUT_IS_SEEKABLE(this->input)) {
      original_pos = this->input->get_current_pos(this->input);
    
      if((len = this->input->read(this->input, search_buffer, MAX_PREVIEW_SIZE)) <= 0) {
        xprintf(this->stream->xine, XINE_VERBOSITY_DEBUG, 
                "failed to read header\n");
        this->status = DEMUX_FINISHED;
        return;
      }
      
      offset = 0;
    } else if((this->input->get_capabilities(this->input) & INPUT_CAP_PREVIEW) != 0) {
      if((len = this->input->get_optional_data(this->input, search_buffer, INPUT_OPTIONAL_DATA_PREVIEW)) <= 0) {
        xprintf(this->stream->xine, XINE_VERBOSITY_DEBUG, 
                "failed to read header\n");
        this->status = DEMUX_FINISHED;
        return;
      }
      
      /* Preview data starts at the beginning of the file */
      offset = this->data_start + 18;
    } else {
      xprintf(this->stream->xine, XINE_VERBOSITY_DEBUG,
              "unable to search for correct stream\n");
      this->status = DEMUX_FINISHED;
      return;
    }
    
    while((offset < len) &&
          ((!this->video_stream && (this->num_video_streams > 0)) ||
           (!this->audio_stream && (this->num_audio_streams > 0)))) {
      uint32_t id;
      int      i, stream;
      
      /* Check for end of the data chunk */
      if(((id = BE_32(&search_buffer[0])) == DATA_TAG) ||
         (id == INDX_TAG))
        break;
      
      /* Check that this is a "keyframe" data chunk - in some files
       * there are multiple streams but only one set of keyframe data - 
       * trying to play one without makes horrible noises */
      if(search_buffer[11] & PN_KEYFRAME_FLAG) {
        stream = BE_16(&search_buffer[offset + 4]);

        for(i = 0; !this->video_stream && (i < this->num_video_streams); i++) {
          if(stream == this->video_streams[i].mdpr->stream_number) {
            this->video_stream = &this->video_streams[i];
            lprintf("selecting video stream: %d\n", stream);
          }
        }
      
        for(i = 0; !this->audio_stream && (i < this->num_audio_streams); i++) {
          if(stream == this->audio_streams[i].mdpr->stream_number) {
            this->audio_stream = &this->audio_streams[i];
            lprintf("selecting audio stream: %d\n", stream);
          }
        }
      }

      offset += BE_16(&search_buffer[offset + 2]);
    }
    
    if(INPUT_IS_SEEKABLE(this->input))
      this->input->seek(this->input, original_pos, SEEK_SET);
  }
  
  /* Let the user know if we haven't managed to detect what streams to play */
  if((!this->video_stream && this->num_video_streams) ||
     (!this->audio_stream && this->num_audio_streams)) {
    xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
            "unable to determine which audio/video streams to play\n");
    this->status = DEMUX_FINISHED;
    return;
  }
    
  /* Send headers and set meta info */
  if(this->video_stream) {
    buf_element_t *buf;
      
    /* Send header */
    buf = this->video_fifo->buffer_pool_alloc(this->video_fifo);
    buf->content = buf->mem;
    
    if(this->video_stream->buf_type == BUF_VIDEO_RV10) {
      xine_bmiheader bih;

      bih.biWidth       = BE_16(this->video_stream->mdpr->type_specific_data + 12);
      bih.biHeight      = BE_16(this->video_stream->mdpr->type_specific_data + 14);
      bih.biCompression = BE_32(this->video_stream->mdpr->type_specific_data + 30);
      bih.biSize        = sizeof(bih);
      
      lprintf("setting size to w:%u h:%u for RV10\n", bih.biWidth, bih.biHeight);
      lprintf("setting sub-codec to %X for RV10\n", bih.biCompression);
      memcpy(buf->content, &bih, bih.biSize);
    } else {
      memcpy(buf->content, this->video_stream->mdpr->type_specific_data,
             this->video_stream->mdpr->type_specific_len);

      buf->size = this->video_stream->mdpr->type_specific_len;
    }

    buf->type                   = this->video_stream->buf_type;
    buf->decoder_flags          = BUF_FLAG_HEADER;
    buf->extra_info->input_pos  = 0;
    buf->extra_info->input_time = 0;

    this->video_fifo->put (this->video_fifo, buf);

    /* Set meta info */
    this->stream->stream_info[XINE_STREAM_INFO_HAS_VIDEO] = 1;
    this->stream->stream_info[XINE_STREAM_INFO_VIDEO_FOURCC] =
      this->video_stream->fourcc;
    this->stream->stream_info[XINE_STREAM_INFO_VIDEO_BITRATE] =
      this->video_stream->mdpr->avg_bit_rate;
  }

  if(this->audio_stream) {
    /* Send headers */
    if(this->audio_fifo) {
      buf_element_t *buf;

      buf = this->audio_fifo->buffer_pool_alloc (this->audio_fifo);
      buf->content = buf->mem;

      memcpy(buf->content,
             this->audio_stream->mdpr->type_specific_data + 4,
             this->audio_stream->mdpr->type_specific_len - 4);

      buf->size = this->audio_stream->mdpr->type_specific_len - 4;

      buf->type                   = this->audio_stream->buf_type;
      buf->decoder_flags          = BUF_FLAG_HEADER;
      buf->extra_info->input_pos  = 0;
      buf->extra_info->input_time = 0;

      this->audio_fifo->put (this->audio_fifo, buf);
    }

    /* Set meta info */
    this->stream->stream_info[XINE_STREAM_INFO_HAS_AUDIO] = 1;
    this->stream->stream_info[XINE_STREAM_INFO_AUDIO_FOURCC] =
      this->audio_stream->fourcc;
    this->stream->stream_info[XINE_STREAM_INFO_AUDIO_BITRATE] =
      this->audio_stream->mdpr->avg_bit_rate;
  }
}


/* very naive approach for parsing ram files. it will extract known
 * mrls directly so it should work for simple smil files too.
 * no attempt is made to support smil features:
 * http://service.real.com/help/library/guides/production/htmfiles/smil.htm
 */
static int demux_real_parse_references( demux_real_t *this) {

  char           *buf = NULL;
  int             buf_size = 0;
  int             buf_used = 0;
  int             len, i, j;
  int             alternative = 0;
  xine_mrl_reference_data_t *data;
  xine_event_t    uevent;


  /* read file to memory.
   * warning: dumb code, but hopefuly ok since reference file is small */
  do {
    buf_size += 1024;
    buf = realloc(buf, buf_size+1);

    len = this->input->read(this->input, &buf[buf_used], buf_size-buf_used);

    if( len > 0 )
      buf_used += len;

    /* 50k of reference file? no way. something must be wrong */
    if( buf_used > 50*1024 )
      break;
  } while( len > 0 );

  if(buf_used)
    buf[buf_used] = '\0';

  for(i=0;i<buf_used;i++) {

    /* "--stop--" is used to have pnm alternative for old real clients
     * new real clients will stop processing the file and thus use
     * rtsp protocol.
     */
    if( !strncmp(&buf[i],"--stop--",8) )
      alternative++;

    if( !strncmp(&buf[i],"pnm://",6) || !strncmp(&buf[i],"rtsp://",7) ) {
      for(j=i; buf[j] && buf[j] != '"' && !isspace(buf[j]); j++ )
        ;
      buf[j]='\0';

      uevent.type = XINE_EVENT_MRL_REFERENCE;
      uevent.stream = this->stream;
      uevent.data_length = strlen(&buf[i])+sizeof(xine_mrl_reference_data_t);
      data = malloc(uevent.data_length);
      uevent.data = data;
      strcpy(data->mrl, &buf[i]);
      data->alternative = alternative;
      xine_event_send(this->stream, &uevent);
      free(data);

      i = j;
    }
  }  
  
  free(buf);
  
  this->status = DEMUX_FINISHED;
  return this->status;
}
        
/* redefine abs as macro to handle 64-bit diffs.
   i guess llabs may not be available everywhere */
#define abs(x) ( ((x)<0) ? -(x) : (x) )

#define WRAP_THRESHOLD           220000
#define PTS_AUDIO                0
#define PTS_VIDEO                1

static void check_newpts (demux_real_t *this, int64_t pts, int video, int preview) {
  int64_t diff;

  diff = pts - this->last_pts[video];
  lprintf ("check_newpts %lld\n", pts);

  if (!preview && pts &&
      (this->send_newpts || (this->last_pts[video] && abs(diff)>WRAP_THRESHOLD) ) ) {

    lprintf ("diff=%lld\n", diff);

    if (this->buf_flag_seek) {
      xine_demux_control_newpts(this->stream, pts, BUF_FLAG_SEEK);
      this->buf_flag_seek = 0;
    } else {
      xine_demux_control_newpts(this->stream, pts, 0);
    }
    this->send_newpts = 0;
    this->last_pts[1-video] = 0;
  }

  if (!preview && pts )
    this->last_pts[video] = pts;
}

static int stream_read_char (demux_real_t *this) {
  uint8_t ret;
  this->input->read (this->input, &ret, 1);
  return ret;
}

static int stream_read_word (demux_real_t *this) {
  uint16_t ret;
  this->input->read (this->input, (char *) &ret, 2);
  return BE_16(&ret);
}

static int demux_real_send_chunk(demux_plugin_t *this_gen) {

  demux_real_t   *this = (demux_real_t *) this_gen;
  char            header[DATA_PACKET_HEADER_SIZE];
  int             stream, size, keyframe;
  uint32_t        id, timestamp;
  int64_t         pts;
  off_t           offset;

  if(this->reference_mode)
    return demux_real_parse_references(this);
    
  /* load a header from wherever the stream happens to be pointing */
  if ( (size=this->input->read(this->input, header, DATA_PACKET_HEADER_SIZE)) !=
      DATA_PACKET_HEADER_SIZE) {

    xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG,
             "read failed. wanted %d bytes, but got only %d\n", DATA_PACKET_HEADER_SIZE, size);

    this->status = DEMUX_FINISHED;
    return this->status;
  }

  /* Check to see if we've gone past the end of the data chunk */
  if(((id = BE_32(&header[0])) == DATA_TAG) ||
     (id == INDX_TAG)) {
    lprintf("finished reading data chunk\n");
    this->status = DEMUX_FINISHED;
    return this->status;
  }

  /* read the packet information */
  stream   = BE_16(&header[4]);
  offset   = this->input->get_current_pos(this->input);
  size     = BE_16(&header[2]) - DATA_PACKET_HEADER_SIZE;
  timestamp= BE_32(&header[6]);
  pts      = (int64_t) timestamp * 90;
  keyframe = header[11] & PN_KEYFRAME_FLAG;

  lprintf ("packet of stream %d, 0x%X bytes @ %llX, pts = %lld%s\n",
           stream, size, offset, pts, keyframe ? ", keyframe" : "");

  if (this->video_stream && (stream == this->video_stream->mdpr->stream_number)) {

    int            vpkg_header, vpkg_length, vpkg_offset;
    int            vpkg_seqnum = -1;
    int            vpkg_subseq = 0;
    buf_element_t *buf;
    int            n, fragment_size;

    lprintf ("video chunk detected.\n");

    /* sub-demuxer */

    while (size > 2) {

      /*
       * read packet header
       * bit 7: 1=last block in block chain
       * bit 6: 1=short header (only one block?)
       */

      vpkg_header = stream_read_char (this); size--;
      lprintf ("vpkg_hdr: %02x (size=%d)\n", vpkg_header, size);

      if (0x40==(vpkg_header&0xc0)) {
	/*
	 * seems to be a very short header
	 * 2 bytes, purpose of the second byte yet unknown
	 */
	 int bummer;

	 bummer = stream_read_char (this); size--;
	 lprintf ("bummer == %02X\n",bummer);

	 vpkg_offset = 0;
	 vpkg_length = size;

      } else {

	if (0==(vpkg_header & 0x40)) {
	  /* sub-seqnum (bits 0-6: number of fragment. bit 7: ???) */
	  vpkg_subseq = stream_read_char (this); size--;

	  vpkg_subseq &= 0x7f;
	}

	/*
	 * size of the complete packet
	 * bit 14 is always one (same applies to the offset)
	 */
	vpkg_length = stream_read_word (this); size -= 2;
	if (!(vpkg_length&0xC000)) {
	  vpkg_length <<= 16;
	  vpkg_length |=  stream_read_word (this);
	  size-=2;
	} else
	  vpkg_length &= 0x3fff;

	/*
	 * offset of the following data inside the complete packet
	 * Note: if (hdr&0xC0)==0x80 then offset is relative to the
	 * _end_ of the packet, so it's equal to fragment size!!!
	 */

	vpkg_offset = stream_read_word (this); size -= 2;

	if (!(vpkg_offset&0xC000)) {
	  vpkg_offset <<= 16;
	  vpkg_offset |=  stream_read_word (this);
	  size -= 2;
	} else
	  vpkg_offset &= 0x3fff;

	vpkg_seqnum = stream_read_char (this); size--;
      }

      lprintf ("seq=%d, offset=%d, length=%d, size=%d, frag size=%d, flags=%02x\n",
               vpkg_seqnum, vpkg_offset, vpkg_length, size, this->fragment_size,
               vpkg_header);

      if (vpkg_seqnum != this->old_seqnum) {
        lprintf ("new seqnum\n");

	this->fragment_size = 0;
	this->old_seqnum = vpkg_seqnum;
      }

      buf = this->video_fifo->buffer_pool_alloc (this->video_fifo);

      buf->content       = buf->mem;
      buf->pts           = pts;
      buf->extra_info->input_pos     = this->input->get_current_pos (this->input);

      /* if we have a seekable stream then use the timestamp for the data
       * packet for more accurate seeking - if not then estimate time using
       * average bitrate */
      if(this->video_stream->index)
        buf->extra_info->input_time = timestamp;
      else
        buf->extra_info->input_time = (int)((int64_t)buf->extra_info->input_pos 
                                             * 8 * 1000 / this->avg_bitrate);

      buf->type          = this->video_stream->buf_type;
      
      if(this->data_start && this->data_chunk_size)
        buf->extra_info->input_length = this->data_start + 18 + this->data_chunk_size;
        
      if(this->duration)
        buf->extra_info->total_time = this->duration;
      
      check_newpts (this, pts, PTS_VIDEO, 0);

      if (this->fragment_size == 0) {
	lprintf ("new packet starting\n");
	buf->decoder_flags = BUF_FLAG_FRAME_START;
      } else {
	lprintf ("continuing packet \n");
	buf->decoder_flags = 0;
      }

      /*
       * calc size of fragment
       */

      if ((vpkg_header & 0xc0) == 0x080)
	fragment_size = vpkg_offset;
      else {
	if (0x00 == (vpkg_header&0xc0))
	  fragment_size = size;
	else
	  fragment_size = vpkg_length;
      }
      lprintf ("fragment size is %d\n", fragment_size);

      /*
       * read fragment_size bytes of data
       */

      n = this->input->read (this->input, buf->content, fragment_size);

      buf->size = fragment_size;

      if (n<fragment_size) {
	xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG, "read error %d/%d\n", n, fragment_size);
	buf->free_buffer(buf);
	this->status = DEMUX_FINISHED;
	return this->status;
      }

      this->video_fifo->put (this->video_fifo, buf);

      size -= fragment_size;
      lprintf ("demux_real: size left %d\n", size);

      this->fragment_size += fragment_size;

      if (this->fragment_size >= vpkg_length) {
        lprintf ("fragment finished (%d/%d)\n", this->fragment_size, vpkg_length);
        this->fragment_size = 0;
      }

    } /* while(size>2) */

  } else if (this->audio_fifo && this->audio_stream &&
             (stream == this->audio_stream->mdpr->stream_number)) {

    buf_element_t *buf;
    int            n;

    lprintf ("audio chunk detected.\n");

    if(this->audio_need_keyframe && !keyframe)
      goto discard;
    else
      this->audio_need_keyframe = 0;
    
    buf = this->audio_fifo->buffer_pool_alloc (this->audio_fifo);

    buf->content       = buf->mem;
    buf->pts           = pts;
    buf->extra_info->input_pos     = this->input->get_current_pos (this->input);
    
    /* if we have a seekable stream then use the timestamp for the data
     * packet for more accurate seeking - if not then estimate time using
     * average bitrate */
    if(this->audio_stream->index)
      buf->extra_info->input_time = timestamp;
    else
      buf->extra_info->input_time = (int)((int64_t)buf->extra_info->input_pos 
                                           * 8 * 1000 / this->avg_bitrate); 

    buf->type          = this->audio_stream->buf_type;
    buf->decoder_flags = 0;
    buf->size          = size;

    if(this->data_start && this->data_chunk_size)
      buf->extra_info->input_length = this->data_start + 18 + this->data_chunk_size;
        
    if(this->duration)
      buf->extra_info->total_time = this->duration;
    
    check_newpts (this, pts, PTS_AUDIO, 0);

    n = this->input->read (this->input, buf->content, size);

    if (n<size || size < 0) {
      xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG, "read error 44\n");

      buf->free_buffer(buf);
      this->status = DEMUX_FINISHED;
      return this->status;
    }

    this->audio_fifo->put (this->audio_fifo, buf);  

    /* FIXME: dp->flags = (flags & 0x2) ? 0x10 : 0; */

  } else {

    /* discard */
    lprintf ("chunk not detected; discarding.\n");

discard:
    this->input->seek(this->input, size, SEEK_CUR);

  }

#if 0

  this->current_data_chunk_packet_count--;

  /* check if it's time to reload */
  if (!this->current_data_chunk_packet_count &&
      this->next_data_chunk_offset) {
    char            preamble[PREAMBLE_SIZE];
    unsigned char   data_chunk_header[DATA_CHUNK_HEADER_SIZE];

    /* seek to the next DATA chunk offset */
    this->input->seek(this->input, this->next_data_chunk_offset, SEEK_SET);

    /* load the DATA chunk preamble */
    if (this->input->read(this->input, preamble, PREAMBLE_SIZE) !=
      PREAMBLE_SIZE) {
      this->status = DEMUX_FINISHED;
      return this->status;
    }

    /* load the rest of the DATA chunk header */
    if (this->input->read(this->input, data_chunk_header,
      DATA_CHUNK_HEADER_SIZE) != DATA_CHUNK_HEADER_SIZE) {
      this->status = DEMUX_FINISHED;
      return this->status;
    }
    lprintf ("**** found next DATA tag\n");
    this->current_data_chunk_packet_count = BE_32(&data_chunk_header[2]);
    this->next_data_chunk_offset = BE_32(&data_chunk_header[6]);
  }

  if (!this->current_data_chunk_packet_count) {
    this->status = DEMUX_FINISHED;
    return this->status;
  }

#endif

  return this->status;
}

static void demux_real_send_headers(demux_plugin_t *this_gen) {

  demux_real_t *this = (demux_real_t *) this_gen;

  this->video_fifo = this->stream->video_fifo;
  this->audio_fifo = this->stream->audio_fifo;

  this->status = DEMUX_OK;

  this->last_pts[0]   = 0;
  this->last_pts[1]   = 0;

  this->avg_bitrate   = 1;


  /* send start buffers */
  xine_demux_control_start(this->stream);

  /* send init info to decoders */

  this->input->seek (this->input, 0, SEEK_SET);

  this->stream->stream_info[XINE_STREAM_INFO_HAS_VIDEO] = 0;
  this->stream->stream_info[XINE_STREAM_INFO_HAS_AUDIO] = 0;

  if( !this->reference_mode ) {
    real_parse_headers (this);
  } else {
    if ((this->input->get_capabilities (this->input) & INPUT_CAP_SEEKABLE) != 0)
      this->input->seek (this->input, 0, SEEK_SET);
  }
}

static int demux_real_seek (demux_plugin_t *this_gen,
                             off_t start_pos, int start_time) {

  demux_real_t       *this = (demux_real_t *) this_gen;
  real_index_entry_t *index, *other_index = NULL;
  int                 i = 0, entries;

  if((this->input->get_capabilities(this->input) & INPUT_CAP_SEEKABLE) &&
     ((this->audio_stream && this->audio_stream->index) ||
      (this->video_stream && this->video_stream->index))) {

    /* video index has priority over audio index */
    if(this->video_stream && this->video_stream->index) {
      index = this->video_stream->index;
      entries = this->video_stream->index_entries;
      if(this->audio_stream)
        other_index = this->audio_stream->index;

      /* when seeking by video index the first audio chunk won't necesserily
       * be a keyframe which would upset the decoder */
      this->audio_need_keyframe = 1;
    } else {
      index = this->audio_stream->index;
      entries = this->audio_stream->index_entries;
      if(this->video_stream)
        other_index = this->video_stream->index;
    }

    if(start_pos) {
      while((index[i+1].offset < start_pos) && (i < entries - 1))
        i++;
    } else if(start_time) {
      while((index[i+1].timestamp < start_time) && (i < entries - 1))
        i++;
    }

    /* make sure we don't skip past audio/video at start of file */
    if((i == 0) && other_index && (other_index[0].offset < index[0].offset))
      index = other_index;

    this->input->seek(this->input, index[i].offset, SEEK_SET);

    if(this->stream->demux_thread_running) {
      this->buf_flag_seek = 1;
      xine_demux_flush_engine(this->stream);
    }
  }

  this->send_newpts     = 1;
  this->old_seqnum      = -1;
  this->fragment_size   = 0;
  this->status          = DEMUX_OK;

  return this->status;
}

static void demux_real_dispose (demux_plugin_t *this_gen) {
  demux_real_t *this = (demux_real_t *) this_gen;
  int i;

  for(i = 0; i < this->num_video_streams; i++) {
    real_free_mdpr(this->video_streams[i].mdpr);
    if(this->video_streams[i].index)
      free(this->video_streams[i].index);
  }
  
  for(i = 0; i < this->num_audio_streams; i++) {
    real_free_mdpr(this->audio_streams[i].mdpr);
    if(this->audio_streams[i].index)
      free(this->audio_streams[i].index);
  }
  
  free(this);
}

static int demux_real_get_status (demux_plugin_t *this_gen) {
  demux_real_t *this = (demux_real_t *) this_gen;

  return this->status;
}

static int demux_real_get_stream_length (demux_plugin_t *this_gen) {
  demux_real_t *this = (demux_real_t *) this_gen;

  /* duration is stored in the file as milliseconds */
  return this->duration;
}

static uint32_t demux_real_get_capabilities(demux_plugin_t *this_gen) {
  return DEMUX_CAP_NOCAP;
}

static int demux_real_get_optional_data(demux_plugin_t *this_gen,
					void *data, int data_type) {
  return DEMUX_OPTIONAL_UNSUPPORTED;
}

/* help function to discover stream type. returns:
 *  0 if not known.
 *  1 if normal stream.
 *  2 if reference stream.
 */
static int real_check_stream_type(uint8_t *buf, int len)
{
  if ((buf[0] == 0x2e)
      && (buf[1] == 'R')
      && (buf[2] == 'M')
      && (buf[3] == 'F'))
    return 1;

  buf[len] = '\0';
  if( strstr(buf,"pnm://") || strstr(buf,"rtsp://") || strstr(buf,"<smil>") )
    return 2;

  return 0;
}

static demux_plugin_t *open_plugin (demux_class_t *class_gen, xine_stream_t *stream,
                                    input_plugin_t *input) {

  demux_real_t   *this;
  uint8_t buf[1024+1];
  int len, stream_type=0;

  switch (stream->content_detection_method) {

  case METHOD_BY_CONTENT:{

    if (! (len = xine_demux_read_header(input, buf, 1024)) )
      return NULL;

    lprintf ("read 4 bytes: %02x %02x %02x %02x\n",
             buf[0], buf[1], buf[2], buf[3]);

    if (!(stream_type = real_check_stream_type(buf,len)))
      return NULL;
  }

  lprintf ("by content accepted.\n");
  break;

  case METHOD_BY_EXTENSION: {
    char *extensions, *mrl;

    mrl = input->get_mrl (input);
    extensions = class_gen->get_extensions (class_gen);

    lprintf ("by extension '%s'\n", mrl);

    if (!xine_demux_check_extension (mrl, extensions)) {
      return NULL;
    }
    lprintf ("by extension accepted.\n");
  }

  break;

  case METHOD_EXPLICIT:
    break;

  default:
    return NULL;
  }


  this         = xine_xmalloc (sizeof (demux_real_t));
  this->stream = stream;
  this->input  = input;


  /* discover stream type */
  if(!stream_type)
    if ( (len = xine_demux_read_header(this->input, buf, 1024)) )
      stream_type = real_check_stream_type(buf,len);

  if(stream_type == 2){
    this->reference_mode = 1;
    lprintf("reference stream detected\n");
  }else
    this->reference_mode = 0;


  this->demux_plugin.send_headers      = demux_real_send_headers;
  this->demux_plugin.send_chunk        = demux_real_send_chunk;
  this->demux_plugin.seek              = demux_real_seek;
  this->demux_plugin.dispose           = demux_real_dispose;
  this->demux_plugin.get_status        = demux_real_get_status;
  this->demux_plugin.get_stream_length = demux_real_get_stream_length;
  this->demux_plugin.get_video_frame   = NULL;
  this->demux_plugin.got_video_frame_cb= NULL;
  this->demux_plugin.get_capabilities  = demux_real_get_capabilities;
  this->demux_plugin.get_optional_data = demux_real_get_optional_data;
  this->demux_plugin.demux_class       = class_gen;

  return &this->demux_plugin;
}

static char *get_description (demux_class_t *this_gen) {
  return "RealMedia file demux plugin";
}

static char *get_identifier (demux_class_t *this_gen) {
  return "Real";
}

static char *get_extensions (demux_class_t *this_gen) {
  return "rm ram";
}

static char *get_mimetypes (demux_class_t *this_gen) {
  return "audio/x-pn-realaudio: ra, rm, ram: Real Media file;"
         "audio/x-pn-realaudio-plugin: rpm: Real Media plugin file;"
         "audio/x-real-audio: ra, rm, ram: Real Media file;"
         "application/vnd.rn-realmedia: ra, rm, ram: Real Media file;"; 
}

static void class_dispose (demux_class_t *this_gen) {
  demux_real_class_t *this = (demux_real_class_t *) this_gen;

  free (this);
}

static void *init_class (xine_t *xine, void *data) {
  demux_real_class_t     *this;

  this = xine_xmalloc (sizeof (demux_real_class_t));

  this->demux_class.open_plugin     = open_plugin;
  this->demux_class.get_description = get_description;
  this->demux_class.get_identifier  = get_identifier;
  this->demux_class.get_mimetypes   = get_mimetypes;
  this->demux_class.get_extensions  = get_extensions;
  this->demux_class.dispose         = class_dispose;

  return this;
}

/*
 * exported plugin catalog entry
 */

plugin_info_t xine_plugin_info[] = {
  /* type, API, "name", version, special_info, init_function */
  { PLUGIN_DEMUX, 22, "real", XINE_VERSION_CODE, NULL, init_class },
  { PLUGIN_NONE, 0, "", 0, NULL, NULL }
};
