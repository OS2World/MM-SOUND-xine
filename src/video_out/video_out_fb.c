/*
 * Copyright (C) 2000-2003 the xine project and Fredrik Noring
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
 * $Id: video_out_fb.c,v 1.30 2003/10/23 15:17:07 mroi Exp $
 * 
 * video_out_fb.c, frame buffer xine driver by Miguel Freitas
 *
 * Contributors:
 *
 *     Fredrik Noring <noring@nocrew.org>:  Zero copy buffers and clean up.
 *
 * based on xine's video_out_xshm.c...
 * ...based on mpeg2dec code from
 * Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * ideas from ppmtofb - Display P?M graphics on framebuffer devices
 *            by Geert Uytterhoeven and Chris Lawrence
 *
 * Note: Use this with fbxine. It may work with the regular xine too,
 * provided the visual type is changed (see below).
 *
 * TODO: VT switching (configurable)
 */

/* #define USE_X11_VISUAL */

#define RECOMMENDED_NUM_BUFFERS  5
#define MAXIMUM_NUM_BUFFERS     25

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h> 
             
#include "xine.h"
#include "video_out.h"

#include <errno.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <pthread.h>
#include <netinet/in.h>

#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>

#include "xine_internal.h"
#include "alphablend.h"
#include "yuv2rgb.h"
#include "xineutils.h"
#include "vo_scale.h"

#define LOG

typedef struct fb_frame_s
{
  vo_frame_t         vo_frame;

  int                format;
  int                flags;
  
  vo_scale_t         sc;

  uint8_t           *chunk[3]; /* mem alloc by xmalloc_aligned           */

  yuv2rgb_t         *yuv2rgb;  /* yuv2rgb converter for this frame */
  uint8_t           *rgb_dst;
  int                yuv_stride;
  int                stripe_height, stripe_inc;
  
  int                bytes_per_line;

  uint8_t*           video_mem;            /* mmapped video memory */
  uint8_t*           data;
  int                yoffset;

  struct fb_driver_s *this;
} fb_frame_t;

typedef struct fb_driver_s
{
  vo_driver_t        vo_driver;

  int                fd;
  int                mem_size;
  uint8_t*           video_mem_base;       /* mmapped video memory */
  
  int                depth, bpp, bytes_per_pixel;
  
  int                total_num_native_buffers;
  int                used_num_buffers;
	
  int                yuv2rgb_mode;
  int                yuv2rgb_swap;
  int                yuv2rgb_gamma;
  uint8_t           *yuv2rgb_cmap;
  yuv2rgb_factory_t *yuv2rgb_factory;

  vo_overlay_t      *overlay;

  /* size / aspect ratio calculations */
  vo_scale_t         sc;
  
  int                fb_bytes_per_line;

  fb_frame_t        *cur_frame, *old_frame;
	
  struct fb_var_screeninfo fb_var;
  struct fb_fix_screeninfo fb_fix;

  int                use_zero_copy;
} fb_driver_t;

typedef struct
{
  video_driver_class_t driver_class;
  config_values_t     *config;
} fb_class_t;

static uint32_t fb_get_capabilities(vo_driver_t *this_gen)
{
  return VO_CAP_YV12 | VO_CAP_YUY2;
}

static void fb_frame_proc_slice(vo_frame_t *vo_img, uint8_t **src)
{
  fb_frame_t *frame = (fb_frame_t *)vo_img ;
  
  vo_img->proc_called = 1;
  if(frame->format == XINE_IMGFMT_YV12)
    frame->yuv2rgb->yuv2rgb_fun(frame->yuv2rgb, frame->rgb_dst,
				 src[0], src[1], src[2]);
  else
    frame->yuv2rgb->yuy22rgb_fun(frame->yuv2rgb,
				 frame->rgb_dst, src[0]);
  frame->rgb_dst += frame->stripe_inc; 
}

