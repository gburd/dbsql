/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * http://creativecommons.org/licenses/GPL/2.0/
 *
 * $Id: vdbe.h 7 2007-02-03 13:34:17Z gburd $
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
