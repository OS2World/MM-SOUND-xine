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
 * $Id: resample.h,v 1.3 2002/10/23 17:12:34 guenter Exp $
 *
 * utilitiy functions for audio drivers
 *
 * FIXME: not all of them are implemented yet
 */

#ifndef HAVE_RESAMPLE_H
#define HAVE_RESAMPLE_H

void audio_out_resample_stereo(int16_t* input_samples, uint32_t in_samples, 
			       int16_t* output_samples, uint32_t out_samples);

void audio_out_resample_mono(int16_t* input_samples, uint32_t in_samples, 
			     int16_t* output_samples, uint32_t out_samples);

void audio_out_resample_4channel(int16_t* input_samples, uint32_t in_samples, 
				 int16_t* output_samples, uint32_t out_samples);

void audio_out_resample_5channel(int16_t* input_samples, uint32_t in_samples, 
				 int16_t* output_samples, uint32_t out_samples);

void audio_out_resample_6channel(int16_t* input_samples, uint32_t in_samples, 
				 int16_t* output_samples, uint32_t out_samples);

void audio_out_resample_8to16(int8_t* input_samples, 
                              int16_t* output_samples, uint32_t samples);

void audio_out_resample_16to8(int16_t* input_samples, 
                              int8_t*  output_samples, uint32_t samples);

void audio_out_resample_monotostereo(int16_t* input_samples, 
                                     int16_t* output_samples, uint32_t frames);

void audio_out_resample_stereotomono(int16_t* input_samples, 
                                     int16_t* output_samples, uint32_t frames);
                            
#endif
