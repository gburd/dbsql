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
 *
 * UPDATED for register-based VDBE (SQLite 3.52.0 architecture).
 * The instruction format now has 5 operands: P1 (int), P2 (int),
 * P3 (int), P4 (union), P5 (u16 flags).
 */

#if defined(__cplusplus)
extern "C" {
#endif


/*
 * A single VDBE is an opaque structure named "vdbe_t".  Only routines
 * in the source file vdbe_method.c are allowed to see the insides
 * of this structure.
 */
typedef struct vdbe vdbe_t;

/*
 * The names of the following types declared in vdbe_int.h are required
 * for the VdbeOp definition.
 */
typedef struct mem mem_t;
typedef struct sub_program sub_program_t;

/*
 * A single instruction of the virtual machine has an opcode and as
 * many as five operands.  The instruction is recorded as an instance
 * of the following structure.
 *
 * This is the register-based format (SQLite 3.52.0 style):
 *   P1 (int)   - First operand, usually a register number or cursor
 *   P2 (int)   - Second operand, often the jump destination
 *   P3 (int)   - Third operand, often the output register
 *   P4 (union) - Fourth operand: rich typed union (string, int, ptr, etc.)
 *   P5 (u16)   - Fifth operand: unsigned 16-bit flags
 *
 * This replaces the old 3-operand stack-based format where P3 was char*.
 */
struct vdbe_op {
  u8 opcode;              /* What operation to perform */
  signed char p4type;     /* One of the P4_xxx constants for p4 */
  u16 p5;                 /* Fifth parameter is an unsigned 16-bit integer */
  int p1;                 /* First operand */
  int p2;                 /* Second parameter (often the jump destination) */
  int p3;                 /* Third parameter */
  union p4union {         /* Fourth parameter */
    int i;                     /* Integer value if p4type==P4_INT32 */
    void *p;                   /* Generic pointer */
    char *z;                   /* Pointer to data for string (char array) types */
    i64 *pI64;                 /* Used when p4type is P4_INT64 */
    double *pReal;             /* Used when p4type is P4_REAL */
    func_def_t *pFunc;         /* Used when p4type is P4_FUNCDEF */
    struct __dbsql_func *pCtx; /* Used when p4type is P4_FUNCCTX */
    coll_seq_t *pColl;         /* Used when p4type is P4_COLLSEQ */
    mem_t *pMem;               /* Used when p4type is P4_MEM */
    vtable_t *pVtab;           /* Used when p4type is P4_VTAB */
    key_info_t *pKeyInfo;      /* Used when p4type is P4_KEYINFO */
    u32 *ai;                   /* Used when p4type is P4_INTARRAY */
    sub_program_t *pProgram;   /* Used when p4type is P4_SUBPROGRAM */
    table_t *pTab;             /* Used when p4type is P4_TABLE */
  } p4;
#ifdef VDBE_PROFILE
  u64 nExec;              /* Number of times this instruction was executed */
  u64 nCycle;             /* Total CPU cycles spent on this instruction */
#endif
};
typedef struct vdbe_op vdbe_op_t;

/* VdbeOp is the SQLite 3.52.0 name for the instruction struct */
typedef struct vdbe_op VdbeOp;

/*
 * A sub-program used to implement a trigger program.
 * (SQLite: struct SubProgram)
 */
struct sub_program {
  vdbe_op_t *opcodes;          /* Array of opcodes for sub-program */
  int nOp;                     /* Elements in opcodes[] */
  int nMem;                    /* Number of memory cells required */
  int nCsr;                    /* Number of cursors required */
  u8 *aOnce;                   /* Array of OP_Once flags */
  void *token;                 /* id that may be used to detect recursion */
  sub_program_t *next_program; /* Next sub-program already visited */
};

/*
 * A smaller version of vdbe_op_t used for __vdbe_add_op_list() because
 * it takes up less space.
 * (SQLite: struct VdbeOpList)
 */
struct vdbe_op_list {
  u8 opcode;              /* What operation to perform */
  signed char p1;         /* First operand */
  signed char p2;         /* Second parameter (often the jump destination) */
  signed char p3;         /* Third parameter */
};
typedef struct vdbe_op_list vdbe_op_list_t;
typedef struct vdbe_op_list VdbeOpList;

/*
 * Allowed values of vdbe_op_t.p4type
 *
 * Values >= P4_FREE_IF_LE own resources that must be freed when the
 * opcode is deleted.  Values > P4_FREE_IF_LE do not own resources.
 */
#define P4_NOTUSED      0   /* The P4 parameter is not used */
#define P4_TRANSIENT    0   /* P4 is a pointer to a transient string */
#define P4_STATIC     (-1)  /* Pointer to a static string */
#define P4_COLLSEQ    (-2)  /* P4 is a pointer to a coll_seq_t structure */
#define P4_INT32      (-3)  /* P4 is a 32-bit signed integer */
#define P4_SUBPROGRAM (-4)  /* P4 is a pointer to a sub_program_t */
#define P4_TABLE      (-5)  /* P4 is a pointer to a table_t structure */
/* Above do not own any resources.  Must free those below */
#define P4_FREE_IF_LE (-6)
#define P4_DYNAMIC    (-6)  /* Pointer to memory from __os_malloc() */
#define P4_FUNCDEF    (-7)  /* P4 is a pointer to a func_def_t structure */
#define P4_KEYINFO    (-8)  /* P4 is a pointer to a key_info_t structure */
#define P4_EXPR       (-9)  /* P4 is a pointer to an expr_t tree */
#define P4_MEM        (-10) /* P4 is a pointer to a Mem structure */
#define P4_VTAB       (-11) /* P4 is a pointer to a vtable_t structure */
#define P4_REAL       (-12) /* P4 is a 64-bit floating point value */
#define P4_INT64      (-13) /* P4 is a 64-bit signed integer */
#define P4_INTARRAY   (-14) /* P4 is a vector of 32-bit integers */
#define P4_FUNCCTX    (-15) /* P4 is a pointer to a __dbsql_func object */
#define P4_TABLEREF   (-16) /* Like P4_TABLE, but reference counted */

/* Legacy P3 type aliases for backward compatibility during transition */
#define P3_NOTUSED    P4_NOTUSED
#define P3_DYNAMIC    P4_DYNAMIC
#define P3_STATIC     P4_STATIC
#define P3_POINTER    P4_DYNAMIC  /* P3_POINTER maps to dynamic P4 */

/* Error message codes for OP_Halt P5 */
#define P5_ConstraintNotNull 1
#define P5_ConstraintUnique  2
#define P5_ConstraintCheck   3
#define P5_ConstraintFK      4

/*
 * The Vdbe.aColName array contains 5n Mem structures, where n is the
 * number of columns of data returned by the statement.
 */
#define COLNAME_NAME     0
#define COLNAME_DECLTYPE 1
#define COLNAME_DATABASE 2
#define COLNAME_TABLE    3
#define COLNAME_COLUMN   4
#define COLNAME_N        2    /* Store the name and decltype */

/*
 * The following macro converts a label into an index into the
 * parser_t.aLabel[] array that contains the resolved address.
 * (SQLite 3.52.0 uses ~(X); old DBSQL used -1-(X))
 */
#define ADDR(X)  (~(X))

/*
 * The makefile scans the vdbe.c source file and creates the "opcodes.h"
 * header file that defines a number for each opcode used by the VDBE.
 */
#include "opcodes.h"

#if defined(__cplusplus)
}
#endif
#endif /* !_VDBE_H_ */
