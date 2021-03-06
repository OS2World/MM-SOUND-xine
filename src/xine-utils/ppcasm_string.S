/*
 * BK Id: SCCS/s.string.S 1.9 10/25/01 10:08:51 trini
 */
/*
 * String handling functions for PowerPC.
 *
 * Copyright (C) 1996 Paul Mackerras.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include "../../config.h"

#ifdef ARCH_PPC

#warning Be forewarned - using PowerPC assembly

#define __KERNEL__
#define __ASSEMBLY__

#include "ppc_asm.tmpl"
#include <linux/config.h>
#include <asm/processor.h>
#include <asm/cache.h>
#include <asm/errno.h>

#define COPY_16_BYTES		\
	lwz	r7,4(r4);	\
	lwz	r8,8(r4);	\
	lwz	r9,12(r4);	\
	lwzu	r10,16(r4);	\
	stw	r7,4(r6);	\
	stw	r8,8(r6);	\
	stw	r9,12(r6);	\
	stwu	r10,16(r6)

#define COPY_16_BYTES_WITHEX(n)	\
8 ## n ## 0:			\
	lwz	r7,4(r4);	\
8 ## n ## 1:			\
	lwz	r8,8(r4);	\
8 ## n ## 2:			\
	lwz	r9,12(r4);	\
8 ## n ## 3:			\
	lwzu	r10,16(r4);	\
8 ## n ## 4:			\
	stw	r7,4(r6);	\
8 ## n ## 5:			\
	stw	r8,8(r6);	\
8 ## n ## 6:			\
	stw	r9,12(r6);	\
8 ## n ## 7:			\
	stwu	r10,16(r6)

#define COPY_16_BYTES_EXCODE(n)			\
9 ## n ## 0:					\
	addi	r5,r5,-(16 * n);		\
	b	104f;				\
9 ## n ## 1:					\
	addi	r5,r5,-(16 * n);		\
	b	105f;				\
.section __ex_table,"a";			\
	.align	2;				\
	.long	8 ## n ## 0b,9 ## n ## 0b;	\
	.long	8 ## n ## 1b,9 ## n ## 0b;	\
	.long	8 ## n ## 2b,9 ## n ## 0b;	\
	.long	8 ## n ## 3b,9 ## n ## 0b;	\
	.long	8 ## n ## 4b,9 ## n ## 1b;	\
	.long	8 ## n ## 5b,9 ## n ## 1b;	\
	.long	8 ## n ## 6b,9 ## n ## 1b;	\
	.long	8 ## n ## 7b,9 ## n ## 1b;	\
	.text

	.text

CACHELINE_BYTES = 32
LG_CACHELINE_BYTES = 5
CACHELINE_MASK = (32 -1)

	.global	ppcasm_cacheable_memcpy
ppcasm_cacheable_memcpy:
#if 0 /* this part causes "error loading shared library: unexpected reloc type
         0x0b (???) */
	add	r7,r3,r5		/* test if the src & dst overlap */
	add	r8,r4,r5
	cmplw	0,r4,r7
	cmplw	1,r3,r8
	crand	0,0,4			/* cr0.lt &= cr1.lt */
	blt	ppcasm_memcpy			/* if regions overlap */
#endif
	addi	r4,r4,-4
	addi	r6,r3,-4
	neg	r0,r3
	andi.	r0,r0,CACHELINE_MASK	/* # bytes to start of cache line */
	beq	58f

	cmplw	0,r5,r0			/* is this more than total to do? */
	blt	63f			/* if not much to do */
	andi.	r8,r0,3			/* get it word-aligned first */
	subf	r5,r0,r5
	mtctr	r8
	beq+	61f
70:	lbz	r9,4(r4)		/* do some bytes */
	stb	r9,4(r6)
	addi	r4,r4,1
	addi	r6,r6,1
	bdnz	70b
61:	srwi.	r0,r0,2
	mtctr	r0
	beq	58f
72:	lwzu	r9,4(r4)		/* do some words */
	stwu	r9,4(r6)
	bdnz	72b

58:	srwi.	r0,r5,LG_CACHELINE_BYTES /* # complete cachelines */
	clrlwi	r5,r5,32-LG_CACHELINE_BYTES
	li	r11,4
	mtctr	r0
	beq	63f
53:
#if !defined(CONFIG_8xx)
	dcbz	r11,r6
#endif
	COPY_16_BYTES
#if L1_CACHE_LINE_SIZE >= 32
	COPY_16_BYTES
#if L1_CACHE_LINE_SIZE >= 64
	COPY_16_BYTES
	COPY_16_BYTES
#if L1_CACHE_LINE_SIZE >= 128
	COPY_16_BYTES
	COPY_16_BYTES
	COPY_16_BYTES
	COPY_16_BYTES
#endif
#endif
#endif
	bdnz	53b

63:	srwi.	r0,r5,2
	mtctr	r0
	beq	64f
30:	lwzu	r0,4(r4)
	stwu	r0,4(r6)
	bdnz	30b

64:	andi.	r0,r5,3
	mtctr	r0
	beq+	65f
40:	lbz	r0,4(r4)
	stb	r0,4(r6)
	addi	r4,r4,1
	addi	r6,r6,1
	bdnz	40b
65:	blr

	.globl	ppcasm_memcpy
ppcasm_memcpy:
	srwi.	r7,r5,3
	addi	r6,r3,-4
	addi	r4,r4,-4
	beq	2f			/* if less than 8 bytes to do */
	andi.	r0,r6,3			/* get dest word aligned */
	mtctr	r7
	bne	5f
1:	lwz	r7,4(r4)
	lwzu	r8,8(r4)
	stw	r7,4(r6)
	stwu	r8,8(r6)
	bdnz	1b
	andi.	r5,r5,7
2:	cmplwi	0,r5,4
	blt	3f
	lwzu	r0,4(r4)
	addi	r5,r5,-4
	stwu	r0,4(r6)
3:	cmpwi	0,r5,0
	beqlr
	mtctr	r5
	addi	r4,r4,3
	addi	r6,r6,3
4:	lbzu	r0,1(r4)
	stbu	r0,1(r6)
	bdnz	4b
	blr
5:	subfic	r0,r0,4
	mtctr	r0
6:	lbz	r7,4(r4)
	addi	r4,r4,1
	stb	r7,4(r6)
	addi	r6,r6,1
	bdnz	6b
	subf	r5,r0,r5
	rlwinm.	r7,r5,32-3,3,31
	beq	2b
	mtctr	r7
	b	1b

#endif /* ARCH_PPC */
