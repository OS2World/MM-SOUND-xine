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
** $Id: hcb_1.c,v 1.2 2002/12/16 18:58:54 miguelfreitas Exp $
**/

#include "../common.h"
#include "hcb.h"

/* 2-step huffman table HCB_1 */


/* 1st step: 5 bits
 *           2^5 = 32 entries
 *
 * Used to find offset into 2nd step table and number of extra bits to get
 */
extern hcb hcb1_1[] = {
    { /* 00000 */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /*       */ 0, 0 },
    { /* 10000 */ 1, 0 },
    { /* 10001 */ 2, 0 },
    { /* 10010 */ 3, 0 },
    { /* 10011 */ 4, 0 },
    { /* 10100 */ 5, 0 },
    { /* 10101 */ 6, 0 },
    { /* 10110 */ 7, 0 },
    { /* 10111 */ 8, 0 },

    /* 7 bit codewords */
    { /* 11000 */ 9,  2 },
    { /* 11001 */ 13, 2 },
    { /* 11010 */ 17, 2 },
    { /* 11011 */ 21, 2 },
    { /* 11100 */ 25, 2 },
    { /* 11101 */ 29, 2 },

    /* 9 bit codewords */
    { /* 11110 */ 33, 4 },

    /* 9/10/11 bit codewords */
    { /* 11111 */ 49, 6 }
};

/* 2nd step table
 *
 * Gives size of codeword and actual data (x,y,v,w)
 */
extern hcb_2_quad hcb1_2[] = {
    /* 1 bit codeword */
    { 1,  0,  0,  0,  0 },

    /* 5 bit codewords */
    { 5,  1,  0,  0,  0 },
    { 5, -1,  0,  0,  0 },
    { 5,  0,  0,  0, -1 },
    { 5,  0,  1,  0,  0 },
    { 5,  0,  0,  0,  1 },
    { 5,  0,  0, -1,  0 },
    { 5,  0,  0,  1,  0 },
    { 5,  0, -1,  0,  0 },

    /* 7 bit codewords */
    /* first 5 bits: 11000 */
    { 7,  1, -1,  0,  0 },
    { 7, -1,  1,  0,  0 },
    { 7,  0,  0, -1,  1 },
    { 7,  0,  1, -1,  0 },
    /* first 5 bits: 11001 */
    { 7,  0, -1,  1,  0 },
    { 7,  0,  0,  1, -1 },
    { 7,  1,  1,  0,  0 },
    { 7,  0,  0, -1, -1 },
    /* first 5 bits: 11010 */
    { 7, -1, -1,  0,  0 },
    { 7,  0, -1, -1,  0 },
    { 7,  1,  0, -1,  0 },
    { 7,  0,  1,  0, -1 },
    /* first 5 bits: 11011 */
    { 7, -1,  0,  1,  0 },
    { 7,  0,  0,  1,  1 },
    { 7,  1,  0,  1,  0 },
    { 7,  0, -1,  0,  1 },
    /* first 5 bits: 11100 */
    { 7,  0,  1,  1,  0 },
    { 7,  0,  1,  0,  1 },
    { 7, -1,  0, -1,  0 },
    { 7,  1,  0,  0,  1 },
    /* first 5 bits: 11101 */
    { 7, -1,  0,  0, -1 },
    { 7,  1,  0,  0, -1 },
    { 7, -1,  0,  0,  1 },
    { 7,  0, -1,  0, -1 },

    /* 9 bit codeword */
    /* first 5 bits: 11110 */
    { 9,  1,  1, -1,  0 },
    { 9, -1,  1, -1,  0 },
    { 9,  1, -1,  1,  0 },
    { 9,  0,  1,  1, -1 },
    { 9,  0,  1, -1,  1 },
    { 9,  0, -1,  1,  1 },
    { 9,  0, -1,  1, -1 },
    { 9,  1, -1, -1,  0 },
    { 9,  1,  0, -1,  1 },
    { 9,  0,  1, -1, -1 },
    { 9, -1,  1,  1,  0 },
    { 9, -1,  0,  1, -1 },
    { 9, -1, -1,  1,  0 },
    { 9,  0, -1, -1,  1 },
    { 9,  1, -1,  0,  1 },
    { 9,  1, -1,  0, -1 },

    /* 9/10/11 bit codewords */
    /* first 5 bits: 11111 */
    /* 9 bit: reading 11 bits -> 2 too much so 4 entries for each codeword */
    { 9, -1,  1,  0, -1 }, { 9, -1,  1,  0, -1 }, { 9, -1,  1,  0, -1 }, { 9, -1,  1,  0, -1 },
    { 9, -1, -1, -1,  0 }, { 9, -1, -1, -1,  0 }, { 9, -1, -1, -1,  0 }, { 9, -1, -1, -1,  0 },
    { 9,  0, -1, -1, -1 }, { 9,  0, -1, -1, -1 }, { 9,  0, -1, -1, -1 }, { 9,  0, -1, -1, -1 },
    { 9,  0,  1,  1,  1 }, { 9,  0,  1,  1,  1 }, { 9,  0,  1,  1,  1 }, { 9,  0,  1,  1,  1 },
    { 9,  1,  0,  1, -1 }, { 9,  1,  0,  1, -1 }, { 9,  1,  0,  1, -1 }, { 9,  1,  0,  1, -1 },
    { 9,  1,  1,  0,  1 }, { 9,  1,  1,  0,  1 }, { 9,  1,  1,  0,  1 }, { 9,  1,  1,  0,  1 },
    { 9, -1,  1,  0,  1 }, { 9, -1,  1,  0,  1 }, { 9, -1,  1,  0,  1 }, { 9, -1,  1,  0,  1 },
    { 9,  1,  1,  1,  0 }, { 9,  1,  1,  1,  0 }, { 9,  1,  1,  1,  0 }, { 9,  1,  1,  1,  0 },
    /* 10 bit: reading 11 bits -> 1 too much so 2 entries for each codeword */
    { 10, -1, -1,  0,  1 }, { 10, -1, -1,  0,  1 },
    { 10, -1,  0, -1, -1 }, { 10, -1,  0, -1, -1 },
    { 10,  1,  1,  0, -1 }, { 10,  1,  1,  0, -1 },
    { 10,  1,  0, -1, -1 }, { 10,  1,  0, -1, -1 },
    { 10, -1,  0, -1,  1 }, { 10, -1,  0, -1,  1 },
    { 10, -1, -1,  0, -1 }, { 10, -1, -1,  0, -1 },
    { 10, -1,  0,  1,  1 }, { 10, -1,  0,  1,  1 },
    { 10,  1,  0,  1,  1 }, { 10,  1,  0,  1,  1 },
    /* 11 bit */
    { 11,  1, -1,  1, -1 },
    { 11, -1,  1, -1,  1 },
    { 11, -1,  1,  1, -1 },
    { 11,  1, -1, -1,  1 },
    { 11,  1,  1,  1,  1 },
    { 11, -1, -1,  1,  1 },
    { 11,  1,  1, -1, -1 },
    { 11, -1, -1,  1, -1 },
    { 11, -1, -1, -1, -1 },
    { 11,  1,  1, -1,  1 },
    { 11,  1, -1,  1,  1 },
    { 11, -1,  1,  1,  1 },
    { 11, -1,  1, -1, -1 },
    { 11, -1, -1, -1,  1 },
    { 11,  1, -1, -1, -1 },
    { 11,  1,  1,  1, -1 }
};
