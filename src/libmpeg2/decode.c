/*
 * decode.c
 * Copyright (C) 2000-2002 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 * See http://libmpeg2.sourceforge.net/ for updates.
 *
 * mpeg2dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpeg2dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * xine-specific version by G. Bartsch
 *
 */

#include "config.h"

#include <stdio.h>
#include <string.h>	/* memcpy/memset, try to remove */
#include <stdlib.h>
#include <inttypes.h>

#include "xine_internal.h"
#include "video_out.h"
#include "mpeg2.h"
#include "mpeg2_internal.h"
#include "xineutils.h"

/*
#define LOG_PAN_SCAN
*/

/*
#define LOG
*/

/* #define BUFFER_SIZE (224 * 1024) */
#define BUFFER_SIZE (1194 * 1024) /* new buffer size for mpeg2dec 0.2.1 */

static void process_userdata(mpeg2dec_t *mpeg2dec, uint8_t *buffer);

void mpeg2_init (mpeg2dec_t * mpeg2dec, 
		 xine_video_port_t * output)
{
  static int do_init = 1;
  uint32_t mm_accel;

    if (do_init) {
	do_init = 0;
	mm_accel = xine_mm_accel();
	mpeg2_cpu_state_init (mm_accel);
	mpeg2_idct_init (mm_accel);
	mpeg2_mc_init (mm_accel);
    }

    if( !mpeg2dec->chunk_buffer )
      mpeg2dec->chunk_buffer = xine_xmalloc_aligned (16, BUFFER_SIZE + 4, 
						     (void**)&mpeg2dec->chunk_base);
    if( !mpeg2dec->picture )
      mpeg2dec->picture = xine_xmalloc_aligned (16, sizeof (picture_t),
						(void**)&mpeg2dec->picture_base);

    mpeg2dec->shift = 0xffffff00;
    mpeg2dec->new_sequence = 0;
    mpeg2dec->is_sequence_needed = 1;
    mpeg2dec->is_wait_for_ip_frames = 2;
    mpeg2dec->frames_to_drop = 0;
    mpeg2dec->drop_frame = 0;
    mpeg2dec->in_slice = 0;
    mpeg2dec->output = output;
    mpeg2dec->chunk_ptr = mpeg2dec->chunk_buffer;
    mpeg2dec->code = 0xb4;
    mpeg2dec->seek_mode = 0;

    memset (mpeg2dec->picture, 0, sizeof (picture_t));

    /* initialize substructures */
    mpeg2_header_state_init (mpeg2dec->picture);

    if( output->get_capabilities(output) & VO_CAP_XVMC_MOCOMP ) {
      printf("libmpeg2: output port has XvMC capability\n");
      mpeg2dec->frame_format = XINE_IMGFMT_XVMC;
    } else {
      mpeg2dec->frame_format = XINE_IMGFMT_YV12;
    }
}

static inline void get_frame_duration (mpeg2dec_t * mpeg2dec, vo_frame_t *frame)
{
  switch (mpeg2dec->picture->frame_rate_code) {
  case 1: /* 23.976 fps */
    frame->duration      = 3754;  /* actually it's 3753.75 */
    break;
  case 2: /* 24 fps */
    frame->duration      = 3750;
    break;
  case 3: /* 25 fps */
    frame->duration      = 3600;
    break;
  case 4: /* 29.97 fps */
    frame->duration      = 3003;
    break;
  case 5: /* 30 fps */
    frame->duration      = 3000;
    break;
  case 6: /* 50 fps */
    frame->duration      = 1800;
    break;
  case 7: /* 59.94 fps */
    frame->duration      = 1502;  /* actually it's 1501.5 */
    break;
  case 8: /* 60 fps */
    frame->duration      = 1500;
    break;
  default:
       /* printf ("invalid/unknown frame rate code : %d \n",
               frame->frame_rate_code); */
    frame->duration      = 0;
  }
  
  frame->duration = frame->duration *
    ((mpeg2dec->picture->frame_rate_ext_n + 1) /
     (mpeg2dec->picture->frame_rate_ext_d + 1));
 
  /* this should be used to detect any special rff pattern */
  mpeg2dec->rff_pattern = mpeg2dec->rff_pattern << 1;
  mpeg2dec->rff_pattern |= !!frame->repeat_first_field;

  if( ((mpeg2dec->rff_pattern & 0xff) == 0xaa ||
      (mpeg2dec->rff_pattern & 0xff) == 0x55) &&
      !mpeg2dec->picture->progressive_sequence ) {
    /* special case for ntsc 3:2 pulldown */
    frame->duration += frame->duration/4;
  }
  else
  {  
    if( frame->repeat_first_field ) {
      if( !mpeg2dec->picture->progressive_sequence &&
           mpeg2dec->picture->progressive_frame ) {
        /* decoder should output 3 fields, so adjust duration to
           count on this extra field time */
        frame->duration += frame->duration/2;     
      } else if( mpeg2dec->picture->progressive_sequence ) {
        /* for progressive sequences the output should repeat the
           frame 1 or 2 times depending on top_field_first flag. */
        frame->duration *= (mpeg2dec->picture->top_field_first)?3:2;
      }
    }
  }
  
  /*printf("mpeg2dec: rff=%u\n",frame->repeat_first_field);*/
} 