static void fb_frame_field(vo_frame_t *vo_img, int which_field)
{
  fb_frame_t *frame = (fb_frame_t *)vo_img ;
  
  switch(which_field)
  {
  case VO_TOP_FIELD:
      frame->rgb_dst    = frame->data;
      frame->stripe_inc = 2*frame->stripe_height *
			  frame->bytes_per_line;
    break;
			
  case VO_BOTTOM_FIELD:
      frame->rgb_dst    = frame->data +
			  frame->bytes_per_line ;
      frame->stripe_inc = 2*frame->stripe_height *
			  frame->bytes_per_line;
    break;
			
  case VO_BOTH_FIELDS:
      frame->rgb_dst    = frame->data;
    break;
  }
}

static void fb_frame_dispose(vo_frame_t *vo_img)
{
  fb_frame_t *frame = (fb_frame_t *)vo_img;

  if(!frame->this->use_zero_copy)
     free(frame->data);
  free(frame);
}

static vo_frame_t *fb_alloc_frame(vo_driver_t *this_gen)
{
  fb_driver_t *this = (fb_driver_t *)this_gen;
  fb_frame_t *frame;

  if(this->use_zero_copy &&
     this->total_num_native_buffers <= this->used_num_buffers)
    return 0;

  frame = (fb_frame_t *)malloc(sizeof(fb_frame_t));
  if(!frame)
  {
    fprintf(stderr, "fb_alloc_frame: Out of memory.\n");
    return 0;
  }

  memset(frame, 0, sizeof(fb_frame_t));
  memcpy(&frame->sc, &this->sc, sizeof(vo_scale_t));

  pthread_mutex_init(&frame->vo_frame.mutex, NULL);
  
  /* supply required functions */
  frame->vo_frame.proc_slice = fb_frame_proc_slice;
  frame->vo_frame.proc_frame = NULL;
  frame->vo_frame.field      = fb_frame_field; 
  frame->vo_frame.dispose    = fb_frame_dispose;
  frame->vo_frame.driver     = this_gen;
  
  frame->this = this;
  
  /* colorspace converter for this frame */
  frame->yuv2rgb =
    this->yuv2rgb_factory->create_converter(this->yuv2rgb_factory);

  if(this->use_zero_copy)
  {
    frame->yoffset = this->used_num_buffers * this->fb_var.yres;
    frame->video_mem = this->video_mem_base +
		       this->used_num_buffers * this->fb_var.yres *
		       this->fb_bytes_per_line;

    memset(frame->video_mem, 0,
	   this->fb_var.yres * this->fb_bytes_per_line);
  }
  else
    frame->video_mem = this->video_mem_base;

  this->used_num_buffers++;

  return (vo_frame_t *)frame;
}

static void fb_compute_ideal_size(fb_driver_t *this, fb_frame_t *frame)
{
  vo_scale_compute_ideal_size(&frame->sc);
}

static void fb_compute_rgb_size(fb_driver_t *this, fb_frame_t *frame)
{
  vo_scale_compute_output_size(&frame->sc);
  
  /* avoid problems in yuv2rgb */
  if(frame->sc.output_height < (frame->sc.delivered_height+15) >> 4)
    frame->sc.output_height = (frame->sc.delivered_height+15) >> 4;

  if (frame->sc.output_width < 8)
    frame->sc.output_width = 8;

  /* yuv2rgb_mlib needs an even YUV2 width */
  if (frame->sc.output_width & 1) 
    frame->sc.output_width++;

#ifdef LOG
  printf("video_out_fb: frame source %d x %d => screen output %d x %d%s\n",
	 frame->sc.delivered_width, frame->sc.delivered_height,
	 frame->sc.output_width, frame->sc.output_height,
	 (frame->sc.delivered_width != frame->sc.output_width ||
	  frame->sc.delivered_height != frame->sc.output_height ?
	  ", software scaling" : ""));
#endif
}

