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
** $Id: lt_predict.h,v 1.2 2002/12/16 19:00:37 miguelfreitas Exp $
**/

#ifdef LTP_DEC

#ifndef __LT_PREDICT_H__
#define __LT_PREDICT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "filtbank.h"

void lt_prediction(ic_stream *ics,
                   ltp_info *ltp,
                   real_t *spec,
                   real_t *lt_pred_stat,
                   fb_info *fb,
                   uint8_t win_shape,
                   uint8_t win_shape_prev,
                   uint8_t sr_index,
                   uint8_t object_type,
                   uint16_t frame_len);

void lt_update_state(real_t *lt_pred_stat,
                     real_t *time,
                     real_t *overlap,
                     uint16_t frame_len,
                     uint8_t object_type);

#ifdef __cplusplus
}
#endif
#endif

#endif