static double get_aspect_ratio(mpeg2dec_t *mpeg2dec)
{
  double ratio;
  picture_t * picture = mpeg2dec->picture;
  double mpeg1_pel_ratio[16] = {1.0 /* forbidden */,
    1.0, 0.6735, 0.7031, 0.7615, 0.8055, 0.8437, 0.8935, 0.9157,
    0.9815, 1.0255, 1.0695, 1.0950, 1.1575, 1.2015, 1.0 /*reserved*/ };

  if( !picture->mpeg1 ) {
    /* these hardcoded values are defined on mpeg2 standard for
     * aspect ratio. other values are reserved or forbidden.  */
    switch(picture->aspect_ratio_information) {
    case 2:
      ratio = 4.0/3.0;
      break;
    case 3:
      ratio = 16.0/9.0;
      break;
    case 4:
      ratio = 2.11/1.0;
      break;
    case 1:
    default:
      ratio = (double)picture->coded_picture_width/(double)picture->coded_picture_height;
      break;
    }
  } else {
    /* mpeg1 constants refer to pixel aspect ratio */
    ratio = (double)picture->coded_picture_width/(double)picture->coded_picture_height;
    ratio /= mpeg1_pel_ratio[picture->aspect_ratio_information];
  }

  return ratio;
}

static void remember_metainfo (mpeg2dec_t *mpeg2dec) {

  picture_t * picture = mpeg2dec->picture;

  mpeg2dec->stream->stream_info[XINE_STREAM_INFO_VIDEO_WIDTH]  = picture->frame_width;
  mpeg2dec->stream->stream_info[XINE_STREAM_INFO_VIDEO_HEIGHT] = picture->frame_height;
  mpeg2dec->stream->stream_info[XINE_STREAM_INFO_VIDEO_RATIO] = 
    ((double)10000 * get_aspect_ratio(mpeg2dec));

  switch (mpeg2dec->picture->frame_rate_code) {
  case 1: /* 23.976 fps */
    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_FRAME_DURATION]      = 3913;
    break;
  case 2: /* 24 fps */
    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_FRAME_DURATION]      = 3750;
    break;
  case 3: /* 25 fps */
    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_FRAME_DURATION]      = 3600;
    break;
  case 4: /* 29.97 fps */
    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_FRAME_DURATION]      = 3003;
    break;
  case 5: /* 30 fps */
    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_FRAME_DURATION]      = 3000;
    break;
  case 6: /* 50 fps */
    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_FRAME_DURATION]      = 1800;
    break;
  case 7: /* 59.94 fps */
    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_FRAME_DURATION]      = 1525;
    break;
  case 8: /* 60 fps */
    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_FRAME_DURATION]      = 1509;
    break;
  default:
       /* printf ("invalid/unknown frame rate code : %d \n",
               frame->frame_rate_code); */
    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_FRAME_DURATION]      = 3000;
  }

  mpeg2dec->stream->meta_info[XINE_META_INFO_VIDEOCODEC]  = strdup ("MPEG");
}