static void setup_colorspace_converter(fb_frame_t *frame, int flags)
{
  switch(flags)
  {
    case VO_TOP_FIELD:
    case VO_BOTTOM_FIELD:
      frame->yuv2rgb->
	configure(frame->yuv2rgb,
		  frame->sc.delivered_width,
		  16,
		  2 * frame->vo_frame.pitches[0],
		  2 * frame->vo_frame.pitches[1],
		  frame->sc.output_width,
		  frame->stripe_height,
		  frame->bytes_per_line * 2);
      frame->yuv_stride = frame->bytes_per_line * 2;
      break;
    
    case VO_BOTH_FIELDS:
      frame->yuv2rgb->
	configure(frame->yuv2rgb,
		  frame->sc.delivered_width,
		  16,
		  frame->vo_frame.pitches[0],
		  frame->vo_frame.pitches[1],
		  frame->sc.output_width,
		  frame->stripe_height,
		  frame->bytes_per_line);
      frame->yuv_stride = frame->bytes_per_line;
      break;
  }
}

static void reset_dest_pointers(fb_frame_t *frame, int flags)
{
  switch(flags)
  {
    case VO_TOP_FIELD:
      frame->rgb_dst = frame->data;
      frame->stripe_inc = 2 * frame->stripe_height *
			  frame->bytes_per_line;
      break;

    case VO_BOTTOM_FIELD:
      frame->rgb_dst = frame->data +
		       frame->bytes_per_line ;
      frame->stripe_inc = 2 * frame->stripe_height *
			  frame->bytes_per_line;
      break;

    case VO_BOTH_FIELDS:
      frame->rgb_dst = frame->data;
      frame->stripe_inc = frame->stripe_height *
			  frame->bytes_per_line;
      break;
  }
}

static void frame_reallocate(fb_driver_t *this, fb_frame_t *frame,
			     uint32_t width, uint32_t height, int format)
{
  if(frame->chunk[0])
  {
    free(frame->chunk[0]);
        frame->chunk[0] = NULL;
  }
  if(frame->chunk[1])
  {
    free(frame->chunk[1]);
        frame->chunk[1] = NULL;
  }
  if(frame->chunk[2])
  {
    free(frame->chunk[2]);
        frame->chunk[2] = NULL;
  }
      
  if(this->use_zero_copy)
  {
    frame->data = frame->video_mem +
		  frame->sc.output_yoffset*this->fb_bytes_per_line+
		  frame->sc.output_xoffset*this->bytes_per_pixel;
  }
  else
  {
    if(frame->data)
      free(frame->data);
    frame->data = xine_xmalloc(frame->sc.output_width *
			       frame->sc.output_height *
			       this->bytes_per_pixel);
  }

  if(format == XINE_IMGFMT_YV12)
  {
      frame->vo_frame.pitches[0] = 8*((width + 7) / 8);
      frame->vo_frame.pitches[1] = 8*((width + 15) / 16);
      frame->vo_frame.pitches[2] = 8*((width + 15) / 16);
		
    frame->vo_frame.base[0] =
      xine_xmalloc_aligned(16,
			   frame->vo_frame.pitches[0] *
			   height,
                                                      (void **)&frame->chunk[0]);
		
    frame->vo_frame.base[1] =
      xine_xmalloc_aligned(16,
			   frame->vo_frame.pitches[1] *
			   ((height+1)/2),
                                                      (void **)&frame->chunk[1]);
		
    frame->vo_frame.base[2] =
      xine_xmalloc_aligned(16,
			   frame->vo_frame.pitches[2] *
			   ((height+1)/2),
                                                      (void **)&frame->chunk[2]);
  }
  else
  {
    frame->vo_frame.pitches[0] = 8 * ((width + 3) / 4);
		
    frame->vo_frame.base[0] =
      xine_xmalloc_aligned(16,
			   frame->vo_frame.pitches[0] *
			   height,
                                                      (void **)&frame->chunk[0]);
      frame->chunk[1] = NULL;
      frame->chunk[2] = NULL;
  }
}
    
