/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef	_VDBE_H_
#define	_VDBE_H_

#include <stdio.h>

/*
 * This header defines the interface to the virtual database engine
 * or VDBE.  The VDBE implements an abstract machine that runs a
 * simple program to access and modify the underlying database.
 */

#if defined(__cplusplus)
extern "C" {
#endif


/*
 * A single VDBE is an opaque structure named "vdbe_t".  Only routines
 * in the source file vdbe_aux.c are allowed to see the insides
 * of this structure.
 */
typedef struct vdbe vdbe_t;

/*
 * As SQL is translated into a sequence of instructions to be
 * executed by a virtual machine each instruction is an instance
 * of the following structure.  A single instruction of the virtual
 * machine has an opcode and as many as three operands.
 */
struct vdbe_op {
  int opcode;         /* What operation to perform */
  int p1;             /* First operand */
  int p2;             /* Second parameter (often the jump destination) */
  char *p3;           /* Third parameter */
  int p3type;         /* P3_STATIC, P3_DYNAMIC or P3_POINTER */
#ifdef VDBE_PROFILE
  int cnt;            /* Number of times this instruction was executed */
  long long cycles;   /* Total time spend executing this instruction */
#endif
};
typedef struct vdbe_op vdbe_op_t;

/*
** Allowed values of vdbe_op_t.p3type
*/
#define P3_NOTUSED    0   /* The P3 parameter is not used */
#define P3_DYNAMIC  (-1)  /* Pointer to a string obtained from __os_calloc() */
#define P3_STATIC   (-2)  /* Pointer to a static string */
#define P3_POINTER  (-3)  /* P3 is a pointer to some structure or object */

/*
** The following macro converts a relative address in the p2 field
** of a vdbe_op_t structure into a negative number so that 
** __vdbe_add_op_list() knows that the address is relative.  Calling
** the macro again restores the address.
*/
#define ADDR(X)  (-1-(X))

/*
** The makefile scans the vdbe.c source file and creates the "opcodes.h"
** header file that defines a number for each opcode used by the VDBE.
*/
#include "opcodes.h"

#if defined(__cplusplus)
}
#endif
#endif /* !_VDBE_H_ */