static inline int parse_chunk (mpeg2dec_t * mpeg2dec, int code,
			       uint8_t * buffer)
{
    picture_t * picture;
    int is_frame_done;
    
    /* wait for sequence_header_code */
    if (mpeg2dec->is_sequence_needed) {
      if (code != 0xb3) {
        /* printf ("libmpeg2: waiting for sequence header\n");  */
	mpeg2dec->pts = 0;
	return 0;
      }
    }
    if (mpeg2dec->is_frame_needed) {
      /* printf ("libmpeg2: waiting for frame start\n");  */
      mpeg2dec->pts = 0;
      if (mpeg2dec->picture->current_frame)
        mpeg2dec->picture->current_frame->bad_frame = 1;
    }

    mpeg2_stats (code, buffer);

    picture = mpeg2dec->picture;
    is_frame_done = mpeg2dec->in_slice && ((!code) || (code >= 0xb0));

    if (is_frame_done)
	mpeg2dec->in_slice = 0;
    
    if (is_frame_done && picture->current_frame != NULL) {

	if (((picture->picture_structure == FRAME_PICTURE) ||
	     (picture->second_field)) ) {
	  
	  if (mpeg2dec->drop_frame)
	    picture->current_frame->bad_frame = 1;
   
	  if (picture->picture_coding_type == B_TYPE) {
	    if( picture->current_frame && !picture->current_frame->drawn ) {

	      /* hack against wrong mpeg1 pts */
	      if (picture->mpeg1) 
	        picture->current_frame->pts = 0;

	      get_frame_duration(mpeg2dec, picture->current_frame);
	      mpeg2dec->frames_to_drop = picture->current_frame->draw (picture->current_frame, mpeg2dec->stream);
	      picture->current_frame->drawn = 1;
	    }
	  } else if (picture->forward_reference_frame && !picture->forward_reference_frame->drawn) {
	    get_frame_duration(mpeg2dec, picture->forward_reference_frame);
	    mpeg2dec->frames_to_drop = picture->forward_reference_frame->draw (picture->forward_reference_frame,
									       mpeg2dec->stream);
	    picture->forward_reference_frame->drawn = 1;
	  }
	}
    }

    switch (code) {
    case 0x00:	/* picture_start_code */
	if (mpeg2_header_picture (picture, buffer)) {
	    fprintf (stderr, "bad picture header\n");
	    abort();
	}

	mpeg2dec->is_frame_needed=0;

	if (!picture->second_field) {
	  /* find out if we want to skip this frame */
	  mpeg2dec->drop_frame = 0;
	  
	  /* picture->skip_non_intra_dct = (mpeg2dec->frames_to_drop>0) ; */
	  
	  switch (picture->picture_coding_type) {
	  case B_TYPE:
	    
#ifdef LOG
	    printf ("libmpeg2: B-Frame\n");
#endif

	    if (mpeg2dec->frames_to_drop>1) {
#ifdef LOG
	      printf ("libmpeg2: dropping b-frame because frames_to_drop==%d\n",
		      mpeg2dec->frames_to_drop);
#endif
	      mpeg2dec->drop_frame = 1;
	    } else if (!picture->forward_reference_frame || picture->forward_reference_frame->bad_frame 
		       || !picture->backward_reference_frame || picture->backward_reference_frame->bad_frame) {
#ifdef LOG
	      printf ("libmpeg2: dropping b-frame because ref is bad (");
	      if (picture->forward_reference_frame)
		printf ("fw ref frame %d, bad %d;", picture->forward_reference_frame->id,
			picture->forward_reference_frame->bad_frame);
	      else
		printf ("fw ref frame not there;");
	      if (picture->backward_reference_frame)
		printf ("bw ref frame %d, bad %d)\n", picture->backward_reference_frame->id,
			picture->backward_reference_frame->bad_frame);
	      else
		printf ("fw ref frame not there)\n");
#endif
	      mpeg2dec->drop_frame = 1;
	    } else if (mpeg2dec->is_wait_for_ip_frames > 0) {
#ifdef LOG
	      printf("libmpeg2: dropping b-frame because refs are invalid\n");
#endif
	      mpeg2dec->drop_frame = 1;
	    }
	    break;
	    
	  case P_TYPE:
	    
#ifdef LOG
	    printf ("libmpeg2: P-Frame\n");
#endif

	    if (mpeg2dec->frames_to_drop>2) {
	      mpeg2dec->drop_frame = 1;
#ifdef LOG
	      printf ("libmpeg2: dropping p-frame because frames_to_drop==%d\n",
		      mpeg2dec->frames_to_drop);
#endif
	    } else if (!picture->backward_reference_frame || picture->backward_reference_frame->bad_frame) {
	      mpeg2dec->drop_frame = 1;
#ifdef LOG
	      if (!picture->backward_reference_frame)
		printf ("libmpeg2: dropping p-frame because no ref frame\n");
	      else
		printf ("libmpeg2: dropping p-frame because ref %d is bad\n", picture->backward_reference_frame->id);
#endif
	    } else if (mpeg2dec->is_wait_for_ip_frames > 1) {
#ifdef LOG
	      printf("libmpeg2: dropping p-frame because ref is invalid\n");
#endif
	      mpeg2dec->drop_frame = 1;
	    } else if (mpeg2dec->is_wait_for_ip_frames)
	      mpeg2dec->is_wait_for_ip_frames--;
	    
	    break;
	    
	  case I_TYPE:
#ifdef LOG
	    printf ("libmpeg2: I-Frame\n");
#endif
	    /* for the sake of dvd menus, never drop i-frames
	    if (mpeg2dec->frames_to_drop>4) {
	      mpeg2dec->drop_frame = 1;
	    }
	    */
	    
	    if (mpeg2dec->is_wait_for_ip_frames)
	      mpeg2dec->is_wait_for_ip_frames--;
	    
	    break;
	  }
	}

	break;

    case 0xb2: /* user data code */
        process_userdata(mpeg2dec, buffer);
        break;

    case 0xb3:	/* sequence_header_code */
	if (mpeg2_header_sequence (picture, buffer)) {
	    fprintf (stderr, "bad sequence header\n");
	    /* abort(); */
	    break;
	}
	if (mpeg2dec->force_aspect) picture->aspect_ratio_information = mpeg2dec->force_aspect;

	if (mpeg2dec->is_sequence_needed ) {
	    mpeg2dec->new_sequence = 1;
	}

	if (mpeg2dec->is_sequence_needed 
	    || (picture->frame_width != picture->coded_picture_width)
	    || (picture->frame_height != picture->coded_picture_height)) {
	    xine_event_t event;
	    xine_format_change_data_t data;
	    
	    remember_metainfo (mpeg2dec);
	    event.type = XINE_EVENT_FRAME_FORMAT_CHANGE;
	    event.stream = mpeg2dec->stream;
	    event.data = &data;
	    event.data_length = sizeof(data);
	    data.width = picture->coded_picture_width;
	    data.height = picture->coded_picture_height;
	    data.aspect = picture->aspect_ratio_information;
	    data.pan_scan = mpeg2dec->force_pan_scan;
	    xine_event_send(mpeg2dec->stream, &event);

            mpeg2dec->stream->stream_info[XINE_STREAM_INFO_VIDEO_WIDTH]
              = picture->coded_picture_width;
	    mpeg2dec->stream->stream_info[XINE_STREAM_INFO_VIDEO_HEIGHT]
              = picture->coded_picture_height;

	    if (picture->forward_reference_frame &&
		picture->forward_reference_frame != picture->current_frame &&
		picture->forward_reference_frame != picture->backward_reference_frame)
	      picture->forward_reference_frame->free (picture->forward_reference_frame);
	  
	    if (picture->backward_reference_frame &&
		picture->backward_reference_frame != picture->current_frame)
	      picture->backward_reference_frame->free (picture->backward_reference_frame);

	    mpeg2dec->is_sequence_needed = 0;
	    picture->forward_reference_frame = NULL;
	    picture->backward_reference_frame = NULL;

	    picture->frame_width = picture->coded_picture_width;
	    picture->frame_height = picture->coded_picture_height;
	}
	break;

    case 0xb5:	/* extension_start_code */
	if (mpeg2_header_extension (picture, buffer)) {
	    fprintf (stderr, "bad extension\n");
	    abort();
	}
	break;

    case 0xb7:	/* sequence end code */
#ifdef LOG_PAN_SCAN
      printf ("libmpeg2: sequence end code not handled\n");
#endif
    case 0xb8:	/* group of pictures start code */
	if (mpeg2_header_group_of_pictures (picture, buffer)) {
	  printf ("libmpeg2: bad group of pictures\n");
	  abort();
	}
    default:
	if (code >= 0xb9)
	  printf ("libmpeg2: stream not demultiplexed ?\n");

	if (code >= 0xb0)
	    break;

	if (!(mpeg2dec->in_slice)) {
	    mpeg2dec->in_slice = 1;

	    if (picture->second_field) {
	      if (picture->current_frame)
		picture->current_frame->field(picture->current_frame, 
					      picture->picture_structure);
	      else
		mpeg2dec->drop_frame = 1;		
	    } else {
		int flags = VO_INTERLACED_FLAG | picture->picture_structure;
		if (mpeg2dec->force_pan_scan) flags |= VO_PAN_SCAN_FLAG;
		if (mpeg2dec->new_sequence) flags |= VO_NEW_SEQUENCE_FLAG;
		
		if ( picture->current_frame && 
		     picture->current_frame != picture->backward_reference_frame &&
		     picture->current_frame != picture->forward_reference_frame ) {
			picture->current_frame->free (picture->current_frame);
		}
		if (picture->picture_coding_type == B_TYPE)
		    picture->current_frame =
		        mpeg2dec->stream->video_out->get_frame (mpeg2dec->stream->video_out,
						     picture->coded_picture_width,
						     picture->coded_picture_height,
						     get_aspect_ratio(mpeg2dec),
						     mpeg2dec->frame_format,
						     flags);
		else {
		    picture->current_frame =
		        mpeg2dec->stream->video_out->get_frame (mpeg2dec->stream->video_out,
						     picture->coded_picture_width,
						     picture->coded_picture_height,
						     get_aspect_ratio(mpeg2dec),
						     mpeg2dec->frame_format,
						     flags);
		    if (picture->forward_reference_frame &&
		        picture->forward_reference_frame != picture->backward_reference_frame)
		      picture->forward_reference_frame->free (picture->forward_reference_frame);

		    picture->forward_reference_frame =
			picture->backward_reference_frame;
		    picture->backward_reference_frame = picture->current_frame;
		}
		if(mpeg2dec->new_sequence)
		{
		    picture->mc = picture->current_frame->macroblocks;
		    mpeg2dec->new_sequence = 0;
		}
		picture->current_frame->bad_frame          = 1;
		picture->current_frame->drawn              = 0;
		picture->current_frame->pts                = mpeg2dec->pts;
                picture->current_frame->top_field_first    = picture->top_field_first;
                picture->current_frame->repeat_first_field = picture->repeat_first_field;
                picture->current_frame->progressive_frame  = picture->progressive_frame;

                switch( picture->picture_coding_type ) {
                  case I_TYPE:
                    picture->current_frame->picture_coding_type = XINE_PICT_I_TYPE;
                    break;
                  case P_TYPE:
                    picture->current_frame->picture_coding_type = XINE_PICT_P_TYPE;
                    break;
                  case B_TYPE:
                    picture->current_frame->picture_coding_type = XINE_PICT_B_TYPE;
                    break;
                  case D_TYPE:
                    picture->current_frame->picture_coding_type = XINE_PICT_D_TYPE;
                    break;
                }

#ifdef LOG
		printf ("libmpeg2: decoding frame %d, type %s\n",
			picture->current_frame->id, picture->picture_coding_type == I_TYPE ? "I" :
			picture->picture_coding_type == P_TYPE ? "P" : "B");
#endif
		mpeg2dec->pts = 0;
	    }
	}

	if (!mpeg2dec->drop_frame && picture->current_frame != NULL) {
#ifdef DEBUG_LOG
	  printf("slice target %08x past %08x future %08x\n",picture->current_frame,picture->forward_reference_frame,picture->backward_reference_frame);
	  fflush(stdout);
#endif

	  if(picture->mc && picture->mc->xvmc_accel) {
	    mpeg2_xvmc_slice (picture, code, buffer);
	    
	  } else {
	    mpeg2_slice (picture, code, buffer);
	  }

	  if( picture->v_offset > picture->limit_y ) { 
	    picture->current_frame->bad_frame = 0;
#ifdef LOG
	    printf("libmpeg2: frame %d successfuly decoded\n",picture->current_frame->id); 
#endif
	  }
	}
    }

    /* printf ("libmpeg2: parse_chunk %d completed\n", code);  */
    return is_frame_done;
}