static void fb_update_frame_format(vo_driver_t *this_gen,
				   vo_frame_t *frame_gen,
				   uint32_t width, uint32_t height,
				   double ratio, int format, int flags)
{
  fb_driver_t *this = (fb_driver_t *)this_gen;
  fb_frame_t *frame = (fb_frame_t *)frame_gen;
  
  flags &= VO_BOTH_FIELDS;

  /* Find out if we need to adapt this frame. */
  if (width != frame->sc.delivered_width    ||
      height != frame->sc.delivered_height  ||
      ratio != frame->sc.delivered_ratio    ||
      flags != frame->flags                 ||
      format != frame->format               ||
      this->sc.user_ratio != frame->sc.user_ratio)
  {
#ifdef LOG
    printf("video_out_fb: frame format (from decoder) "
	   "has changed => adapt\n");
#endif

    frame->sc.delivered_width  = width;
    frame->sc.delivered_height = height;
    frame->sc.delivered_ratio  = ratio;
    frame->flags               = flags;
    frame->format              = format;
    frame->sc.user_ratio       = this->sc.user_ratio;

    fb_compute_ideal_size(this, frame);
    fb_compute_rgb_size(this, frame);

    frame_reallocate(this, frame, width, height, format);

    frame->stripe_height = 16 * frame->sc.output_height /
			   frame->sc.delivered_height;
    if(this->use_zero_copy)
      frame->bytes_per_line = this->fb_bytes_per_line;
    else
      frame->bytes_per_line = frame->sc.output_width *
			      this->bytes_per_pixel;
    
    setup_colorspace_converter(frame, flags);
  }

  reset_dest_pointers(frame, flags);
}

static void fb_overlay_clut_yuv2rgb(fb_driver_t *this,
				    vo_overlay_t *overlay, fb_frame_t *frame)
{
  int i;
  clut_t* clut = (clut_t*)overlay->color;
	
  if(!overlay->rgb_clut)
  {
    for(i = 0;
	i < sizeof(overlay->color)/sizeof(overlay->color[0]);
	i++)
    {
      *((uint32_t *)&clut[i]) =
	frame->yuv2rgb->
	yuv2rgb_single_pixel_fun(frame->yuv2rgb,
				 clut[i].y,
				 clut[i].cb,
				 clut[i].cr);
    }
    overlay->rgb_clut++;
  }
	
  if(!overlay->clip_rgb_clut)
  {
    clut = (clut_t*) overlay->clip_color;
		
    for(i = 0;
	i < sizeof(overlay->color)/sizeof(overlay->color[0]);
	i++)
    {
      *((uint32_t *)&clut[i]) =
	frame->yuv2rgb->
	yuv2rgb_single_pixel_fun(frame->yuv2rgb,
				 clut[i].y,
				 clut[i].cb,
				 clut[i].cr);
    }
    overlay->clip_rgb_clut++;
  }
}

static void fb_overlay_blend (vo_driver_t *this_gen, vo_frame_t *frame_gen,
			      vo_overlay_t *overlay)
{
  fb_driver_t *this = (fb_driver_t *)this_gen;
  fb_frame_t *frame = (fb_frame_t *)frame_gen;

  /* Alpha Blend here */
  if(overlay->rle)
  {
    if(!overlay->rgb_clut || !overlay->clip_rgb_clut)
       fb_overlay_clut_yuv2rgb(this,overlay,frame);

    switch(this->bpp)
    {
       case 16:
	blend_rgb16(frame->data,
		    overlay,
		    frame->sc.output_width,
		    frame->sc.output_height,
		    frame->sc.delivered_width,
		    frame->sc.delivered_height);
        break;
				
       case 24:
	blend_rgb24(frame->data,
		    overlay,
		    frame->sc.output_width,
		    frame->sc.output_height,
		    frame->sc.delivered_width,
		    frame->sc.delivered_height);
        break;
				
       case 32:
	blend_rgb32(frame->data,
		    overlay,
		    frame->sc.output_width,
		    frame->sc.output_height,
		    frame->sc.delivered_width,
		    frame->sc.delivered_height);
	break;
     }        
   }
}

static int fb_redraw_needed(vo_driver_t *this_gen)
{
  return 0;
}

