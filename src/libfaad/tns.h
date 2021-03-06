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
** $Id: tns.h,v 1.3 2003/04/12 14:58:47 miguelfreitas Exp $
**/

#ifndef __TNS_H__
#define __TNS_H__

#ifdef __cplusplus
extern "C" {
#endif


#define TNS_MAX_ORDER 20

    
void tns_decode_frame(ic_stream *ics, tns_info *tns, uint8_t sr_index,
                      uint8_t object_type, real_t *spec, uint16_t frame_len);
void tns_encode_frame(ic_stream *ics, tns_info *tns, uint8_t sr_index,
                      uint8_t object_type, real_t *spec, uint16_t frame_len);

static void tns_decode_coef(uint8_t order, uint8_t coef_res_bits, uint8_t coef_compress,
                            uint8_t *coef, real_t *a);
static void tns_ar_filter(real_t *spectrum, uint16_t size, int8_t inc, real_t *lpc,
                          uint8_t order);
static void tns_ma_filter(real_t *spectrum, uint16_t size, int8_t inc, real_t *lpc,
                          uint8_t order);


#ifdef __cplusplus
}
#endif
#endif