static inline uint8_t * copy_chunk (mpeg2dec_t * mpeg2dec,
				    uint8_t * current, uint8_t * end)
{
    uint32_t shift;
    uint8_t * chunk_ptr;
    uint8_t * limit;
    uint8_t byte;

    shift = mpeg2dec->shift;
    chunk_ptr = mpeg2dec->chunk_ptr;
    limit = current + (mpeg2dec->chunk_buffer + BUFFER_SIZE - chunk_ptr);
    if (limit > end)
	limit = end;

    while (1) {

	byte = *current++;
	if (shift != 0x00000100) {
	    shift = (shift | byte) << 8;
	    *chunk_ptr++ = byte;
	    if (current < limit)
		continue;
	    if (current == end) {
		mpeg2dec->chunk_ptr = chunk_ptr;
		mpeg2dec->shift = shift;
		return NULL;
	    } else {
		/* we filled the chunk buffer without finding a start code */
		mpeg2dec->code = 0xb4;	/* sequence_error_code */
		mpeg2dec->chunk_ptr = mpeg2dec->chunk_buffer;
		return current;
	    }
	}
	mpeg2dec->code = byte;
	mpeg2dec->chunk_ptr = mpeg2dec->chunk_buffer;
	mpeg2dec->shift = 0xffffff00;
	return current;
    }
}