static void fb_display_frame(vo_driver_t *this_gen, vo_frame_t *frame_gen)
{
  fb_driver_t  *this = (fb_driver_t *) this_gen;
  fb_frame_t   *frame = (fb_frame_t *) frame_gen;
  uint8_t	*dst, *src;
  int y;

  if(frame->sc.output_width  != this->sc.output_width ||
     frame->sc.output_height != this->sc.output_height)
  {
    this->sc.output_width    = frame->sc.output_width;
    this->sc.output_height   = frame->sc.output_height;

    printf("video_out_fb: gui size %d x %d, frame size %d x %d\n",
            this->sc.gui_width, this->sc.gui_height,
            frame->sc.output_width, frame->sc.output_height);
    
    memset(this->video_mem_base, 0, this->mem_size);
  }

  if (this->sc.frame_output_cb) {
    this->sc.delivered_height   = frame->sc.delivered_height;
    this->sc.delivered_width    = frame->sc.delivered_width;
    vo_scale_redraw_needed( &this->sc );
  }

  if(this->use_zero_copy)
  {
    if(this->old_frame)
      this->old_frame->vo_frame.free
	(&this->old_frame->vo_frame);
    this->old_frame = this->cur_frame;
    this->cur_frame = frame;
		
    this->fb_var.yoffset = frame->yoffset;
    if(ioctl(this->fd, FBIOPAN_DISPLAY, &this->fb_var) == -1)
      perror("video_out_fb: ioctl FBIOPAN_DISPLAY failed");
  }
  else
  {
    dst = frame->video_mem +
	  frame->sc.output_yoffset * this->fb_bytes_per_line +
        frame->sc.output_xoffset * this->bytes_per_pixel;
    src = frame->data;
   
    for(y = 0; y < frame->sc.output_height; y++)
    {
      xine_fast_memcpy(dst, src, frame->bytes_per_line);
      src += frame->bytes_per_line;
      dst += this->fb_bytes_per_line;
    } 
  
    frame->vo_frame.free(&frame->vo_frame);
  }
}

static int fb_get_property(vo_driver_t *this_gen, int property)
{
  fb_driver_t *this = (fb_driver_t *)this_gen;

  switch(property)
  {
    case VO_PROP_ASPECT_RATIO:
      return this->sc.user_ratio;

    case VO_PROP_BRIGHTNESS:
    return this->yuv2rgb_gamma;

    default:
      printf("video_out_fb: tried to get unsupported "
	     "property %d\n", property);
  }

  return 0;
}

static int fb_set_property(vo_driver_t *this_gen, int property, int value)
{
  fb_driver_t *this = (fb_driver_t *)this_gen;

  switch(property)
  {
    case VO_PROP_ASPECT_RATIO:
      if(value>=XINE_VO_ASPECT_NUM_RATIOS)
      value = XINE_VO_ASPECT_AUTO;
    this->sc.user_ratio = value;
    printf("video_out_fb: aspect ratio changed to %s\n",
           vo_scale_aspect_ratio_name(value));
      break;

    case VO_PROP_BRIGHTNESS:
    this->yuv2rgb_gamma = value;
      this->yuv2rgb_factory->
	set_csc_levels(this->yuv2rgb_factory, value, 128, 128);
    printf("video_out_fb: gamma changed to %d\n",value);
      break;

    default:
      printf("video_out_fb: tried to set unsupported "
	     "property %d\n", property);
  }

  return value;
}

static void fb_get_property_min_max(vo_driver_t *this_gen,
				    int property, int *min, int *max)
{
  /* fb_driver_t *this = (fb_driver_t *) this_gen;  */
	
  if(property == VO_PROP_BRIGHTNESS)
  {
    *min = -100;
    *max = +100;
  }
  else
  {
    *min = 0;
    *max = 0;
  }
}

static int fb_gui_data_exchange(vo_driver_t *this_gen,
				int data_type, void *data)
{
  return 0;
}

static void fb_dispose(vo_driver_t *this_gen)
{
  fb_driver_t *this = (fb_driver_t *)this_gen;
  
  munmap(0, this->mem_size);   
  close(this->fd);
}

