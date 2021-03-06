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
 * $Id: scratch.h,v 1.5 2002/10/23 17:12:34 guenter Exp $
 *
 * scratch buffer for log output
 *
 */

#ifndef HAVE_SCRATCH_H
#define HAVE_SCRATCH_H

#include <stdarg.h>

typedef struct scratch_buffer_s scratch_buffer_t;

struct scratch_buffer_s {

  void         (*scratch_printf) (scratch_buffer_t *this, const char *format, va_list ap);

  const char **(*get_content) (scratch_buffer_t *this);

  void         (*dispose) (scratch_buffer_t *this);

  char         **lines;
  const char   **ordered;

  int            num_lines;
  int            cur;

};

scratch_buffer_t *new_scratch_buffer (int num_lines);

#endif