int mpeg2_decode_data (mpeg2dec_t * mpeg2dec, uint8_t * current, uint8_t * end,
		       uint64_t pts)
{
    int ret;
    uint8_t code;

    ret = 0;
    if (mpeg2dec->seek_mode) {
      mpeg2dec->chunk_ptr = mpeg2dec->chunk_buffer;
      mpeg2dec->code = 0xb4;
      mpeg2dec->seek_mode = 0;
      mpeg2dec->shift = 0xffffff00;
      mpeg2dec->is_frame_needed = 1;
    }

    if (pts)
      mpeg2dec->pts = pts;

    while (current != end) {
	code = mpeg2dec->code;
	current = copy_chunk (mpeg2dec, current, end);
	if (current == NULL)
	    return ret;
	ret += parse_chunk (mpeg2dec, code, mpeg2dec->chunk_buffer);
    }

    return ret;
}

void mpeg2_discontinuity (mpeg2dec_t * mpeg2dec) {
  picture_t *picture = mpeg2dec->picture;

  if( !picture )
    return;
  
  mpeg2dec->pts = 0;  
  if ( picture->current_frame )
    picture->current_frame->pts = 0;
  if ( picture->forward_reference_frame )
    picture->forward_reference_frame->pts = 0;
  if ( picture->backward_reference_frame )
    picture->backward_reference_frame->pts = 0;
}