static int get_fb_var_screeninfo(int fd, struct fb_var_screeninfo *var)
{
  int i;
  
  if(ioctl(fd, FBIOGET_VSCREENINFO, var))
  {
    perror("video_out_fb: ioctl FBIOGET_VSCREENINFO");
    return 0;
  }

  var->xres_virtual = var->xres;
  var->xoffset      = 0;
  var->yoffset      = 0;
  var->nonstd       = 0;
  var->vmode       &= ~FB_VMODE_YWRAP;

  /* Maximize virtual yres to fit as many buffers as possible. */
  for(i = MAXIMUM_NUM_BUFFERS; i > 0; i--)
  {
    var->yres_virtual = i * var->yres;
    if(ioctl(fd, FBIOPUT_VSCREENINFO, var) == -1)
      continue;
    break;
  }
    
  /* Get proper value for maximized var->yres_virtual. */
  if(ioctl(fd, FBIOGET_VSCREENINFO, var) == -1)
  {
    perror("video_out_fb: ioctl FBIOGET_VSCREENINFO");
    return 0;
  }

  return 1;
}

static int get_fb_fix_screeninfo(int fd, struct fb_fix_screeninfo *fix)
{
  if(ioctl(fd, FBIOGET_FSCREENINFO, fix))
  {
    perror("video_out_fb: ioctl FBIOGET_FSCREENINFO");
    return 0;
  }

  if((fix->visual != FB_VISUAL_TRUECOLOR &&
      fix->visual != FB_VISUAL_DIRECTCOLOR) ||
     fix->type != FB_TYPE_PACKED_PIXELS)
  {
    fprintf(stderr, "video_out_fb: only packed truecolor/directcolor is supported (%d).\n"
	    "     Check 'fbset -i' or try 'fbset -depth 16'.\n",
	    fix->visual);
    return 0;
  }

  return 1;
}

static void register_callbacks(fb_driver_t *this)
{
  this->vo_driver.get_capabilities     = fb_get_capabilities;
  this->vo_driver.alloc_frame          = fb_alloc_frame;
  this->vo_driver.update_frame_format  = fb_update_frame_format;
  this->vo_driver.overlay_begin        = 0; /* not used */
  this->vo_driver.overlay_blend        = fb_overlay_blend;
  this->vo_driver.overlay_end          = 0; /* not used */
  this->vo_driver.display_frame        = fb_display_frame;
  this->vo_driver.get_property         = fb_get_property;
  this->vo_driver.set_property         = fb_set_property;
  this->vo_driver.get_property_min_max = fb_get_property_min_max;
  this->vo_driver.gui_data_exchange    = fb_gui_data_exchange;
  this->vo_driver.dispose              = fb_dispose;
  this->vo_driver.redraw_needed        = fb_redraw_needed;
}

static int open_fb_device(config_values_t *config)
{
  static char devkey[] = "video.fb_device";   /* Why static? */
  char *device_name;
  int fd;

  device_name = config->register_string(config, devkey, "",
					_("framebuffer device"),
					NULL, 10, NULL, NULL);
  if(strlen(device_name) > 3)
  {
    fd = open(device_name, O_RDWR);
  }
  else
  {
    device_name = "/dev/fb1";
    fd = open(device_name, O_RDWR);
    
    if(fd < 0)
    {  
      device_name = "/dev/fb0";
      fd = open(device_name, O_RDWR);
    }
  }
  
  if(fd < 0)
  {
    fprintf(stderr, "video_out_fb: Unable to open device \"%s\", aborting: %s\n",
	    device_name, strerror(errno));
    return -1;
  }
  
  config->update_string(config, devkey, device_name);

  return fd;
}
  
static int mode_visual(fb_driver_t *this, config_values_t *config,
		       struct fb_var_screeninfo *var,
		       struct fb_fix_screeninfo *fix)
{
  switch(fix->visual)
  {
    case FB_VISUAL_TRUECOLOR:
    case FB_VISUAL_DIRECTCOLOR:
      switch(this->depth)
      {
	case 24:
	  if(this->bpp == 32)
	  {
	    if(!var->blue.offset)
	      return MODE_32_RGB;
	    return MODE_32_BGR;
	  }
	  if(!var->blue.offset)
	    return MODE_24_RGB;
	  return MODE_24_BGR;

	case 16:
	  if(!var->blue.offset)
	    return MODE_16_RGB;
	  return MODE_16_BGR;
  
	case 15:
	  if(!var->blue.offset)
	    return MODE_15_RGB;
	  return MODE_15_BGR;

	case 8:
	  if(!var->blue.offset)
	    return MODE_8_RGB; 
	  return MODE_8_BGR; 

      }
  }
  