void mpeg2_reset (mpeg2dec_t * mpeg2dec) {
  
  picture_t *picture = mpeg2dec->picture;

  if( !picture )
    return;
  
  mpeg2_discontinuity(mpeg2dec);
  
  if( !picture->mpeg1 ) {
    mpeg2dec->is_wait_for_ip_frames = 2;

    /* mark current frames as bad so they won't make to screen */
    if ( picture->current_frame )
      picture->current_frame->bad_frame=1;
    if (picture->forward_reference_frame )
      picture->forward_reference_frame->bad_frame=1;
    if (picture->backward_reference_frame)
      picture->backward_reference_frame->bad_frame=1;

  } else {
    /* to free reference frames one also needs to fix slice.c to 
     * abort when they are NULL. unfortunately it seems to break
     * DVD menus.
     *
     * ...so let's do this for mpeg-1 only :)
     */
    if ( picture->current_frame && 
	 picture->current_frame != picture->backward_reference_frame &&
	 picture->current_frame != picture->forward_reference_frame )
      picture->current_frame->free (picture->current_frame);
    picture->current_frame = NULL;
    
    if (picture->forward_reference_frame &&
        picture->forward_reference_frame != picture->backward_reference_frame)
      picture->forward_reference_frame->free (picture->forward_reference_frame);
    picture->forward_reference_frame = NULL;
    
    if (picture->backward_reference_frame)
      picture->backward_reference_frame->free (picture->backward_reference_frame);
    picture->backward_reference_frame = NULL;
  }
  
  mpeg2dec->in_slice = 0;
  mpeg2dec->seek_mode = 1;

}

/* flush must never allocate any frame (get_frame/duplicate_frame).
 * it is called from inside video_out loop and frame allocation
 * may cause some (rare) deadlocks.
 */
void mpeg2_flush (mpeg2dec_t * mpeg2dec) {

  picture_t *picture = mpeg2dec->picture;

  if (!picture)
    return;
  
  if (picture->current_frame && !picture->current_frame->drawn &&
      !picture->current_frame->bad_frame) {
    
#ifdef LOG
    printf ("libmpeg2: blasting out current frame %d on flush\n",
	    picture->current_frame->id);
#endif
    
    picture->current_frame->drawn = 1;
    get_frame_duration(mpeg2dec, picture->current_frame);
    
    picture->current_frame->pts = 0;
    picture->current_frame->draw(picture->current_frame, mpeg2dec->stream);
  }

}

void mpeg2_close (mpeg2dec_t * mpeg2dec)
{
    picture_t *picture = mpeg2dec->picture;

    /*
    {
	static uint8_t finalizer[] = {0,0,1,0xb4};
	mpeg2_decode_data (mpeg2dec, finalizer, finalizer+4, 0);
    }
    */

    /* 
      dont remove any picture->*->free() below. doing so will cause buffer 
      leak, and we only have about 15 of them.
    */ 
 
    if ( picture->current_frame ) {
      if( !picture->current_frame->drawn ) {
#ifdef LOG
        printf ("libmpeg2: blasting out current frame on close\n");
#endif
        picture->current_frame->pts = 0;
        get_frame_duration(mpeg2dec, picture->current_frame);
        picture->current_frame->draw (picture->current_frame, mpeg2dec->stream);
        picture->current_frame->drawn = 1;
      }
         
      if( picture->current_frame != picture->backward_reference_frame &&
          picture->current_frame != picture->forward_reference_frame ) {
        picture->current_frame->free (picture->current_frame);
      }
      picture->current_frame = NULL;
    }
    
    if (picture->forward_reference_frame &&
        picture->forward_reference_frame != picture->backward_reference_frame) {
      picture->forward_reference_frame->free (picture->forward_reference_frame);
      picture->forward_reference_frame = NULL;
    }
    
    if (picture->backward_reference_frame) {
      if( !picture->backward_reference_frame->drawn) {
#ifdef LOG
        printf ("libmpeg2: blasting out backward reference frame on close\n");
#endif
        picture->backward_reference_frame->pts = 0;
        get_frame_duration(mpeg2dec, picture->backward_reference_frame);
        picture->backward_reference_frame->draw (picture->backward_reference_frame, mpeg2dec->stream);
        picture->backward_reference_frame->drawn = 1;      
      }
      picture->backward_reference_frame->free (picture->backward_reference_frame);
      picture->backward_reference_frame = NULL;
    }

    if ( mpeg2dec->chunk_buffer ) {
      free (mpeg2dec->chunk_base);
      mpeg2dec->chunk_buffer = NULL;
    }
    
    if ( mpeg2dec->picture ) {
      free (mpeg2dec->picture_base);
      mpeg2dec->picture = NULL;
    }
    
    if ( mpeg2dec->cc_dec) {
      /* dispose the closed caption decoder */
      mpeg2dec->cc_dec->dispose(mpeg2dec->cc_dec);
      mpeg2dec->cc_dec = NULL;
    }
}