  fprintf(stderr, "video_out_fb: Your video mode was not recognized, sorry.\n");
  return 0;
}
    
static int setup_yuv2rgb(fb_driver_t *this, config_values_t *config,
			 struct fb_var_screeninfo *var,
			 struct fb_fix_screeninfo *fix)
{
  this->yuv2rgb_mode = mode_visual(this, config, var, fix);
  if(!this->yuv2rgb_mode)
    return 0;

  this->yuv2rgb_swap  = 0;
  this->yuv2rgb_gamma =
    config->register_range(config, "video.fb_gamma", 0,
			   -100, 100, 
			   "gamma correction for fb driver",
			   NULL, 0, NULL, NULL);

  this->yuv2rgb_factory = yuv2rgb_factory_init(this->yuv2rgb_mode,
					       this->yuv2rgb_swap, 
					       this->yuv2rgb_cmap);
  this->yuv2rgb_factory->set_csc_levels(this->yuv2rgb_factory,
					this->yuv2rgb_gamma, 128, 128);
  
  return 1;
}

static void setup_buffers(fb_driver_t *this,
			  struct fb_var_screeninfo *var)
{
  /*
   * depth in X11 terminology land is the number of bits used to
   * actually represent the colour.
   *
   * bpp in X11 land means how many bits in the frame buffer per
   * pixel.
   *
   * ex. 15 bit color is 15 bit depth and 16 bpp. Also 24 bit
   *     color is 24 bit depth, but can be 24 bpp or 32 bpp.
   *
   * fb assumptions: bpp % 8   = 0 (e.g. 8, 16, 24, 32 bpp)
   *                 bpp      <= 32
   *                 msb_right = 0
   */
   
  this->bytes_per_pixel = (this->fb_var.bits_per_pixel + 7)/8;
  this->bpp = this->bytes_per_pixel * 8;
  this->depth = this->fb_var.red.length +
		this->fb_var.green.length +
		this->fb_var.blue.length;

  this->total_num_native_buffers = var->yres_virtual / var->yres;
  this->used_num_buffers = 0;
	
  this->cur_frame = this->old_frame = 0;
	
  printf("video_out_fb: %d video RAM buffers are available.\n",
	 this->total_num_native_buffers);

  if(this->total_num_native_buffers < RECOMMENDED_NUM_BUFFERS)
  {
    this->use_zero_copy = 0;
    printf("WARNING: video_out_fb: Zero copy buffers are DISABLED because only %d buffers\n"
	   "     are available which is less than the recommended %d buffers. Lowering\n"
	   "     the frame buffer resolution might help.\n",
	   this->total_num_native_buffers,
	   RECOMMENDED_NUM_BUFFERS);
  }
  else
  {
    /* test if FBIOPAN_DISPLAY works */
    this->fb_var.yoffset = this->fb_var.yres;
    if(ioctl(this->fd, FBIOPAN_DISPLAY, &this->fb_var) == -1) {
      printf("WARNING: video_out_fb: Zero copy buffers are DISABLED because kernel driver\n"
	     "     do not support screen panning (used for frame flips).\n");
    } else {
      this->fb_var.yoffset = 0;
      ioctl(this->fd, FBIOPAN_DISPLAY, &this->fb_var);

      this->use_zero_copy = 1;
      printf("video_out_fb: Using zero copy buffers.\n");
    }
  }
}

static vo_driver_t *fb_open_plugin(video_driver_class_t *class_gen,
				   const void *visual_gen)
{
  config_values_t *config;
  fb_driver_t *this;
  fb_class_t *class;
  fb_visual_t *visual = NULL;
  
  if (visual_gen) {
    visual = (fb_visual_t *) visual_gen;
  }
          
  class = (fb_class_t *)class_gen;
  config = class->config;

  /* allocate plugin struct */
  this = malloc(sizeof(fb_driver_t));
  if(!this)
  {
    fprintf(stderr, "video_out_fb: malloc failed\n");
    return 0;
  }
  memset(this, 0, sizeof(fb_driver_t));
  
  register_callbacks(this);

  this->fd = open_fb_device(config);
  if(this->fd == -1)
    goto error;
  if(!get_fb_var_screeninfo(this->fd, &this->fb_var))
    goto error;
  if(!get_fb_fix_screeninfo(this->fd, &this->fb_fix))
    goto error;
   
  if(this->fb_fix.line_length)
    this->fb_bytes_per_line = this->fb_fix.line_length;
  else
    this->fb_bytes_per_line =
      (this->fb_var.xres_virtual *
       this->fb_var.bits_per_pixel)/8;
    
  vo_scale_init(&this->sc, 0, 0, config);
  this->sc.gui_width  = this->fb_var.xres;
  this->sc.gui_height = this->fb_var.yres;
  this->sc.user_ratio = XINE_VO_ASPECT_AUTO;

  if (visual) {
    this->sc.frame_output_cb = visual->frame_output_cb;
    this->sc.user_data       = visual->user_data;
  }

  this->sc.scaling_disabled =
    config->register_bool(config, "video.disable_scaling", 0,
			  _("disable all video scaling (faster!)"),
			  NULL, 10, NULL, NULL);
  
  setup_buffers(this, &this->fb_var);

  if(this->depth > 16)
    printf("WARNING: video_out_fb: current display depth is %d. For better performance\n"
	   "     a depth of 16 bpp is recommended!\n\n",
	   this->depth);

  printf("video_out_fb: video mode depth is %d (%d bpp),\n"
	 "     red: %d/%d, green: %d/%d, blue: %d/%d\n",
	 this->depth, this->bpp,
	 this->fb_var.red.length, this->fb_var.red.offset,
	 this->fb_var.green.length, this->fb_var.green.offset,
	 this->fb_var.blue.length, this->fb_var.blue.offset);
                                  
  if(!setup_yuv2rgb(this, config, &this->fb_var, &this->fb_fix))
    goto error;
	
  /* mmap whole video memory */
  this->mem_size = this->fb_fix.smem_len;
  this->video_mem_base = mmap(0, this->mem_size, PROT_READ | PROT_WRITE,
			      MAP_SHARED, this->fd, 0);
  return &this->vo_driver;
error:
  free(this);
  return 0;
}

static char* fb_get_identifier(video_driver_class_t *this_gen)
{
  return "fb";
}

static char* fb_get_description(video_driver_class_t *this_gen)
{
  return _("Xine video output plugin using the Linux frame buffer device");
}

static void fb_dispose_class(video_driver_class_t *this_gen)
{
  fb_class_t *this = (fb_class_t *)this_gen;
  free(this);
}

static void *fb_init_class(xine_t *xine, void *visual_gen)
{
  fb_class_t *this = (fb_class_t *)malloc(sizeof(fb_class_t));

  this->driver_class.open_plugin     = fb_open_plugin;
  this->driver_class.get_identifier  = fb_get_identifier;
  this->driver_class.get_description = fb_get_description;
  this->driver_class.dispose         = fb_dispose_class;

  this->config          = xine->config;

  return this;
}

static vo_info_t vo_info_fb =
{
  1,                    /* priority    */
#ifdef USE_X11_VISUAL
  XINE_VISUAL_TYPE_X11  /* visual type */
#else
  XINE_VISUAL_TYPE_FB   /* visual type */
#endif
};

/* exported plugin catalog entry */
plugin_info_t xine_plugin_info[] =
{
  /* type, API, "name", version, special_info, init_function */  
  {
    PLUGIN_VIDEO_OUT,
    18,
    "fb",
    XINE_VERSION_CODE,
    &vo_info_fb, fb_init_class
  },
  {
    PLUGIN_NONE,
    0,
    "",
    0,
    NULL,
    NULL
  }
};