void mpeg2_find_sequence_header (mpeg2dec_t * mpeg2dec,
				 uint8_t * current, uint8_t * end){

  uint8_t code;
  picture_t *picture = mpeg2dec->picture;

  mpeg2dec->seek_mode = 1;

  while (current != end) {
    code = mpeg2dec->code;
    current = copy_chunk (mpeg2dec, current, end);
    if (current == NULL)
      return ;

    /* printf ("looking for sequence header... %02x\n", code);  */

    mpeg2_stats (code, mpeg2dec->chunk_buffer);

    if (code == 0xb3) {	/* sequence_header_code */
      if (mpeg2_header_sequence (picture, mpeg2dec->chunk_buffer)) {
	printf ("libmpeg2: bad sequence header\n");
	continue;
      }
      if (mpeg2dec->force_aspect) picture->aspect_ratio_information = mpeg2dec->force_aspect;
	  
      if (mpeg2dec->is_sequence_needed) {
        xine_event_t event;
        xine_format_change_data_t data;

	mpeg2dec->new_sequence = 1;
	
	mpeg2dec->is_sequence_needed = 0;
	picture->frame_width  = picture->coded_picture_width;
	picture->frame_height = picture->coded_picture_height;

	remember_metainfo (mpeg2dec);

	event.type = XINE_EVENT_FRAME_FORMAT_CHANGE;
	event.stream = mpeg2dec->stream;
	event.data = &data;
	event.data_length = sizeof(data);
	data.width = picture->coded_picture_width;
	data.height = picture->coded_picture_height;
	data.aspect = picture->aspect_ratio_information;
	data.pan_scan = mpeg2dec->force_pan_scan;
	xine_event_send(mpeg2dec->stream, &event);

	mpeg2dec->stream->stream_info[XINE_STREAM_INFO_VIDEO_WIDTH]
          = picture->coded_picture_width;
	mpeg2dec->stream->stream_info[XINE_STREAM_INFO_VIDEO_HEIGHT]
          = picture->coded_picture_height;
      }
    } else if (code == 0xb5) {	/* extension_start_code */
      if (mpeg2_header_extension (picture, mpeg2dec->chunk_buffer)) {
	printf ("libmpeg2: bad extension\n");
	continue ;
      }
    }
  }
}

/* Find the end of the userdata field in an MPEG-2 stream */
static uint8_t *find_end(uint8_t *buffer)
{
  uint8_t *current = buffer;
  while(1) {
    if (current[0] == 0 && current[1] == 0 && current[2] == 1)
      break;
    current++;
  }
  return current;
}

static void process_userdata(mpeg2dec_t *mpeg2dec, uint8_t *buffer)
{
  /* check if user data denotes closed captions */
  if (buffer[0] == 'C' && buffer[1] == 'C') {
    
    if (!mpeg2dec->cc_dec) {
      xine_event_t event;
      xine_format_change_data_t data;
      
      /* open the closed caption decoder first */
      mpeg2dec->cc_dec = get_spu_decoder(mpeg2dec->stream, (BUF_SPU_CC >> 16) & 0xff);
      
      /* send a frame format event so that the CC decoder knows the initial image size */
      event.type = XINE_EVENT_FRAME_FORMAT_CHANGE;
      event.stream = mpeg2dec->stream;
      event.data = &data;
      event.data_length = sizeof(data);
      data.width = mpeg2dec->picture->coded_picture_width;
      data.height = mpeg2dec->picture->coded_picture_height;
      data.aspect = mpeg2dec->picture->aspect_ratio_information;
      data.pan_scan = mpeg2dec->force_pan_scan;
      xine_event_send(mpeg2dec->stream, &event);

      mpeg2dec->stream->stream_info[XINE_STREAM_INFO_VIDEO_WIDTH]
        = mpeg2dec->picture->coded_picture_width;
      mpeg2dec->stream->stream_info[XINE_STREAM_INFO_VIDEO_HEIGHT]
        = mpeg2dec->picture->coded_picture_height;
    }
    
    if (mpeg2dec->cc_dec) {
      buf_element_t buf;
      
      buf.type = BUF_SPU_CC;
      buf.content = &buffer[2];
      buf.pts = mpeg2dec->pts;
      buf.size = find_end(buffer) - &buffer[2];
      buf.decoder_flags = 0;
      
      mpeg2dec->cc_dec->decode_data(mpeg2dec->cc_dec, &buf);
    }
  }
}
