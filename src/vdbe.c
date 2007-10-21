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
 * $Id: vdbe.c 7 2007-02-03 13:34:17Z gburd $
 */

/* The code in this file implements execution method of the 
 * Virtual Database Engine (VDBE).  A separate file ("vdbe_aux.c")
 * handles housekeeping details such as creating and deleting
 * VDBE instances.  This file is solely interested in executing
 * the VDBE program.
 *
 * In the external interface, an "dbsql_stmt_t*" is an opaque pointer
 * to a VDBE.
 *
 * The SQL parser generates a program which is then executed by
 * the VDBE to do the work of the SQL statement.  VDBE programs are 
 * similar in form to assembly language.  The program consists of
 * a linear sequence of operations.  Each operation has an opcode 
 * and 3 operands.  Operands P1 and P2 are integers.  Operand P3 
 * is a null-terminated string.   The P2 operand must be non-negative.
 * Opcodes will typically ignore one or more operands.  Many opcodes
 * ignore all three operands.
 *
 * Computation results are stored on a stack.  Each entry on the
 * stack is either an integer, a null-terminated string, a floating point
 * number, or the SQL "NULL" value.  An inplicit conversion from one
 * type to the other occurs as necessary.
 * 
 * Most of the code in this file is taken up by the __vdbe_exec()
 * function which does the work of interpreting a VDBE program.
 * But other routines are also provided to help in building up
 * a program instruction by instruction.
 *
 * Various scripts scan this source file in order to generate HTML
 * documentation, headers files, or other derived files.  The formatting
 * of the code in this file is, therefore, important.  See other comments
 * in this file for details.  If in doubt, do not deviate from existing
 * commenting and indentation practices when changing or adding code.
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#endif

#include "dbsql_int.h"
#include "inc/vdbe_int.h"

/*
 * The following global variable is incremented every time a cursor
 * moves, either by the OP_MoveTo or the OP_Next opcode.  The test
 * procedures use this information to make sure that indices are
 * working correctly.
 * This variable has no function other than to help verify the correct
 * operation of the library.
 */
#ifdef CONFIG_TEST
int dbsql_search_count = 0;
#endif

/*
 * When this global variable is positive, it gets decremented once before
 * each instruction in the VDBE.  When reaches zero, the DBSQL_Interrupt
 * of the db.flags field is set in order to simulate and interrupt.
 * This variable is used for testing purposes only.  It does not function
 * in an ordinary build.
 */
#ifdef CONFIG_TEST
int dbsql_interrupt_count = 0;
#endif

/*
 * __api_step --
 *	Advance the virtual machine to the next output row.
 *
 *	The return vale will be either DBSQL_BUSY, DBSQL_DONE, 
 *	DBSQL_ROW, DBSQL_ERROR, or DBSQL_MISUSE.
 *
 *	DBSQL_BUSY means that the virtual machine attempted to open
 *	a locked database and there is no busy callback registered.
 *	Call DBSQL->step() again to retry the open.  '*n' is set to 0
 *	and '*col_names' and '*values' are both set to NULL.
 *
 *	DBSQL_DONE means that the virtual machine has finished
 *	executing.  DBSQL->step() should not be called again on this
 *	virtual machine.  '*n' and '*col_names' are set appropriately
 *	but '*values' is set to NULL.
 *
 *	DBSQL_ROW means that the virtual machine has generated another
 *	row of the result set.  '*n' is set to the number of columns in
 *	the row.  '*col_name' is set to the names of the columns followed
 *	by the column datatypes.  '*values' is set to the values of each
 *	column in the row.  The value of the i-th column is (*values)[i].
 *	The name of the i-th column is (*col_names)[i] and the datatype
 *	of the i-th column is (*col_names)[i+*n].
 *
 *	DBSQL_ERROR means that a run-time error (such as a constraint
 *	violation) has occurred.  The details of the error will be returned
 *	by the next call to DBSQL->finalize().  DBSQL->step() should not
 *	be called again on the VM.
 *
 *	DBSQL_MISUSE means that the this routine was called inappropriately.
 *	Perhaps it was called on a virtual machine that had already been
 *	finalized or on one that had previously returned DBSQL_ERROR or
 *	DBSQL_DONE.  Or it could be the case the the same database connection
 *	is being used simulataneously by two or more threads.
 *
 * PUBLIC: int __api_step __P((dbsql_stmt_t *, int *, const char ***,
 * PUBLIC:                const char ***));
 *
 * p				The virtual machine to execute
 * n				OUT: Number of columns in result
 * values			OUT: Column data
 * col_names			OUT: Column names and datatypes
 */
int
__api_step(p, n, values, col_names)
	dbsql_stmt_t *p;
	int *n;
	const char ***values;
	const char ***col_names;
{
	vdbe_t *vm = (vdbe_t*)p;
	DBSQL *db;
	int rc;

	if (vm->magic != VDBE_MAGIC_RUN) {
		return DBSQL_MISUSE;
	}
	db = vm->db;
	if (__safety_on(db)) {
		return DBSQL_MISUSE;
	}
	if (vm->explain) {
		rc = __vdbe_list(vm);
	} else {
		rc = __vdbe_exec(vm);
	}
	if (rc == DBSQL_DONE || rc == DBSQL_ROW) {
		if (col_names)
			*col_names = (const char**)vm->azColName;
		if (n)
			*n = vm->nResColumn;
	} else {
		if (col_names)
			*col_names = 0;
		if (n)
			*n = 0;
	}
	if (values) {
		if (rc == DBSQL_ROW) {
			*values = (const char**)vm->azResColumn;
		} else {
			*values = 0;
		}
	}
	if (__safety_off(db)) {
		return DBSQL_MISUSE;
	}
	return rc;
}

/*
 * __agg_insert --
 *	Insert a new aggregate element and make it the element that
 *	has focus.
 *	Return 0 on success and 1 if memory is exhausted.
 *
 * STATIC: static int __agg_insert __P((agg_t *, char *, int));
 */
static int
__agg_insert(p, key, len)
	agg_t *p;
	char *key;
	int len;
{
	agg_elem_t *elem, *old;
	int i;
	mem_t *mem;

	if (__dbsql_calloc(NULL, 1, sizeof(agg_elem_t) + len +
			((p->nMem - 1) * sizeof(elem->aMem[0])),
			&elem) == ENOMEM)
		return 1;
	elem->zKey = (char*)&elem->aMem[p->nMem];
	memcpy(elem->zKey, key, len);
	elem->nKey = len;
	old = __hash_insert(&p->hash, elem->zKey, elem->nKey, elem);
	if (old != 0) {
		DBSQL_ASSERT(old == elem);  /* Malloc failed on insert */
		__dbsql_free(NULL, old);
		return 0;
	}
	for (i = 0, mem = elem->aMem; i < p->nMem; i++, mem++) {
		mem->flags = MEM_Null;
	}
	p->pCurrent = elem;
	return 0;
}

/*
 * __agg_in_focus --
 *	Get the agg_elem_t currently in focus
 *
 * STATIC: static agg_elem_t *__agg_in_focus __P((agg_t *p));
 */
static agg_elem_t *
__agg_in_focus(p)
	agg_t *p;
{
	if (p->pCurrent) {
		return p->pCurrent;
	} else {
		hash_ele_t *elem;
		elem = __hash_first(&p->hash);
		if (elem == 0) {
			__agg_insert(p,"",1);
			elem = __hash_first(&p->hash);
		}
		return elem ? __hash_data(elem) : 0;
	}
}

/*
 * __entity_as_string --
 *	Convert the given stack entity into a string if it isn't one
 *	already.
 *
 * STATIC: static int __entity_as_string __P((mem_t *));
 */
static int
__entity_as_string(stack)
	mem_t *stack;
{
	if ((stack->flags & MEM_Str) == 0) {
		int fg = stack->flags;
		if (fg & MEM_Real) {
			snprintf(stack->zShort, sizeof(stack->zShort),
					"%.15g", stack->r);
		} else if (fg & MEM_Int) {
			snprintf(stack->zShort, sizeof(stack->zShort),
					"%d",stack->i);
		} else {
			stack->zShort[0] = 0;
		}
		stack->z = stack->zShort;
		stack->n = strlen(stack->zShort) + 1;
		stack->flags = MEM_Str | MEM_Short;
		return 0;
	}
}

/*
 * __entity_to_string -- 
 *	Convert the given stack entity into a string that has been obtained
 *	from __dbsql_calloc().  This is different from __entity_as_string()
 *	above in that __entity_as_string() will use the NBFS bytes of static
 *	string space if the string will fit but this routine will always
 *	__dbsql_malloc new memory. Return non-zero if we run out of memory.
 *
 * STATIC: int __entity_to_string __P((mem_t *));
 */
static int
__entity_to_string(stack)
	mem_t *stack;
{
	if ((stack->flags & MEM_Dyn) == 0) {
		int fg = stack->flags;
		char *z;
		__entity_as_string(stack);
		DBSQL_ASSERT((fg & MEM_Dyn) == 0);
		if (__dbsql_calloc(NULL, 1, stack->n, &z) == ENOMEM)
			return 1;
		memcpy(z, stack->z, stack->n);
		stack->z = z;
		stack->flags |= MEM_Dyn;
	}
	return 0;
}

/*
 * __entity_ephem_to_dyn --
 *	An ephemeral string value (signified by the MEM_Ephem flag) contains
 *	a pointer to a dynamically allocated string where some other entity
 *	is responsible for deallocating that string.  Because the stack entry
 *	does not control the string, it might be deleted without the stack
 *	entry knowing it.
 *
 *	This routine converts an ephemeral string into a dynamically allocated
 *	string that the stack entry itself controls.  In other words, it
 *	converts an MEM_Ephem string into an MEM_Dyn string.
 *
 * STATIC: static int __entity_ephem_to_dyn __P((mem_t * stack));
 */
static int
__entity_ephem_to_dyn(stack)
	mem_t *stack;
{
	if ((stack->flags & MEM_Ephem) != 0) {
		char *z;
		if (__dbsql_calloc(NULL, 1, stack->n, &z) == ENOMEM)
			return 1;
		memcpy(z, stack->z, stack->n);
		stack->z = z;
		stack->flags &= ~MEM_Ephem;
		stack->flags |= MEM_Dyn;
	}
	return 0;
}

/*
 * __entity_release_mem --
 *	Release the memory associated with the given stack level.  This
 *	leaves the mem_t.flags field in an inconsistent state.
 *
 * STATIC: static void __entity_release_mem __P((mem_t *));
 */
static void
__entity_release_mem(stack)
	mem_t *stack;
{
	if ((stack->flags & MEM_Dyn) != 0) {
		__dbsql_free(NULL, stack->z);
	}
}

/*
 * __pop_stack --
 *	Pop the stack N times.
 *
 * STATIC: void __pop_stack __P((mem_t **, int));
 */
static void
__pop_stack(stack, n)
	mem_t **stack;
	int n;
{
	mem_t *tos = *stack;
	while(n > 0) {
		n--;
		__entity_release_mem(tos);
		tos--;
	}
	*stack = tos;
}

/*
 * __entity_to_int --
 *	Convert the given stack entity into a integer if it isn't one
 *	already.  Any prior string or real representation is invalidated.  
 *	NULLs are converted into 0.
 *
 * STATIC: static void __entity_to_int __P((mem_t *));
 */
static void
__entity_to_int(stack)
	mem_t *stack;
{
	if ((stack->flags & MEM_Int) == 0) {
		if (stack->flags & MEM_Real) {
			stack->i = (int)stack->r;
			__entity_release_mem(stack);
		} else if (stack->flags & MEM_Str) {
			__dbsql_atoi(stack->z, &stack->i);
			__entity_release_mem(stack);
		} else {
			stack->i = 0;
		}
		stack->flags = MEM_Int;
	}
}

/*
 * __entity_to_real --
 *	Get a valid Real representation for the given stack element.
 *	Any prior string or integer representation is retained.
 *	NULLs are converted into 0.0.
 *
 * STATIC: static void __entity_to_real __P((mem_t *));
 */
static void
__entity_to_real(stack)
	mem_t *stack;
{
	if ((stack->flags & MEM_Real) == 0) {
		if (stack->flags & MEM_Str) {
			stack->r = __dbsql_atof(stack->z);
		} else if (stack->flags & MEM_Int) {
			stack->r = stack->i;
		} else {
			stack->r = 0.0;
		}
		stack->flags |= MEM_Real;
	}
}

/*
 * __sorted_merge --
 *	The parameters are pointers to the head of two sorted lists
 *	of sorter_t structures.  Merge these two lists together and return
 *	a single sorted list.  This routine forms the core of the merge-sort
 *	algorithm.  In the case of a tie, left sorts in front of right.
 *
 * STATIC: static sorter_t * __sorted_merge __P((sorter_t *, sorter_t *));
 */
static sorter_t *
__sorted_merge(left, right)
	sorter_t *left;
	sorter_t *right;
{
	sorter_t head;
	sorter_t *tail;
	tail = &head;
	tail->pNext = 0;
	while(left && right) {
		int c = __str_cmp(left->zKey, right->zKey);
		if (c <= 0) {
			tail->pNext = left;
			left = left->pNext;
		} else {
			tail->pNext = right;
			right = right->pNext;
		}
		tail = tail->pNext;
	}
	if (left) {
		tail->pNext = left;
	} else if (right) {
		tail->pNext = right;
	}
	return head.pNext;
}

/*
 * __fgets --
 *	The following routine works like a replacement for the standard
 *	library routine fgets().  The difference is in how end-of-line (EOL)
 *	is handled.  Standard fgets() uses LF for EOL under unix, CRLF
 *	under windows, and CR under mac.  This routine accepts any of these
 *	character sequences as an EOL mark.  The EOL mark is replaced by
 *	a single LF character in zBuf.
 *
 * STATIC: static char *__fgets __P((char *, int, FILE *));
 */
static char *
__fgets(buf, len, in)
	char *buf;
	int len;
	FILE *in;
{
	int i, c;
	for(i = 0; i < (len - 1) && (c = getc(in)) != EOF; i++) {
		buf[i] = c;
		if (c == '\r' || c == '\n') {
			if (c == '\r') {
				buf[i] = '\n';
				c = getc(in);
				if (c != EOF && c != '\n')
					ungetc(c, in);
			}
			i++;
			break;
		}
	}
	buf[i]  = 0;
	return (i > 0 ? buf : 0);
}

/*
 * __expand_cursor_array_size --
 *	Make sure there is space in the vdbe_t structure to hold at least
 *	'num_cursors' cursors.  If there is not currently enough space, then
 *	allocate more.  If a memory allocation error occurs, return 1.
 *	Return 0 if everything works.
 *
 * STATIC: static int __expand_cursor_array_size __P((vdbe_t *, int));
 */
static int
__expand_cursor_array_size(vm, num_cursors)
	vdbe_t *vm;
	int num_cursors;
{
	if (num_cursors >= vm->nCursor) {
		if (__dbsql_realloc(NULL, ((num_cursors + 1) *
				  sizeof(cursor_t)), &vm->aCsr) == ENOMEM)
			return 1;
		memset(&vm->aCsr[vm->nCursor], 0,
		       (sizeof(cursor_t) * (num_cursors + 1 - vm->nCursor)));
		vm->nCursor = num_cursors + 1;
	}
	return 0;
}

/*
 * The CHECK_FOR_INTERRUPT macro defined here looks to see if the
 * DBSQL->interrupt() routine has been called.  If it has been, then
 * processing of the VDBE program is interrupted.
 *
 * This macro added to every instruction that does a jump in order to
 * implement a loop.  This test used to be on every single instruction,
 * but that meant we had more testing that we needed.  By only testing the
 * flag on jump instructions, we get a (small) speed improvement.
 */
#define CHECK_FOR_INTERRUPT \
   if( db->flags & DBSQL_Interrupt ) goto abort_due_to_interrupt;


/*
 * __vdbe_exec --
 *	Execute as much of a VDBE program as we can then return.
 *
 *	__vdbe_make_ready() must be called before this routine in order to
 *	close the program with a final OP_Halt and to set up the callbacks
 *	and the error message pointer.
 *
 *	Whenever a row or result data is available, this routine will either
 *	invoke the result callback (if there is one) or return with
 *	DBSQL_ROW.
 *
 *	If an attempt is made to open a locked database, then this routine
 *	will either invoke the busy callback (if there is one) or it will
 *	return DBSQL_BUSY.
 *
 *	If an error occurs, an error message is written to memory obtained
 *	from __dbsql_calloc() and p->zErrMsg is made to point to that memory.
 *	The error code is stored in p->rc and this routine returns DBSQL_ERROR.
 *
 *	If the callback ever returns non-zero, then the program exits
 *	immediately.  There will be no error message but the p->rc field is
 *	set to DBSQL_ABORT and this routine will return DBSQL_ERROR.
 *
 *	A memory allocation error causes p->rc to be set to DBSQL_NOMEM and
 *	this routine to return DBSQL_ERROR.
 *
 *	Other fatal errors return DBSQL_ERROR.
 *
 *	After this routine has finished, __vdbe_finalize() should be
 *	used to clean up the mess that was left behind.
 *
 * PUBLIC: int __vdbe_exec __P((vdbe_t *));
 */
int
__vdbe_exec(p)
	vdbe_t *p;
{
	int pc;                    /* The program counter */
	vdbe_op_t *pOp;            /* Current operation */
	int rc = DBSQL_SUCCESS;    /* Value to return */
	DBSQL *db = p->db;         /* The database */
	mem_t *pTos;               /* Top entry in the operand stack */
#define BUF_SIZE 100
	char zBuf[BUF_SIZE];       /* Space to sprintf() an integer */
#ifdef VDBE_PROFILE
	unsigned long long start;  /* CPU clock count at start of opcode */
	int origPc;                /* Program counter at start of opcode */
#endif
#ifndef DBSQL_NO_PROGRESS
	int nProgressOps = 0;      /* Opcodes executed since progress
				      callback. */
#endif

	if (p->magic != VDBE_MAGIC_RUN)
		return DBSQL_MISUSE;
	DBSQL_ASSERT(db->magic == DBSQL_STATUS_BUSY);
	DBSQL_ASSERT(p->rc == DBSQL_SUCCESS || p->rc == DBSQL_BUSY);
	p->rc = DBSQL_SUCCESS;
	DBSQL_ASSERT(p->explain == 0);
	if (p->rc == ENOMEM)
		goto no_mem;
	pTos = p->pTos;
	if (p->popStack) {
		__pop_stack(&pTos, p->popStack);
		p->popStack = 0;
	}
	for(pc = p->pc; rc == DBSQL_SUCCESS; pc++) {
		DBSQL_ASSERT(pc >= 0 && pc < p->nOp);
		DBSQL_ASSERT(pTos <= &p->aStack[pc]);
#ifdef VDBE_PROFILE
		origPc = pc;
		start = __os_hwtime();
#endif
		pOp = &p->aOp[pc];

		/*
		 * Only allow tracing if NDEBUG is not defined.
		 * TODO, DIAGNOSTIC?
		 */
#ifndef NDEBUG
		if (p->trace) {
			__vdbe_print_op(p->trace, pc, pOp);
		}
#endif

		/*
		 * Check to see if we need to simulate an interrupt.  This
		 * only happens if we have a special test build.
		 */
#ifdef CONFIG_TEST
		if (dbsql_interrupt_count > 0) {
			dbsql_interrupt_count--;
			if (dbsql_interrupt_count == 0) {
				db->interrupt(db);
			}
		}
#endif

#ifndef DBSQL_NO_PROGRESS
		/*
		 * Call the progress callback if it is configured and the
		 * required number of VDBE ops have been executed (either
		 * since this invocation of __vdbe_exec() or since last time
		 * the progress callback was called).  If the progress
		 * callback returns non-zero, exit the virtual machine with
		 * a return code DBSQL_ABORT.
		 */
		if (db->xProgress) {
			if (db->nProgressOps == nProgressOps) {
				if (db->xProgress(db->pProgressArg) != 0) {
					rc = DBSQL_ABORT;
					continue;
				}
				nProgressOps = 0;
			}
			nProgressOps++;
		}
#endif

switch(pOp->opcode) {

/*****************************************************************************
 * What follows is a massive switch statement where each case implements a
 * separate instruction in the virtual machine.  If we follow the usual
 * indentation conventions, each case should be indented by 6 spaces.  But
 * that is a lot of wasted space on the left margin.  So the code within
 * the switch statement will break with convention and be flush-left. Another
 * big comment (similar to this one) will mark the point in the code where
 * we transition back to normal indentation.
 *
 * The formatting of each case is important.  The makefile generates
 * two C files "opcodes.h" and "opcodes.c" by scanning this file looking
 * for lines that begin with "case OP_".  The opcodes.h files
 * will be filled with #defines that give unique integer values to each
 * opcode and the opcodes.c file is filled with an array of strings where
 * each string is the symbolic name for the corresponding opcode.
 *
 * Documentation about VDBE opcodes is generated by scanning this file
 * for lines of that contain "Opcode:".  That line and all subsequent
 * comment lines are used in the generation of the opcode.html documentation
 * file.
 *
 * SUMMARY:
 *
 *     Formatting is important to scripts that scan this file.
 *     Do not deviate from the formatting style currently in use.
 *
 ****************************************************************************/

/* Opcode:  Goto * P2 *
**
** An unconditional jump to address P2.
** The next instruction executed will be 
** the one at index P2 from the beginning of
** the program.
*/
case OP_Goto: {
	CHECK_FOR_INTERRUPT;
	pc = pOp->p2 - 1;
	break;
}

/* Opcode:  Gosub * P2 *
**
** Push the current address plus 1 onto the return address stack
** and then jump to address P2.
**
** The return address stack is of limited depth.  If too many
** OP_Gosub operations occur without intervening OP_Returns, then
** the return address stack will fill up and processing will abort
** with a fatal error.
*/
case OP_Gosub: {
	if (p->returnDepth >=
	    (sizeof(p->returnStack) / sizeof(p->returnStack[0]))) {
		__str_append(&p->zErrMsg, "return address stack overflow",
			     (char*)0);
		p->rc = DBSQL_INTERNAL;
		return DBSQL_ERROR;
	}
	p->returnStack[p->returnDepth++] = pc+1;
	pc = pOp->p2 - 1;
	break;
}

/* Opcode:  Return * * *
**
** Jump immediately to the next instruction after the last unreturned
** OP_Gosub.  If an OP_Return has occurred for all OP_Gosubs, then
** processing aborts with a fatal error.
*/
case OP_Return: {
	if (p->returnDepth <= 0) {
		__str_append(&p->zErrMsg, "return address stack underflow",
			     (char*)0);
		p->rc = DBSQL_INTERNAL;
		return DBSQL_ERROR;
	}
	p->returnDepth--;
	pc = p->returnStack[p->returnDepth] - 1;
	break;
}

/* Opcode:  Halt P1 P2 *
**
** Exit immediately.  All open cursors, Lists, Sorts, etc are closed
** automatically.
**
** P1 is the result code returned by dbsql_exec().  For a normal
** halt, this should be DBSQL_SUCCESS (0).  For errors, it can be some
** other value.  If P1!=0 then P2 will determine whether or not to
** rollback the current transaction.  Do not rollback if P2==OE_Fail.
** Do the rollback if P2==OE_Rollback.  If P2==OE_Abort, then back
** out all changes that have occurred during this execution of the
** VDBE, but do not rollback the transaction. 
**
** There is an implied "Halt 0 0 0" instruction inserted at the very end of
** every program.  So a jump past the last instruction of the program
** is the same as executing Halt.
*/
case OP_Halt: {
	p->magic = VDBE_MAGIC_HALT;
	p->pTos = pTos;
	if (pOp->p1 != DBSQL_SUCCESS) {
		p->rc = pOp->p1;
		p->errorAction = pOp->p2;
		if (pOp->p3) {
			__str_append(&p->zErrMsg, pOp->p3, (char*)0);
		}
		return DBSQL_ERROR;
	} else {
		p->rc = DBSQL_SUCCESS;
		return DBSQL_DONE;
	}
}

/* Opcode: Integer P1 * P3
**
** The integer value P1 is pushed onto the stack.  If P3 is not zero
** then it is assumed to be a string representation of the same integer.
*/
case OP_Integer: {
	pTos++;
	pTos->i = pOp->p1;
	pTos->flags = MEM_Int;
	if (pOp->p3) {
		pTos->z = pOp->p3;
		pTos->flags |= MEM_Str | MEM_Static;
		pTos->n = strlen(pOp->p3) + 1;
	}
	break;
}

/* Opcode: String * * P3
**
** The string value P3 is pushed onto the stack.  If P3==0 then a
** NULL is pushed onto the stack.
*/
case OP_String: {
	char *z = pOp->p3;
	pTos++;
	if( z==0 ){
		pTos->flags = MEM_Null;
	} else {
		pTos->z = z;
		pTos->n = strlen(z) + 1;
		pTos->flags = MEM_Str | MEM_Static;
	}
	break;
}

/* Opcode: Variable P1 * *
**
** Push the value of variable P1 onto the stack.  A variable is
** an unknown in the original SQL string as handed to DBSQL->prepare().
** Any occurance of the '?' character in the original SQL is considered
** a variable.  Variables in the SQL string are number from left to
** right beginning with 1.  The values of variables are set using the
** dbsql_bind() API.
*/
case OP_Variable: {
	int j = pOp->p1 - 1;
	pTos++;
	if (j >= 0 && j < p->nVar && p->azVar[j] != 0) {
		pTos->z = p->azVar[j];
		pTos->n = p->anVar[j];
		pTos->flags = MEM_Str | MEM_Static;
	} else {
		pTos->flags = MEM_Null;
	}
	break;
}

/* Opcode: Pop P1 * *
**
** P1 elements are popped off of the top of stack and discarded.
*/
case OP_Pop: {
	DBSQL_ASSERT(pOp->p1 >= 0);
	__pop_stack(&pTos, pOp->p1);
	DBSQL_ASSERT(pTos >= &p->aStack[-1]);
	break;
}

/* Opcode: Dup P1 P2 *
**
** A copy of the P1-th element of the stack 
** is made and pushed onto the top of the stack.
** The top of the stack is element 0.  So the
** instruction "Dup 0 0 0" will make a copy of the
** top of the stack.
**
** If the content of the P1-th element is a dynamically
** allocated string, then a new copy of that string
** is made if P2==0.  If P2!=0, then just a pointer
** to the string is copied.
**
** Also see the Pull instruction.
*/
case OP_Dup: {
	mem_t *pFrom = &pTos[-pOp->p1];
	DBSQL_ASSERT(pFrom <= pTos && pFrom >= p->aStack);
	pTos++;
	memcpy(pTos, pFrom, (sizeof(*pFrom) - NBFS));
	if (pTos->flags & MEM_Str) {
		if (pOp->p2 && (pTos->flags & (MEM_Dyn | MEM_Ephem))) {
			pTos->flags &= ~MEM_Dyn;
			pTos->flags |= MEM_Ephem;
		} else if (pTos->flags & MEM_Short) {
			memcpy(pTos->zShort, pFrom->zShort, pTos->n);
			pTos->z = pTos->zShort;
		} else if ((pTos->flags & MEM_Static) == 0) {
			if (__dbsql_calloc(NULL, 1, pFrom->n,
					   &pTos->z) == ENOMEM)
				goto no_mem;
			memcpy(pTos->z, pFrom->z, pFrom->n);
			pTos->flags &= ~(MEM_Static | MEM_Ephem | MEM_Short);
			pTos->flags |= MEM_Dyn;
		}
	}
	break;
}

/* Opcode: Pull P1 * *
**
** The P1-th element is removed from its current location on 
** the stack and pushed back on top of the stack.  The
** top of the stack is element 0, so "Pull 0 0 0" is
** a no-op.  "Pull 1 0 0" swaps the top two elements of
** the stack.
**
** See also the Dup instruction.
*/
case OP_Pull: {
	mem_t *pFrom = &pTos[-pOp->p1];
	int i;
	mem_t ts;

	ts = *pFrom;
	if (__entity_ephem_to_dyn(pTos) == 1) {
		goto no_mem;
	}
	for (i = 0; i < pOp->p1; i++, pFrom++) {
		if (__entity_ephem_to_dyn(&pFrom[1]) == 1) {
			goto no_mem;
		}
		*pFrom = pFrom[1];
		DBSQL_ASSERT((pFrom->flags & MEM_Ephem) == 0);
		if (pFrom->flags & MEM_Short) {
			DBSQL_ASSERT(pFrom->flags & MEM_Str);
			DBSQL_ASSERT(pFrom->z == pFrom[1].zShort);
			pFrom->z = pFrom->zShort;
		}
	}
	*pTos = ts;
	if (pTos->flags & MEM_Short) {
		DBSQL_ASSERT(pTos->flags & MEM_Str);
		DBSQL_ASSERT(pTos->z == pTos[-pOp->p1].zShort);
		pTos->z = pTos->zShort;
	}
	break;
}

/* Opcode: Push P1 * *
**
** Overwrite the value of the P1-th element down on the
** stack (P1==0 is the top of the stack) with the value
** of the top of the stack.  Then pop the top of the stack.
*/
case OP_Push: {
	mem_t *pTo = &pTos[-pOp->p1];

	DBSQL_ASSERT(pTo >= p->aStack);
	if (__entity_ephem_to_dyn(pTos) == 1) {
		goto no_mem;
	}
	__entity_release_mem(pTo);
	*pTo = *pTos;
	if (pTo->flags & MEM_Short) {
		DBSQL_ASSERT(pTo->z == pTos->zShort);
		pTo->z = pTo->zShort;
	}
	pTos--;
	break;
}


/* Opcode: ColumnName P1 * P3
**
** P3 becomes the P1-th column name (first is 0).  An array of pointers
** to all column names is passed as the 4th parameter to the callback.
*/
case OP_ColumnName: {
	DBSQL_ASSERT(pOp->p1 >= 0 && pOp->p1 < p->nOp);
	p->azColName[pOp->p1] = pOp->p3;
	p->nCallback = 0;
	break;
}

/* Opcode: Callback P1 * *
**
** Pop P1 values off the stack and form them into an array.  Then
** invoke the callback function using the newly formed array as the
** 3rd parameter.
*/
case OP_Callback: {
	int i;
	char **azArgv = p->zArgv;
	mem_t *pCol;

	pCol = &pTos[1 - pOp->p1];
	DBSQL_ASSERT(pCol >= p->aStack);
	for (i = 0; i < pOp->p1; i++, pCol++) {
		if (pCol->flags & MEM_Null) {
			azArgv[i] = 0;
		} else {
			__entity_as_string(pCol);
			azArgv[i] = pCol->z;
		}
	}
	azArgv[i] = 0;
	if (p->xCallback == 0) {
		p->azResColumn = azArgv;
		p->nResColumn = pOp->p1;
		p->popStack = pOp->p1;
		p->pc = pc + 1;
		p->pTos = pTos;
		return DBSQL_ROW;
	}
	if (__safety_off(db))
		goto abort_due_to_misuse; 
	if (p->xCallback(p->pCbArg, pOp->p1, azArgv, p->azColName) != 0) {
		rc = DBSQL_ABORT;
	}
	if (__safety_on(db))
		goto abort_due_to_misuse;
	p->nCallback++;
	__pop_stack(&pTos, pOp->p1);
	DBSQL_ASSERT(pTos >= &p->aStack[-1]);
	break;
}

/* Opcode: NullCallback P1 * *
**
** Invoke the callback function once with the 2nd argument (the
** number of columns) equal to P1 and with the 4th argument (the
** names of the columns) set according to prior OP_ColumnName
** instructions.  This is all like the regular
** OP_Callback or OP_SortCallback opcodes.  But the 3rd argument
** which normally contains a pointer to an array of pointers to
** data is NULL.
**
** The callback is only invoked if there have been no prior calls
** to OP_Callback or OP_SortCallback.
**
** This opcode is used to report the number and names of columns
** in cases where the result set is empty.
*/
case OP_NullCallback: {
	if (p->nCallback == 0 && p->xCallback != 0) {
		if (__safety_off(db))
			goto abort_due_to_misuse; 
		if (p->xCallback(p->pCbArg, pOp->p1, 0, p->azColName) != 0) {
			rc = DBSQL_ABORT;
		}
		if (__safety_on(db))
			goto abort_due_to_misuse;
		p->nCallback++;
	}
	p->nResColumn = pOp->p1;
	break;
}

/* Opcode: Concat P1 P2 P3
**
** Look at the first P1 elements of the stack.  Append them all 
** together with the lowest element first.  Use P3 as a separator.  
** Put the result on the top of the stack.  The original P1 elements
** are popped from the stack if P2==0 and retained if P2==1.  If
** any element of the stack is NULL, then the result is NULL.
**
** If P3 is NULL, then use no separator.  When P1==1, this routine
** makes a copy of the top stack element into memory obtained
** from __dbsql_calloc().
*/
case OP_Concat: {
	char *zNew;
	int nByte;
	int nField;
	int i, j;
	char *zSep;
	int nSep;
	mem_t *pTerm;

	nField = pOp->p1;
	zSep = pOp->p3;
	if (zSep == 0)
		zSep = "";
	nSep = strlen(zSep);
	DBSQL_ASSERT(&pTos[1 - nField] >= p->aStack);
	nByte = 1 - nSep;
	pTerm = &pTos[1 - nField];
	for (i = 0; i < nField; i++, pTerm++) {
		if (pTerm->flags & MEM_Null) {
			nByte = -1;
			break;
		} else {
			__entity_as_string(pTerm);
			nByte += pTerm->n - 1 + nSep;
		}
	}
	if (nByte < 0) {
		if (pOp->p2 == 0) {
			__pop_stack(&pTos, nField);
		}
		pTos++;
		pTos->flags = MEM_Null;
		break;
	}
	if (__dbsql_calloc(NULL, 1, nByte, &zNew) == ENOMEM)
		goto no_mem;
	j = 0;
	pTerm = &pTos[1 - nField];
	for (i = j = 0; i < nField; i++, pTerm++) {
		DBSQL_ASSERT(pTerm->flags & MEM_Str);
		memcpy(&zNew[j], pTerm->z, (pTerm->n - 1));
		j += pTerm->n - 1;
		if (nSep > 0 && i < nField - 1) {
			memcpy(&zNew[j], zSep, nSep);
			j += nSep;
		}
	}
	zNew[j] = 0;
	if (pOp->p2 == 0) {
		__pop_stack(&pTos, nField);
	}
	pTos++;
	pTos->n = nByte;
	pTos->flags = MEM_Str | MEM_Dyn;
	pTos->z = zNew;
	break;
}

/* Opcode: Add * * *
**
** Pop the top two elements from the stack, add them together,
** and push the result back onto the stack.  If either element
** is a string then it is converted to a double using the atof()
** function before the addition.
** If either operand is NULL, the result is NULL.
*/
/* Opcode: Multiply * * *
**
** Pop the top two elements from the stack, multiply them together,
** and push the result back onto the stack.  If either element
** is a string then it is converted to a double using the atof()
** function before the multiplication.
** If either operand is NULL, the result is NULL.
*/
/* Opcode: Subtract * * *
**
** Pop the top two elements from the stack, subtract the
** first (what was on top of the stack) from the second (the
** next on stack)
** and push the result back onto the stack.  If either element
** is a string then it is converted to a double using the atof()
** function before the subtraction.
** If either operand is NULL, the result is NULL.
*/
/* Opcode: Divide * * *
**
** Pop the top two elements from the stack, divide the
** first (what was on top of the stack) from the second (the
** next on stack)
** and push the result back onto the stack.  If either element
** is a string then it is converted to a double using the atof()
** function before the division.  Division by zero returns NULL.
** If either operand is NULL, the result is NULL.
*/
/* Opcode: Remainder * * *
**
** Pop the top two elements from the stack, divide the
** first (what was on top of the stack) from the second (the
** next on stack)
** and push the remainder after division onto the stack.  If either element
** is a string then it is converted to a double using the atof()
** function before the division.  Division by zero returns NULL.
** If either operand is NULL, the result is NULL.
*/
case OP_Add:       /* FALLTHROUGH */
case OP_Subtract:  /* FALLTHROUGH */
case OP_Multiply:  /* FALLTHROUGH */
case OP_Divide:    /* FALLTHROUGH */
case OP_Remainder: {
	mem_t *pNos = &pTos[-1];
	DBSQL_ASSERT(pNos >= p->aStack);
	if (((pTos->flags | pNos->flags) & MEM_Null) != 0) {
		__entity_release_mem(pTos);
		pTos--;
		__entity_release_mem(pTos);
		pTos->flags = MEM_Null;
	} else if ((pTos->flags & pNos->flags & MEM_Int) == MEM_Int) {
		int a, b;
		a = pTos->i;
		b = pNos->i;
		switch(pOp->opcode) {
		case OP_Add:         b += a;       break;
		case OP_Subtract:    b -= a;       break;
		case OP_Multiply:    b *= a;       break;
		case OP_Divide: {
			if (a == 0) goto divide_by_zero;
			b /= a;
			break;
		}
		default: {
			if (a == 0) goto divide_by_zero;
			b %= a;
			break;
		}
		}
		__entity_release_mem(pTos);
		pTos--;
		__entity_release_mem(pTos);
		pTos->i = b;
		pTos->flags = MEM_Int;
	} else {
		double a, b;
		__entity_to_real(pTos);
		__entity_to_real(pNos);
		a = pTos->r;
		b = pNos->r;
		switch( pOp->opcode ){
		case OP_Add:         b += a;       break;
		case OP_Subtract:    b -= a;       break;
		case OP_Multiply:    b *= a;       break;
		case OP_Divide: {
			if (a == 0.0) goto divide_by_zero;
			b /= a;
			break;
		}
		default: {
			int ia = (int)a;
			int ib = (int)b;
			if (ia == 0.0) goto divide_by_zero;
			b = ib % ia;
			break;
		}
		}
		__entity_release_mem(pTos);
		pTos--;
		__entity_release_mem(pTos);
		pTos->r = b;
		pTos->flags = MEM_Real;
	}
	break;

divide_by_zero:
	__entity_release_mem(pTos);
	pTos--;
	__entity_release_mem(pTos);
	pTos->flags = MEM_Null;
	break;
}

/* Opcode: Function P1 * P3
**
** Invoke a user function (P3 is a pointer to a Function structure that
** defines the function) with P1 string arguments taken from the stack.
** Pop all arguments from the stack and push back the result.
**
** See also: AggFunc
*/
case OP_Function: {
	int n, i;
	mem_t *pArg;
	char **azArgv;
	dbsql_func_t ctx;

	n = pOp->p1;
	pArg = &pTos[1-n];
	azArgv = p->zArgv;
	for (i = 0; i < n; i++, pArg++) {
		if (pArg->flags & MEM_Null) {
			azArgv[i] = 0;
		} else {
			__entity_as_string(pArg);
			azArgv[i] = pArg->z;
		}
	}
	ctx.pFunc = (func_def_t*)pOp->p3;
	ctx.s.flags = MEM_Null;
	ctx.s.z = 0;
	ctx.isError = 0;
	ctx.isStep = 0;
	if (__safety_off(db))
		goto abort_due_to_misuse;
	(*ctx.pFunc->xFunc)(&ctx, n, (const char**)azArgv);
	if (__safety_on(db))
		goto abort_due_to_misuse;
	__pop_stack(&pTos, n);
	pTos++;
	*pTos = ctx.s;
	if (pTos->flags & MEM_Short) {
		pTos->z = pTos->zShort;
	}
	if (ctx.isError) {
		__str_append(&p->zErrMsg, ((pTos->flags & MEM_Str) != 0) ?
			     pTos->z : "user function error", (char*)0);
		rc = DBSQL_ERROR;
	}
	break;
}

/* Opcode: BitAnd * * *
**
** Pop the top two elements from the stack.  Convert both elements
** to integers.  Push back onto the stack the bit-wise AND of the
** two elements.
** If either operand is NULL, the result is NULL.
*/
/* Opcode: BitOr * * *
**
** Pop the top two elements from the stack.  Convert both elements
** to integers.  Push back onto the stack the bit-wise OR of the
** two elements.
** If either operand is NULL, the result is NULL.
*/
/* Opcode: ShiftLeft * * *
**
** Pop the top two elements from the stack.  Convert both elements
** to integers.  Push back onto the stack the top element shifted
** left by N bits where N is the second element on the stack.
** If either operand is NULL, the result is NULL.
*/
/* Opcode: ShiftRight * * *
**
** Pop the top two elements from the stack.  Convert both elements
** to integers.  Push back onto the stack the top element shifted
** right by N bits where N is the second element on the stack.
** If either operand is NULL, the result is NULL.
*/
case OP_BitAnd:    /* FALLTHROUGH */
case OP_BitOr:     /* FALLTHROUGH */
case OP_ShiftLeft: /* FALLTHROUGH */
case OP_ShiftRight: {
	mem_t *pNos = &pTos[-1];
	int a, b;

	DBSQL_ASSERT(pNos >= p->aStack);
	if ((pTos->flags | pNos->flags) & MEM_Null) {
		__pop_stack(&pTos, 2);
		pTos++;
		pTos->flags = MEM_Null;
		break;
	}
	__entity_to_int(pTos);
	__entity_to_int(pNos);
	a = pTos->i;
	b = pNos->i;
	switch(pOp->opcode) {
	case OP_BitAnd:      a &= b;     break;
	case OP_BitOr:       a |= b;     break;
	case OP_ShiftLeft:   a <<= b;    break;
	case OP_ShiftRight:  a >>= b;    break;
	default:   /* CANT HAPPEN */     break;
	}
	DBSQL_ASSERT((pTos->flags & MEM_Dyn) == 0);
	DBSQL_ASSERT((pNos->flags & MEM_Dyn) == 0);
	pTos--;
	pTos->i = a;
	DBSQL_ASSERT(pTos->flags == MEM_Int);
	break;
}

/* Opcode: AddImm  P1 * *
** 
** Add the value P1 to whatever is on top of the stack.  The result
** is always an integer.
**
** To force the top of the stack to be an integer, just add 0.
*/
case OP_AddImm: {
	DBSQL_ASSERT(pTos >= p->aStack);
	__entity_to_int(pTos);
	pTos->i += pOp->p1;
	break;
}

/* Opcode: ForceInt P1 P2 *
**
** Convert the top of the stack into an integer.  If the current top of
** the stack is not numeric (meaning that is is a NULL or a string that
** does not look like an integer or floating point number) then pop the
** stack and jump to P2.  If the top of the stack is numeric then
** convert it into the least integer that is greater than or equal to its
** current value if P1==0, or to the least integer that is strictly
** greater than its current value if P1==1.
*/
case OP_ForceInt: {
	int v;
	DBSQL_ASSERT(pTos >= p->aStack);
	if ((pTos->flags & (MEM_Int | MEM_Real)) == 0 &&
	    ((pTos->flags & MEM_Str) == 0 || __str_is_numeric(pTos->z) ==0)) {
		__entity_release_mem(pTos);
		pTos--;
		pc = pOp->p2 - 1;
		break;
	}
	if (pTos->flags & MEM_Int) {
		v = pTos->i + (pOp->p1 != 0);
	} else {
		__entity_to_real(pTos);
		v = (int)pTos->r;
		if (pTos->r>(double)v)
			v++;
		if (pOp->p1 && pTos->r == (double)v)
			v++;
	}
	__entity_release_mem(pTos);
	pTos->i = v;
	pTos->flags = MEM_Int;
	break;
}

/* Opcode: MustBeInt P1 P2 *
** 
** Force the top of the stack to be an integer.  If the top of the
** stack is not an integer and cannot be converted into an integer
** with out data loss, then jump immediately to P2, or if P2==0
** raise an DBSQL_MISMATCH exception.
**
** If the top of the stack is not an integer and P2 is not zero and
** P1 is 1, then the stack is popped.  In all other cases, the depth
** of the stack is unchanged.
*/
case OP_MustBeInt: {
	DBSQL_ASSERT(pTos >= p->aStack);
	if (pTos->flags & MEM_Int) {
		/* Do nothing */
	} else if (pTos->flags & MEM_Real) {
		int i = (int)pTos->r;
		double r = (double)i;
		if (r != pTos->r) {
			goto mismatch;
		}
		pTos->i = i;
	} else if (pTos->flags & MEM_Str) {
		int v;
		if (!__dbsql_atoi(pTos->z, &v)) {
			double r;
			if (!__str_is_numeric(pTos->z)) {
				goto mismatch;
			}
			__entity_to_real(pTos);
			v = (int)pTos->r;
			r = (double)v;
			if (r != pTos->r) {
				goto mismatch;
			}
		}
		pTos->i = v;
	} else {
		goto mismatch;
	}
	__entity_release_mem(pTos);
	pTos->flags = MEM_Int;
	break;

mismatch:
	if (pOp->p2 == 0) {
		rc = DBSQL_MISMATCH;
		goto abort_due_to_error;
	} else {
		if (pOp->p1)
			__pop_stack(&pTos, 1);
		pc = pOp->p2 - 1;
	}
	break;
}

/* Opcode: Eq P1 P2 *
**
** Pop the top two elements from the stack.  If they are equal, then
** jump to instruction P2.  Otherwise, continue to the next instruction.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** If both values are numeric, they are converted to doubles using atof()
** and compared for equality that way.  Otherwise the strcmp() library
** routine is used for the comparison.  For a pure text comparison
** use OP_StrEq.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: Ne P1 P2 *
**
** Pop the top two elements from the stack.  If they are not equal, then
** jump to instruction P2.  Otherwise, continue to the next instruction.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** If both values are numeric, they are converted to doubles using atof()
** and compared in that format.  Otherwise the strcmp() library
** routine is used for the comparison.  For a pure text comparison
** use OP_StrNe.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: Lt P1 P2 *
**
** Pop the top two elements from the stack.  If second element (the
** next on stack) is less than the first (the top of stack), then
** jump to instruction P2.  Otherwise, continue to the next instruction.
** In other words, jump if NOS<TOS.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** If both values are numeric, they are converted to doubles using atof()
** and compared in that format.  Numeric values are always less than
** non-numeric values.  If both operands are non-numeric, the strcmp() library
** routine is used for the comparison.  For a pure text comparison
** use OP_StrLt.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: Le P1 P2 *
**
** Pop the top two elements from the stack.  If second element (the
** next on stack) is less than or equal to the first (the top of stack),
** then jump to instruction P2. In other words, jump if NOS<=TOS.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** If both values are numeric, they are converted to doubles using atof()
** and compared in that format.  Numeric values are always less than
** non-numeric values.  If both operands are non-numeric, the strcmp() library
** routine is used for the comparison.  For a pure text comparison
** use OP_StrLe.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: Gt P1 P2 *
**
** Pop the top two elements from the stack.  If second element (the
** next on stack) is greater than the first (the top of stack),
** then jump to instruction P2. In other words, jump if NOS>TOS.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** If both values are numeric, they are converted to doubles using atof()
** and compared in that format.  Numeric values are always less than
** non-numeric values.  If both operands are non-numeric, the strcmp() library
** routine is used for the comparison.  For a pure text comparison
** use OP_StrGt.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: Ge P1 P2 *
**
** Pop the top two elements from the stack.  If second element (the next
** on stack) is greater than or equal to the first (the top of stack),
** then jump to instruction P2. In other words, jump if NOS>=TOS.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** If both values are numeric, they are converted to doubles using atof()
** and compared in that format.  Numeric values are always less than
** non-numeric values.  If both operands are non-numeric, the strcmp() library
** routine is used for the comparison.  For a pure text comparison
** use OP_StrGe.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
case OP_Eq: /* FALLTHROUGH */
case OP_Ne: /* FALLTHROUGH */
case OP_Lt: /* FALLTHROUGH */
case OP_Le: /* FALLTHROUGH */
case OP_Gt: /* FALLTHROUGH */
case OP_Ge: {
	mem_t *pNos = &pTos[-1];
	int c, v;
	int ft, fn;
	DBSQL_ASSERT(pNos >= p->aStack);
	ft = pTos->flags;
	fn = pNos->flags;
	if ((ft | fn) & MEM_Null) {
		__pop_stack(&pTos, 2);
		if (pOp->p2) {
			if (pOp->p1)
				pc = pOp->p2-1;
		} else {
			pTos++;
			pTos->flags = MEM_Null;
		}
		break;
	} else if ((ft & fn & MEM_Int) == MEM_Int) {
		c = pNos->i - pTos->i;
	} else if ((ft & MEM_Int) != 0 && (fn & MEM_Str) !=0
		   && __dbsql_atoi(pNos->z,&v)) {
		c = v - pTos->i;
	} else if ((fn & MEM_Int) != 0 && (ft & MEM_Str) !=0 &&
		   __dbsql_atoi(pTos->z,&v)) {
		c = pNos->i - v;
	} else {
		__entity_as_string(pTos);
		__entity_as_string(pNos);
		c = __str_numeric_cmp(pNos->z, pTos->z);
	}
	switch(pOp->opcode) {
	case OP_Eq:    c = (c == 0);     break;
	case OP_Ne:    c = (c != 0);     break;
	case OP_Lt:    c = (c <  0);     break;
	case OP_Le:    c = (c <= 0);     break;
	case OP_Gt:    c = (c >  0);     break;
	default:       c = (c >= 0);     break;
	}
	__pop_stack(&pTos, 2);
	if (pOp->p2) {
		if (c)
			pc = pOp->p2 - 1;
	} else {
		pTos++;
		pTos->i = c;
		pTos->flags = MEM_Int;
	}
	break;
}

/* !!!
** WARNING, INSERT NO CODE AT THIS POINT!
**
** The opcode numbers are extracted from this source file by doing
**
**    grep '^case OP_' vdbe.c | ... >opcodes.h
**
** The opcodes are numbered in the order that they appear in this file.
** But in order for the expression generating code to work right, the
** string comparison operators that follow must be numbered exactly 6
** greater than the numeric comparison opcodes above.  So no other
** cases can appear between the two.
** !!!
*/

/* Opcode: StrEq P1 P2 *
**
** Pop the top two elements from the stack.  If they are equal, then
** jump to instruction P2.  Otherwise, continue to the next instruction.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** The strcmp() library routine is used for the comparison.  For a
** numeric comparison, use OP_Eq.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: StrNe P1 P2 *
**
** Pop the top two elements from the stack.  If they are not equal, then
** jump to instruction P2.  Otherwise, continue to the next instruction.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** The strcmp() library routine is used for the comparison.  For a
** numeric comparison, use OP_Ne.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: StrLt P1 P2 *
**
** Pop the top two elements from the stack.  If second element (the
** next on stack) is less than the first (the top of stack), then
** jump to instruction P2.  Otherwise, continue to the next instruction.
** In other words, jump if NOS<TOS.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** The strcmp() library routine is used for the comparison.  For a
** numeric comparison, use OP_Lt.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: StrLe P1 P2 *
**
** Pop the top two elements from the stack.  If second element (the
** next on stack) is less than or equal to the first (the top of stack),
** then jump to instruction P2. In other words, jump if NOS<=TOS.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** The strcmp() library routine is used for the comparison.  For a
** numeric comparison, use OP_Le.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: StrGt P1 P2 *
**
** Pop the top two elements from the stack.  If second element (the
** next on stack) is greater than the first (the top of stack),
** then jump to instruction P2. In other words, jump if NOS>TOS.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** The strcmp() library routine is used for the comparison.  For a
** numeric comparison, use OP_Gt.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
/* Opcode: StrGe P1 P2 *
**
** Pop the top two elements from the stack.  If second element (the next
** on stack) is greater than or equal to the first (the top of stack),
** then jump to instruction P2. In other words, jump if NOS>=TOS.
**
** If either operand is NULL (and thus if the result is unknown) then
** take the jump if P1 is true.
**
** The strcmp() library routine is used for the comparison.  For a
** numeric comparison, use OP_Ge.
**
** If P2 is zero, do not jump.  Instead, push an integer 1 onto the
** stack if the jump would have been taken, or a 0 if not.  Push a
** NULL if either operand was NULL.
*/
case OP_StrEq: /* FALLTHROUGH */
case OP_StrNe: /* FALLTHROUGH */
case OP_StrLt: /* FALLTHROUGH */
case OP_StrLe: /* FALLTHROUGH */
case OP_StrGt: /* FALLTHROUGH */
case OP_StrGe: {
	mem_t *pNos = &pTos[-1];
	int c;
	DBSQL_ASSERT(pNos >= p->aStack);
	if ((pNos->flags | pTos->flags) & MEM_Null) {
		__pop_stack(&pTos, 2);
		if (pOp->p2) {
			if (pOp->p1)
				pc = (pOp->p2 - 1);
		} else {
			pTos++;
			pTos->flags = MEM_Null;
		}
		break;
	} else {
		__entity_as_string(pTos);
		__entity_as_string(pNos);
		c = strcmp(pNos->z, pTos->z);
	}
	/* !!!
	 * The DBSQL_ASSERTs on each case of the following switch are there to verify
	 * that string comparison opcodes are always exactly 6 greater than the
	 * corresponding numeric comparison opcodes.  The code generator
	 * depends on this fact.
	 * !!!
	 */
	switch(pOp->opcode) {
	case OP_StrEq:
		c = (c == 0);
		DBSQL_ASSERT(pOp->opcode-6 == OP_Eq);
		break;
	case OP_StrNe:
		c = (c != 0);
		DBSQL_ASSERT(pOp->opcode-6 == OP_Ne);
		break;
	case OP_StrLt:
		c = (c < 0);
		DBSQL_ASSERT(pOp->opcode-6 == OP_Lt);
		break;
	case OP_StrLe:
		c = (c <= 0);
		DBSQL_ASSERT(pOp->opcode-6 == OP_Le);
		break;
	case OP_StrGt:
		c = (c > 0);
		DBSQL_ASSERT(pOp->opcode-6 == OP_Gt);
		break;
	default:
		c = (c >= 0);
		DBSQL_ASSERT(pOp->opcode-6 == OP_Ge);
		break;
	}
	__pop_stack(&pTos, 2);
	if (pOp->p2) {
		if (c)
			pc = (pOp->p2 - 1);
	} else {
		pTos++;
		pTos->flags = MEM_Int;
		pTos->i = c;
	}
	break;
}

/* Opcode: And * * *
**
** Pop two values off the stack.  Take the logical AND of the
** two values and push the resulting boolean value back onto the
** stack. 
*/
/* Opcode: Or * * *
**
** Pop two values off the stack.  Take the logical OR of the
** two values and push the resulting boolean value back onto the
** stack. 
*/
case OP_And: /* FALLTHROUGH */
case OP_Or: {
	mem_t *pNos = &pTos[-1];
	int v1, v2;    /* 0==TRUE, 1==FALSE, 2==UNKNOWN or NULL */

	DBSQL_ASSERT(pNos >= p->aStack);
	if (pTos->flags & MEM_Null) {
		v1 = 2;
	} else {
		__entity_to_int(pTos);
		v1 = pTos->i == 0;
	}
	if (pNos->flags & MEM_Null) {
		v2 = 2;
	} else {
		__entity_to_int(pNos);
		v2 = pNos->i == 0;
	}
	if (pOp->opcode == OP_And) {
		static const unsigned char and_logic[] =
			{ 0, 1, 2, 1, 1, 1, 2, 1, 2 };
		v1 = and_logic[(v1 * 3) + v2];
	} else {
		static const unsigned char or_logic[] =
			{ 0, 0, 0, 0, 1, 2, 0, 2, 2 };
		v1 = or_logic[(v1 * 3) + v2];
	}
	__pop_stack(&pTos, 2);
	pTos++;
	if (v1 == 2) {
		pTos->flags = MEM_Null;
	} else {
		pTos->i = v1==0;
		pTos->flags = MEM_Int;
	}
	break;
}

/* Opcode: Negative * * *
**
** Treat the top of the stack as a numeric quantity.  Replace it
** with its additive inverse.  If the top of the stack is NULL
** its value is unchanged.
*/
/* Opcode: AbsValue * * *
**
** Treat the top of the stack as a numeric quantity.  Replace it
** with its absolute value. If the top of the stack is NULL
** its value is unchanged.
*/
case OP_Negative: /* FALLTHROUGH */
case OP_AbsValue: {
	DBSQL_ASSERT(pTos >= p->aStack);
	if (pTos->flags & MEM_Real) {
		__entity_release_mem(pTos);
		if (pOp->opcode == OP_Negative || pTos->r < 0.0) {
			pTos->r = -pTos->r;
		}
		pTos->flags = MEM_Real;
	} else if (pTos->flags & MEM_Int) {
		__entity_release_mem(pTos);
		if (pOp->opcode == OP_Negative || pTos->i < 0) {
			pTos->i = -pTos->i;
		}
		pTos->flags = MEM_Int;
	} else if (pTos->flags & MEM_Null) {
		/* Do nothing */
	} else {
		__entity_to_real(pTos);
		__entity_release_mem(pTos);
		if (pOp->opcode == OP_Negative || pTos->r < 0.0) {
			pTos->r = -pTos->r;
		}
		pTos->flags = MEM_Real;
	}
	break;
}

/* Opcode: Not * * *
**
** Interpret the top of the stack as a boolean value.  Replace it
** with its complement.  If the top of the stack is NULL its value
** is unchanged.
*/
case OP_Not: {
	DBSQL_ASSERT(pTos >= p->aStack);
	if (pTos->flags & MEM_Null)
		break;  /* Do nothing to NULLs */
	__entity_to_int(pTos);
	DBSQL_ASSERT(pTos->flags == MEM_Int);
	pTos->i = !pTos->i;
	break;
}

/* Opcode: BitNot * * *
**
** Interpret the top of the stack as an value.  Replace it
** with its ones-complement.  If the top of the stack is NULL its
** value is unchanged.
*/
case OP_BitNot: {
	DBSQL_ASSERT(pTos >= p->aStack);
	if (pTos->flags & MEM_Null)
		break;  /* Do nothing to NULLs */
	__entity_to_int(pTos);
	DBSQL_ASSERT(pTos->flags == MEM_Int);
	pTos->i = ~pTos->i;
	break;
}

/* Opcode: Noop * * *
**
** Do nothing.  This instruction is often useful as a jump
** destination.
*/
case OP_Noop: {
  break;
}

/* Opcode: If P1 P2 *
**
** Pop a single boolean from the stack.  If the boolean popped is
** true, then jump to P2.  Otherwise continue to the next instruction.
** An integer is false if zero and true otherwise.  A string is
** false if it has zero length and true otherwise.
**
** If the value popped of the stack is NULL, then take the jump if P1
** is true and fall through if P1 is false.
*/
/* Opcode: IfNot P1 P2 *
**
** Pop a single boolean from the stack.  If the boolean popped is
** false, then jump to p2.  Otherwise continue to the next instruction.
** An integer is false if zero and true otherwise.  A string is
** false if it has zero length and true otherwise.
**
** If the value popped of the stack is NULL, then take the jump if P1
** is true and fall through if P1 is false.
*/
case OP_If: /* FALLTHROUGH */
case OP_IfNot: {
	int c;
	DBSQL_ASSERT(pTos >= p->aStack);
	if (pTos->flags & MEM_Null) {
		c = pOp->p1;
	} else {
		__entity_to_int(pTos);
		c = pTos->i;
		if (pOp->opcode == OP_IfNot)
			c = !c;
	}
	DBSQL_ASSERT((pTos->flags & MEM_Dyn) == 0);
	pTos--;
	if (c)
		pc = pOp->p2-1;
	break;
}

/* Opcode: IsNull P1 P2 *
**
** If any of the top abs(P1) values on the stack are NULL, then jump
** to P2.  Pop the stack P1 times if P1>0.   If P1<0 leave the stack
** unchanged.
*/
case OP_IsNull: {
	int i, cnt;
	mem_t *pTerm;
	cnt = pOp->p1;
	if (cnt < 0)
		cnt = -cnt;
	pTerm = &pTos[1-cnt];
	DBSQL_ASSERT(pTerm >= p->aStack);
	for (i = 0; i < cnt; i++, pTerm++) {
		if (pTerm->flags & MEM_Null) {
			pc = pOp->p2-1;
			break;
		}
	}
	if (pOp->p1 > 0)
		__pop_stack(&pTos, cnt);
	break;
}

/* Opcode: NotNull P1 P2 *
**
** Jump to P2 if the top P1 values on the stack are all not NULL.  Pop the
** stack if P1 times if P1 is greater than zero.  If P1 is less than
** zero then leave the stack unchanged.
*/
case OP_NotNull: {
	int i, cnt;
	cnt = pOp->p1;
	if (cnt < 0)
		cnt = -cnt;
	DBSQL_ASSERT(&pTos[1 - cnt] >= p->aStack);
	i = 0;
	while(i < cnt && (pTos[1 + i - cnt].flags & MEM_Null) == 0) {
		i++;
	}
	if (i >= cnt )
		pc = pOp->p2 - 1;
	if (pOp->p1 > 0)
		__pop_stack(&pTos, cnt);
	break;
}

/* Opcode: MakeRecord P1 P2 *
**
** Convert the top P1 entries of the stack into a single entry
** suitable for use as a data record in a database table.  The
** details of the format are irrelavant as long as the OP_Column
** opcode can decode the record later.  Refer to source code
** comments for the details of the record format.
**
** If P2 is true (non-zero) and one or more of the P1 entries
** that go into building the record is NULL, then add some extra
** bytes to the record to make it distinct for other entries created
** during the same run of the VDBE.  The extra bytes added are a
** counter that is reset with each run of the VDBE, so records
** created this way will not necessarily be distinct across runs.
** But they should be distinct for transient tables (created using
** OP_OpenTemp) which is what they are intended for.
**
** (Later:) The P2==1 option was intended to make NULLs distinct
** for the UNION operator.  But I have since discovered that NULLs
** are indistinct for UNION.  So this option is never used.
*/
case OP_MakeRecord: {
	char *zNewRecord;
	int nByte;
	int nField;
	int i, j;
	int idxWidth;
	u_int32_t addr;
	mem_t *pRec;
	int addUnique = 0;   /* True to cause bytes to be added to make the
			      * generated record distinct */
	char zTemp[NBFS];    /* Temp space for small records */

	/* Assuming the record contains N fields, the record format looks
	 * like this:
	 *
	 *  -------------------------------------------------------------------
	 *  | idx0 | idx1 | ... | idx(N-1) | idx(N) | data0 | ... | data(N-1) |
	 *  -------------------------------------------------------------------
	 *
	 * All data fields are converted to strings before being stored and
	 * are stored with their null terminators.  NULL entries omit the
	 * null terminator.  Thus an empty string uses 1 byte and a NULL uses
	 * zero bytes.  Data(0) is taken from the lowest element of the stack
	 * and data(N-1) is the top of the stack.
	 *
	 * Each of the idx() entries is either 1, 2, or 3 bytes depending on
	 * how big the total record is.  Idx(0) contains the offset to the
	 * start of data(0).  Idx(k) contains the offset to the start of
	 * data(k).
	 * Idx(N) contains the total number of bytes in the record.
	 */
	nField = pOp->p1;
	pRec = &pTos[1 - nField];
	DBSQL_ASSERT(pRec >= p->aStack);
	nByte = 0;
	for (i = 0; i < nField; i++, pRec++) {
		if (pRec->flags & MEM_Null) {
			addUnique = pOp->p2;
		} else {
			__entity_as_string(pRec);
			nByte += pRec->n;
		}
	}
	if (addUnique)
		nByte += sizeof(p->uniqueCnt);
	if (nByte + nField + 1 < 256) {
		idxWidth = 1;
	} else if (nByte + (2 * nField) + 2 < 65536) {
		idxWidth = 2;
	} else {
		idxWidth = 3;
	}
	nByte += idxWidth * (nField + 1);
	if (nByte <= NBFS) {
		zNewRecord = zTemp;
	} else {
		if (__dbsql_calloc(NULL, 1, nByte, &zNewRecord) == ENOMEM)
			goto no_mem;
	}
	j = 0;
	addr = idxWidth * (nField + 1) + addUnique * sizeof(p->uniqueCnt);
	for (i = 0, pRec = &pTos[1 - nField]; i < nField; i++, pRec++) {
		zNewRecord[j++] = addr & 0xff;
		if (idxWidth > 1) {
			zNewRecord[j++] = (addr >> 8) & 0xff;
			if (idxWidth > 2) {
				zNewRecord[j++] = (addr >> 16) & 0xff;
			}
		}
		if ((pRec->flags & MEM_Null) == 0) {
			addr += pRec->n;
		}
	}
	zNewRecord[j++] = addr & 0xff;
	if (idxWidth > 1) {
		zNewRecord[j++] = (addr >> 8) & 0xff;
		if (idxWidth > 2) {
			zNewRecord[j++] = (addr >> 16) & 0xff;
		}
	}
	if (addUnique) {
		memcpy(&zNewRecord[j], &p->uniqueCnt, sizeof(p->uniqueCnt));
		p->uniqueCnt++;
		j += sizeof(p->uniqueCnt);
	}
	for (i = 0, pRec = &pTos[1 - nField]; i < nField; i++, pRec++) {
		if ((pRec->flags & MEM_Null) == 0) {
			memcpy(&zNewRecord[j], pRec->z, pRec->n);
			j += pRec->n;
		}
	}
	__pop_stack(&pTos, nField);
	pTos++;
	pTos->n = nByte;
	if (nByte <= NBFS) {
		DBSQL_ASSERT(zNewRecord == zTemp);
		memcpy(pTos->zShort, zTemp, nByte);
		pTos->z = pTos->zShort;
		pTos->flags = MEM_Str | MEM_Short;
	} else {
		DBSQL_ASSERT(zNewRecord != zTemp);
		pTos->z = zNewRecord;
		pTos->flags = MEM_Str | MEM_Dyn;
	}
	break;
}

/* Opcode: MakeKey P1 P2 P3
**
** Convert the top P1 entries of the stack into a single entry suitable
** for use as the key in an index.  The top P1 records are
** converted to strings and merged.  The null-terminators 
** are retained and used as separators.
** The lowest entry in the stack is the first field and the top of the
** stack becomes the last.
**
** If P2 is not zero, then the original entries remain on the stack
** and the new key is pushed on top.  If P2 is zero, the original
** data is popped off the stack first then the new key is pushed
** back in its place.
**
** P3 is a string that is P1 characters long.  Each character is either
** an 'n' or a 't' to indicates if the argument should be intepreted as
** numeric or text type.  The first character of P3 corresponds to the
** lowest element on the stack.  If P3 is NULL then all arguments are
** assumed to be of the numeric type.
**
** The type makes a difference in that text-type fields may not be 
** introduced by 'b' (as described in the next paragraph).  The
** first character of a text-type field must be either 'a' (if it is NULL)
** or 'c'.  Numeric fields will be introduced by 'b' if their content
** looks like a well-formed number.  Otherwise the 'a' or 'c' will be
** used.
**
** The key is a concatenation of fields.  Each field is terminated by
** a single 0x00 character.  A NULL field is introduced by an 'a' and
** is followed immediately by its 0x00 terminator.  A numeric field is
** introduced by a single character 'b' and is followed by a sequence
** of characters that represent the number such that a comparison of
** the character string using memcpy() sorts the numbers in numerical
** order.  The character strings for numbers are generated using the
** __str_real_as_sortable() function.  A text field is introduced by a
** 'c' character and is followed by the exact text of the field.  The
** use of an 'a', 'b', or 'c' character at the beginning of each field
** guarantees that NULLs sort before numbers and that numbers sort
** before text.  0x00 characters do not occur except as separators
** between fields.
**
** See also: MakeIdxKey, SortMakeKey
*/
/* Opcode: MakeIdxKey P1 P2 P3
**
** Convert the top P1 entries of the stack into a single entry suitable
** for use as the key in an index.  In addition, take one additional integer
** off of the stack, treat that integer as a four-byte record number, and
** append the four bytes to the key.  Thus a total of P1+1 entries are
** popped from the stack for this instruction and a single entry is pushed
** back.  The first P1 entries that are popped are strings and the last
** entry (the lowest on the stack) is an integer record number.
**
** The converstion of the first P1 string entries occurs just like in
** MakeKey.  Each entry is separated from the others by a null.
** The entire concatenation is null-terminated.  The lowest entry
** in the stack is the first field and the top of the stack becomes the
** last.
**
** If P2 is not zero and one or more of the P1 entries that go into the
** generated key is NULL, then jump to P2 after the new key has been
** pushed on the stack.  In other words, jump to P2 if the key is
** guaranteed to be unique.  This jump can be used to skip a subsequent
** uniqueness test.
**
** P3 is a string that is P1 characters long.  Each character is either
** an 'n' or a 't' to indicates if the argument should be numeric or
** text.  The first character corresponds to the lowest element on the
** stack.  If P3 is null then all arguments are assumed to be numeric.
**
** See also:  MakeKey, SortMakeKey
*/
case OP_MakeIdxKey: /* FALLTHROUGH */
case OP_MakeKey: {
	char *zNewKey;
	int nByte;
	int nField;
	int addRowid;
	int i, j;
	int containsNull = 0;
	mem_t *pRec;
	char zTemp[NBFS];

	addRowid = (pOp->opcode == OP_MakeIdxKey);
	nField = pOp->p1;
	pRec = &pTos[1 - nField];
	DBSQL_ASSERT(pRec >= p->aStack);
	nByte = 0;
	for (j = 0, i = 0; i < nField; i++, j++, pRec++) {
		int flags = pRec->flags;
		int len;
		char *z;
		if (flags & MEM_Null) {
			nByte += 2;
			containsNull = 1;
		} else if (pOp->p3 && pOp->p3[j] == 't') {
			__entity_as_string(pRec);
			pRec->flags &= ~(MEM_Int | MEM_Real);
			nByte += (pRec->n + 1);
		} else if ((flags & (MEM_Real | MEM_Int)) != 0 ||
			   __str_is_numeric(pRec->z)) {
			if ((flags & (MEM_Real | MEM_Int)) == MEM_Int) {
				pRec->r = pRec->i;
			} else if ((flags & (MEM_Real | MEM_Int)) == 0) {
				pRec->r = __dbsql_atof(pRec->z);
			}
			__entity_release_mem(pRec);
			z = pRec->zShort;
			__str_real_as_sortable(pRec->r, z);
			len = strlen(z);
			pRec->z = 0;
			pRec->flags = MEM_Real;
			pRec->n = len+1;
			nByte += (pRec->n + 1);
		} else {
			nByte += (pRec->n + 1);
		}
	}
	if (addRowid)
		nByte += sizeof(u_int32_t);
	if (nByte <= NBFS) {
		zNewKey = zTemp;
	} else {
		if (__dbsql_calloc(NULL, 1, nByte, &zNewKey) == ENOMEM)
			goto no_mem;
	}
	j = 0;
	pRec = &pTos[1 - nField];
	for (i = 0; i < nField; i++, pRec++) {
		if (pRec->flags & MEM_Null) {
			zNewKey[j++] = 'a';
			zNewKey[j++] = 0;
		} else if (pRec->flags == MEM_Real) {
			zNewKey[j++] = 'b';
			memcpy(&zNewKey[j], pRec->zShort, pRec->n);
			j += pRec->n;
		} else {
			DBSQL_ASSERT(pRec->flags & MEM_Str);
			zNewKey[j++] = 'c';
			memcpy(&zNewKey[j], pRec->z, pRec->n);
			j += pRec->n;
		}
	}
	if (addRowid) {
		u_int32_t iKey;
		pRec = &pTos[-nField];
		DBSQL_ASSERT(pRec >= p->aStack);
		__entity_to_int(pRec);
		iKey = INT_TO_KEY(pRec->i);
		memcpy(&zNewKey[j], &iKey, sizeof(u_int32_t));
		__pop_stack(&pTos, (nField + 1));
		if (pOp->p2 && containsNull)
			pc = pOp->p2 - 1;
	} else {
		if (pOp->p2 == 0)
			__pop_stack(&pTos, nField);
	}
	pTos++;
	pTos->n = nByte;
	if (nByte <= NBFS) {
		DBSQL_ASSERT(zNewKey == zTemp);
		pTos->z = pTos->zShort;
		memcpy(pTos->zShort, zTemp, nByte);
		pTos->flags = MEM_Str | MEM_Short;
	} else {
		pTos->z = zNewKey;
		pTos->flags = MEM_Str | MEM_Dyn;
	}
	break;
}

/* Opcode: IncrKey * * *
**
** The top of the stack should contain an index key generated by
** The MakeKey opcode.  This routine increases the least significant
** byte of that key by one.  This is used so that the MoveTo opcode
** will move to the first entry greater than the key rather than to
** the key itself.
*/
case OP_IncrKey: {
	DBSQL_ASSERT(pTos >= p->aStack);
	/*
	 * The IncrKey opcode is only applied to keys generated by
	 * MakeKey or MakeIdxKey and the results of those operands
	 * are always dynamic strings or zShort[] strings.  So we
	 * are always free to modify the string in place.
	 */
	DBSQL_ASSERT(pTos->flags & (MEM_Dyn | MEM_Short));
	pTos->z[pTos->n-1]++;
	break;
}

/* Opcode: Checkpoint P1 * *
**
** A checkpoint will bound recovery time.
** Begin a checkpoint.  A checkpoint is the beginning of a operation that
** is part of a larger transaction but which might need to be rolled back
** itself without effecting the containing transaction.  A checkpoint will
** automatically either be committed or rolled back (aborted) when the VDBE
** halts.
**
** The checkpoint is begun on the database file with index P1.  The main
** database file has an index of 0 and the file used for temporary tables
** has an index of 1.
*/
case OP_Checkpoint: {
	int i = pOp->p1;
	if (i >= 0 && i < db->nDb && db->aDb[i].pBt) {
		rc = __sm_checkpoint(db->aDb[i].pBt);
	}
	break;
}

/* Opcode: Transaction * * *
**
** Begin a transaction.  The transaction ends when a Commit or Rollback
** opcode is encountered.  Depending on the ON CONFLICT setting, the
** transaction might also be rolled back if an error is encountered.
** If the database was not opened with an environment, or the environment
** does not support transactions (i.e. it is a DS or CDS environment) this
** will not actually begin a transaction.
*/
case OP_Transaction: {
	int i = pOp->p1;
	DBSQL_ASSERT(i >= 0 && i < db->nDb);
	if (db->aDb[i].inTrans) /* TODO: do we need this? */
		break;
	rc = __sm_begin_txn(db->aDb[i].pBt);
	db->aDb[i].inTrans = 1;
	p->undoTransOnError = 1;
	break;
}

/* Opcode: Commit * * *
**
** Cause all modifications to the database that have been made during this
** transaction to actually take effect.  No additional modifications
** are allowed until another transaction is started.
*/
case OP_Commit: {
	int i = pOp->p1;
	DBSQL_ASSERT(i >= 0 && i < db->nDb);
	if (db->xCommitCallback != 0) {
		if (__safety_off(db))
			goto abort_due_to_misuse; 
		if (db->xCommitCallback(db->pCommitArg) != 0) {
			rc = DBSQL_CONSTRAINT;
		}
		if (__safety_on(db))
			goto abort_due_to_misuse;
	}
	if (db->aDb[i].inTrans) {
		rc = __sm_commit_txn(db->aDb[i].pBt);
		db->aDb[i].inTrans = 0;
	}
	if (rc == DBSQL_SUCCESS) {
		__commit_internal_changes(db);
	} else {
		__sm_abort_txn(db->aDb[i].pBt);
		__rollback_internal_changes(db);
	}
	break;
}

/* Opcode: Rollback * * *
**
** Cause all modifications to the database that have been made since the
** start of the transaction to be undone. The database is restored to its state
** before the OP_Transaction opcode was executed.  No additional modifications
** are allowed until another transaction is started.
**
** This instruction automatically closes all cursors and releases both
** the read and write locks on the indicated database.
*/
case OP_Rollback: {
	int i = pOp->p1;
	DBSQL_ASSERT(i >= 0 && i < db->nDb);
	DBSQL_ASSERT(db->aDb[i].inTrans); /* TODO: do we need this? */
	if (db->aDb[i].inTrans) {
		__sm_abort_txn(db->aDb[i].pBt);
		__rollback_internal_changes(db);
	}
	break;
}

/* Opcode: SetFormatVersion P1 * *
**
** Write the top of the stack into database P1 as its format_version.
** A transaction must be started before executing this opcode.
*/
case OP_SetFormatVersion: {
	DBSQL_ASSERT(pOp->p1 >= 0 && pOp->p1 < db->nDb);
	DBSQL_ASSERT(db->aDb[pOp->p1].pBt != 0);
	DBSQL_ASSERT(pTos >= p->aStack);
	__entity_to_int(pTos);
	rc = __sm_set_format_version(db->aDb[pOp->p1].pBt, pOp->p1, pTos->i);
	DBSQL_ASSERT(pTos->flags == MEM_Int);
	pTos--;
	break;
}

/* Opcode: SetSchemaSignature P1 * *
**
** The top of the stack is the schema generation version number.
** Every time something changes effecting the schema it is updated.
** This value needs to be stored. P1 is the database to effect.
*/
case OP_SetSchemaSignature: {
	DBSQL_ASSERT(pTos >= p->aStack);
	__entity_to_int(pTos);
	rc = __sm_set_schema_sig(db->aDb[pOp->p1].pBt, pTos->i);
	DBSQL_ASSERT(pTos->flags == MEM_Int);
	pTos--;
	break;
}

/* Opcode: VerifySchemaSignature P1 P2 *
**
** The schema signature is shared by all the DB that make up
** this database.  Ask the storage manager to grab that local
** value and make sure that it is equal to P2.  P1 is the database
** number which is 0 for the main database file and 1 for the file
** holding temporary tables and some higher number for auxiliary
** databases.
**
** The cookie changes its value whenever the database schema changes.
** This operation is used to detect when that the cookie has changed.
** If not in sync, the current process needs to reread the schema.
**
** This operation should only occur during a transaction as a schema
** change in the post-check and pre-commit would spell trouble for
** everyone (and their data).
*/
case OP_VerifySchemaSignature: {
	u_int32_t sig;
	DBSQL_ASSERT(pOp->p1 >= 0 && pOp->p1 < db->nDb);
	rc = __sm_get_schema_sig(db->aDb[pOp->p1].pBt, &sig);
	if (rc == DBSQL_SUCCESS && sig != pOp->p2) {
		__str_append(&p->zErrMsg, "database schema has changed",
			     (char*)0);
		rc = DBSQL_SCHEMA;
	}
	break;
}

/* Opcode: OpenRead P1 P2 P3
**
** Open a read-only cursor for the database table whose root page is
** P2 in a database file.  The database file is determined by an 
** integer from the top of the stack.  0 means the main database and
** 1 means the database used for temporary tables.  Give the new 
** cursor an identifier of P1.  The P1 values need not be contiguous
** but all P1 values should be small integers.  It is an error for
** P1 to be negative.
**
** If P2==0 then take the root page number from the next of the stack.
**
** There will be a read lock on the database whenever there is an
** open cursor.  If the database was unlocked prior to this instruction
** then a read lock is acquired as part of this instruction.  A read
** lock allows other processes to read the database but prohibits
** any other process from modifying the database.  The read lock is
** released when all cursors are closed.  If this instruction attempts
** to get a read lock but fails, the script terminates with an
** DBSQL_BUSY error code.
**
** The P3 value is the name of the table or index being opened.
** The P3 value is not actually used by this opcode and may be
** omitted.  But the code generator usually inserts the index or
** table name into P3 to make the code easier to read.
**
** See also OpenWrite.
*/
/* Opcode: OpenWrite P1 P2 P3
**
** Open a read/write cursor named P1 on the table or index whose root
** page is P2.  If P2==0 then take the root page number from the stack.
**
** The P3 value is the name of the table or index being opened.
** The P3 value is not actually used by this opcode and may be
** omitted.  But the code generator usually inserts the index or
** table name into P3 to make the code easier to read.
**
** This instruction works just like OpenRead except that it opens the cursor
** in read/write mode.  For a given table, there can be one or more read-only
** cursors or a single read/write cursor but not both.
**
** See also OpenRead.
*/
case OP_OpenRead: /* FALLTHROUGH */
case OP_OpenWrite: {
	int busy = 0;
	int i = pOp->p1;
	int p2 = pOp->p2;
	int wrFlag;
	sm_t *pX;
	int iDb;
  
	DBSQL_ASSERT(pTos >= p->aStack);
	__entity_to_int(pTos);
	iDb = pTos->i;
	pTos--;
	DBSQL_ASSERT(iDb >= 0 && iDb < db->nDb);
	pX = db->aDb[iDb].pBt;
	DBSQL_ASSERT(pX != 0);
	wrFlag = (pOp->opcode == OP_OpenWrite);
	if (p2 <= 0) {
		DBSQL_ASSERT(pTos >= p->aStack);
		__entity_to_int(pTos);
		p2 = pTos->i;
		pTos--;
		if(p2 < 2) {
			__str_append(&p->zErrMsg,
				     "root page number less than 2", (char*)0);
			rc = DBSQL_INTERNAL;
			break;
		}
	}
	DBSQL_ASSERT(i >= 0);
	if (__expand_cursor_array_size(p, i))
		goto no_mem;
	__vdbe_cleanup_cursor(&p->aCsr[i]);
	memset(&p->aCsr[i], 0, sizeof(cursor_t));
	p->aCsr[i].nullRow = 1;
	if (pX == 0)
		break;
	do {
		rc = __sm_cursor(pX, p2, wrFlag, &p->aCsr[i].pCursor);
		switch(rc) {
		case DBSQL_BUSY: {
			if (db->xBusyCallback == 0) {
				p->pc = pc;
				p->rc = DBSQL_BUSY;
                                /* Operands must remain on stack */
				p->pTos = &pTos[1 + (pOp->p2 <= 0)];
				return DBSQL_BUSY;
			} else if ((*db->xBusyCallback)(db, db->pBusyArg,
					pOp->p3, ++busy) == 0) {
				__str_append(&p->zErrMsg, dbsql_strerror(rc),
					     (char*)0);
				busy = 0;
			}
			break;
		}
		case DBSQL_SUCCESS: {
			busy = 0;
			break;
		}
		default: {
			goto abort_due_to_error;
		}
		}
	} while(busy);
	break;
}

/* Opcode: OpenTemp P1 P2 *
**
** Open a new cursor to a transient table.
** The transient cursor is always opened read/write even if 
** the main database is read-only.  The transient table is deleted
** automatically when the cursor is closed.
**
** The cursor points to a BTree table if P2==0 and to a BTree index
** if P2==1.  A BTree table must have an integer key and can have arbitrary
** data.  A BTree index has no data but can have an arbitrary key.
**
** This opcode is used for tables that exist for the duration of a single
** SQL statement only.  Tables created using CREATE TEMPORARY TABLE
** are opened using OP_OpenRead or OP_OpenWrite.  "Temporary" in the
** context of this opcode means for the duration of a single SQL statement
** whereas "Temporary" in the context of CREATE TABLE means for the duration
** of the connection to the database.  Same word; different meanings.
*/
case OP_OpenTemp: {
	int i = pOp->p1;
	cursor_t *pCx;
	DBSQL_ASSERT(i >= 0);
	if (__expand_cursor_array_size(p, i))
		goto no_mem;
	pCx = &p->aCsr[i];
	__vdbe_cleanup_cursor(pCx);
	memset(pCx, 0, sizeof(*pCx));
	pCx->nullRow = 1;

	rc = __sm_create(db, 0, 1,
			 (F_ISSET(db, DBSQL_DurableTemp) == 0), &pCx->pBt);

	if (rc == DBSQL_SUCCESS) {
		rc = __sm_begin_txn(pCx->pBt);
	}
	if (rc == DBSQL_SUCCESS) {
		if (pOp->p2) {
			int pgno;
			rc = __sm_create_index(pCx->pBt, &pgno);
			if (rc == DBSQL_SUCCESS) {
				rc = __sm_cursor(pCx->pBt, pgno, 1,
						    &pCx->pCursor);
			}
		} else {
			rc = __sm_cursor(pCx->pBt, 2, 1, &pCx->pCursor);
		}
	}
	break;
}

/* Opcode: OpenPseudo P1 * *
**
** Open a new cursor that points to a fake table that contains a single
** row of data.  Any attempt to write a second row of data causes the
** first row to be deleted.  All data is deleted when the cursor is
** closed.
**
** A pseudo-table created by this opcode is useful for holding the
** NEW or OLD tables in a trigger.
*/
case OP_OpenPseudo: {
	int i = pOp->p1;
	cursor_t *pCx;
	DBSQL_ASSERT(i >= 0);
	if (__expand_cursor_array_size(p, i))
		goto no_mem;
	pCx = &p->aCsr[i];
	__vdbe_cleanup_cursor(pCx);
	memset(pCx, 0, sizeof(*pCx));
	pCx->nullRow = 1;
	pCx->pseudoTable = 1;
	break;
}

/* Opcode: Close P1 * *
**
** Close a cursor previously opened as P1.  If P1 is not
** currently open, this instruction is a no-op.
*/
case OP_Close: {
	int i = pOp->p1;
	if (i >= 0 && i < p->nCursor) {
		__vdbe_cleanup_cursor(&p->aCsr[i]);
	}
	break;
}

/* Opcode: MoveTo P1 P2 *
**
** Pop the top of the stack and use its value as a key.  Reposition
** cursor P1 so that it points to an entry with a matching key.  If
** the table contains no record with a matching key, then the cursor
** is left pointing at the first record that is greater than the key.
** If there are no records greater than the key and P2 is not zero,
** then an immediate jump to P2 is made.
**
** See also: Found, NotFound, Distinct, MoveLt
*/
/* Opcode: MoveLt P1 P2 *
**
** Pop the top of the stack and use its value as a key.  Reposition
** cursor P1 so that it points to the entry with the largest key that is
** less than the key popped from the stack.
** If there are no records less than than the key and P2
** is not zero then an immediate jump to P2 is made.
**
** See also: MoveTo
*/
case OP_MoveLt: /* FALLTHROUGH */
case OP_MoveTo: {
	int i = pOp->p1;
	cursor_t *pC;

	DBSQL_ASSERT(pTos >= p->aStack);
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	pC = &p->aCsr[i];
	if (pC->pCursor != 0) {
		int res, oc;
		pC->nullRow = 0;
		if (pTos->flags & MEM_Int) {
			int iKey = INT_TO_KEY(pTos->i);
			if (pOp->p2 == 0 && pOp->opcode == OP_MoveTo) {
				pC->movetoTarget = iKey;
				pC->deferredMoveto = 1;
				__entity_release_mem(pTos);
				pTos--;
				break;
			}
			__sm_moveto(pC->pCursor, (char*)&iKey, sizeof(int),
				    &res);
			pC->lastRecno = pTos->i;
			pC->recnoIsValid = (res == 0);
		} else {
			__entity_as_string(pTos);
			__sm_moveto(pC->pCursor, pTos->z, pTos->n, &res);
			pC->recnoIsValid = 0;
		}
		pC->deferredMoveto = 0;
#ifdef CONFIG_TEST
		dbsql_search_count++;
#endif
		oc = pOp->opcode;
		if (oc == OP_MoveTo && res < 0) {
			__sm_next(pC->pCursor, &res);
			pC->recnoIsValid = 0;
			if (res && pOp->p2 > 0) {
				pc = pOp->p2 - 1;
			}
		} else if (oc == OP_MoveLt) {
			if (res >= 0) {
				__sm_prev(pC->pCursor, &res);
				pC->recnoIsValid = 0;
			} else {
				/*
				 * 'res' might be negative because the table is
				 * empty.  Check to see if this is the case.
				 */
				int keysize;
				res = (__sm_key_size(pC->pCursor, &keysize)
				       != 0 || keysize == 0);
			}
			if (res && pOp->p2 > 0) {
				pc = pOp->p2 - 1;
			}
		}
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: Distinct P1 P2 *
**
** Use the top of the stack as a string key.  If a record with that key does
** not exist in the table of cursor P1, then jump to P2.  If the record
** does already exist, then fall thru.  The cursor is left pointing
** at the record if it exists. The key is not popped from the stack.
**
** This operation is similar to NotFound except that this operation
** does not pop the key from the stack.
**
** See also: Found, NotFound, MoveTo, IsUnique, NotExists
*/
/* Opcode: Found P1 P2 *
**
** Use the top of the stack as a string key.  If a record with that key
** does exist in table of P1, then jump to P2.  If the record
** does not exist, then fall thru.  The cursor is left pointing
** to the record if it exists.  The key is popped from the stack.
**
** See also: Distinct, NotFound, MoveTo, IsUnique, NotExists
*/
/* Opcode: NotFound P1 P2 *
**
** Use the top of the stack as a string key.  If a record with that key
** does not exist in table of P1, then jump to P2.  If the record
** does exist, then fall thru.  The cursor is left pointing to the
** record if it exists.  The key is popped from the stack.
**
** The difference between this operation and Distinct is that
** Distinct does not pop the key from the stack.
**
** See also: Distinct, Found, MoveTo, NotExists, IsUnique
*/
case OP_Distinct: /* FALLTHROUGH */
case OP_NotFound: /* FALLTHROUGH */
case OP_Found: {
	int i = pOp->p1;
	int alreadyExists = 0;
	cursor_t *pC;
	DBSQL_ASSERT(pTos >= p->aStack);
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	if ((pC = &p->aCsr[i])->pCursor != 0) {
		int res, rx;
		__entity_as_string(pTos);
		rx = __sm_moveto(pC->pCursor, pTos->z, pTos->n, &res);
		alreadyExists = (rx == DBSQL_SUCCESS && res == 0);
		pC->deferredMoveto = 0;
	}
	if (pOp->opcode == OP_Found) {
		if (alreadyExists)
			pc = pOp->p2 - 1;
	} else {
		if (!alreadyExists)
			pc = pOp->p2 - 1;
	}
	if (pOp->opcode != OP_Distinct) {
		__entity_release_mem(pTos);
		pTos--;
	}
	break;
}

/* Opcode: IsUnique P1 P2 *
**
** The top of the stack is an integer record number.  Call this
** record number R.  The next on the stack is an index key created
** using MakeIdxKey.  Call it K.  This instruction pops R from the
** stack but it leaves K unchanged.
**
** P1 is an index.  So all but the last four bytes of K are an
** index string.  The last four bytes of K are a record number.
**
** This instruction asks if there is an entry in P1 where the
** index string matches K but the record number is different
** from R.  If there is no such entry, then there is an immediate
** jump to P2.  If any entry does exist where the index string
** matches K but the record number is not R, then the record
** number for that entry is pushed onto the stack and control
** falls through to the next instruction.
**
** See also: Distinct, NotFound, NotExists, Found
*/
case OP_IsUnique: {
	int i = pOp->p1;
	mem_t *pNos = &pTos[-1];
	sm_cursor_t *pCrsr;
	int R;

	/*
	 * Pop the value R off the top of the stack
	 */
	DBSQL_ASSERT(pNos >= p->aStack);
	__entity_to_int(pTos);
	R = pTos->i;
	pTos--;
	DBSQL_ASSERT(i >= 0 && i <= p->nCursor);
	if ((pCrsr = p->aCsr[i].pCursor) != 0) {
		int res, rc;
		int v;         /* The record number on the P1 entry
				  that matches K. */
		char *zKey;    /* The value of K */
		int nKey;      /* Number of bytes in K */

		/*
		 * Make sure K is a string and make zKey point to K
		 */
		__entity_as_string(pNos);
		zKey = pNos->z;
		nKey = pNos->n;
		DBSQL_ASSERT(nKey >= 4);

		/*
		 * Search for an entry in P1 where all but the last four
		 * bytes match K.  If there is no such entry, jump immediately
		 * to P2.
		 */
		DBSQL_ASSERT(p->aCsr[i].deferredMoveto == 0);
		rc = __sm_moveto(pCrsr, zKey, (nKey - 4), &res);
		if (rc != DBSQL_SUCCESS)
			goto abort_due_to_error;
		if (res < 0) {
			rc = __sm_next(pCrsr, &res);
			if (res) {
				pc = pOp->p2 - 1;
				break;
			}
		}
		rc = __sm_key_compare(pCrsr, zKey, (nKey - 4), 4, &res);
		if (rc != DBSQL_SUCCESS)
			goto abort_due_to_error;
		if (res > 0) {
			pc = pOp->p2 - 1;
			break;
		}

		/*
		 * At this point, pCrsr is pointing to an entry in P1 where
		 * all but the last for bytes of the key match K.  Check to
		 * see if the last four bytes of the key are different from
		 * R.  If the last four bytes equal R then jump immediately
		 * to P2.
		 */
		__sm_key(pCrsr, (nKey - 4), 4, (char*)&v);
		v = KEY_TO_INT(v);
		if (v == R) {
			pc = pOp->p2 - 1;
			break;
		}

		/* The last four bytes of the key are different from R.
		 * Convert the last four bytes of the key into an integer
		 * and push it onto the stack.  (These bytes are the record
		 * number of an entry that violates a UNIQUE constraint.)
		 */
		pTos++;
		pTos->i = v;
		pTos->flags = MEM_Int;
	}
	break;
}

/* Opcode: NotExists P1 P2 *
**
** Use the top of the stack as a integer key.  If a record with that key
** does not exist in table of P1, then jump to P2.  If the record
** does exist, then fall thru.  The cursor is left pointing to the
** record if it exists.  The integer key is popped from the stack.
**
** The difference between this operation and NotFound is that this
** operation assumes the key is an integer and NotFound assumes it
** is a string.
**
** See also: Distinct, Found, MoveTo, NotFound, IsUnique
*/
case OP_NotExists: {
	int i = pOp->p1;
	sm_cursor_t *pCrsr;
	DBSQL_ASSERT(pTos >= p->aStack);
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	if ((pCrsr = p->aCsr[i].pCursor) != 0) {
		int res, rx, iKey;
		DBSQL_ASSERT(pTos->flags & MEM_Int);
		iKey = INT_TO_KEY(pTos->i);
		rx = __sm_moveto(pCrsr, (char*)&iKey, sizeof(int), &res);
		p->aCsr[i].lastRecno = pTos->i;
		p->aCsr[i].recnoIsValid = res==0;
		p->aCsr[i].nullRow = 0;
		if (rx != DBSQL_SUCCESS || res != 0) {
			pc = pOp->p2 - 1;
			p->aCsr[i].recnoIsValid = 0;
		}
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: NewRecno P1 * *
**
** Get a new integer record number to be used as the key to a table.
** The record number is not previously used as a key in the database
** table that cursor P1 points to.  The new record number is pushed 
** onto the stack.
*/
case OP_NewRecno: {
	static struct drand48_data rand;
	static int first_time = 1;
	if (first_time) {
		first_time = 0;
		srand48_r(1, &rand); /* XXX create a portable rand function */
	}
	int i = pOp->p1;
	u_int32_t v = 0;
	cursor_t *pC;
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	if ((pC = &p->aCsr[i])->pCursor == 0) {
		v = 0;
	} else {
		/* !!!
		 * The next rowid or record number (different terms for the
		 * same thing) is obtained in a two-step algorithm.
		 *
		 * First we attempt to find the largest existing rowid and
		 * add one to that.  But if the largest existing rowid is
		 * already the maximum positive integer, we have to fall
		 * through to the second probabilistic algorithm.
		 *
		 * The second algorithm is to select a rowid at random and
		 * see if it already exists in the table.  If it does not
		 * exist, we have succeeded.  If the random rowid does exist,
		 * we select a new one and try again, up to 1000 times.
		 *
		 * For a table with less than 2 billion entries, the
		 * probability of not finding a unused rowid is about
		 * 1.0e-300.  This is a non-zero probability, but it is
		 * still vanishingly small and should never cause a problem.
		 * You are much, much more likely to have a hardware failure
		 * than for this algorithm to fail.
		 *
		 * The analysis in the previous paragraph assumes that you
		 * have a good source of random numbers.  Is a library
		 * function like lrand48() good enough?  Maybe. Maybe not.
		 * It's hard to know whether there might be subtle bugs is
		 * some implementations of lrand48() that could cause problems.
		 * To avoid uncertainty, we implement our own  random number
		 * generator based on the RC4 algorithm.
		 *
		 * To promote locality of reference for repetitive inserts, the
		 * first few attempts at chosing a random rowid pick values
		 * just a little larger than the previous rowid.  This has
		 * been shown experimentally to double the speed of the COPY
		 * operation.
		 */
		int res, rx, cnt, x;
		cnt = 0;
		if (!pC->useRandomRowid) {
			if (pC->nextRowidValid) {
				v = pC->nextRowid;
			} else {
				rx = __sm_last(pC->pCursor, &res);
				if (res) {
					v = 1;
				} else {
					__sm_key(pC->pCursor, 0, sizeof(v),
						 (void*)&v);
					v = KEY_TO_INT(v);
					if (v == 0x7fffffff) {
						pC->useRandomRowid = 1;
					} else {
						v++;
					}
				}
			}
			if (v < 0x7fffffff) {
				pC->nextRowidValid = 1;
				pC->nextRowid = v + 1;
			} else {
				pC->nextRowidValid = 0;
			}
		}
		if (pC->useRandomRowid) {
			v = db->priorNewRowid;
			cnt = 0;
			do {
				if (v == 0 || cnt > 2) {
					rand32_r(&rand, &v);
					if (cnt < 5)
						v &= 0xffffff;
				} else {
					u_int8_t rb;
					rand8_r(&rand, &rb);
					v += rb + 1;
				}
				if (v == 0)
					continue;
				x = INT_TO_KEY(v);
				rx = __sm_moveto(pC->pCursor, &x, sizeof(int),
						 &res);
				cnt++;
			} while(cnt < 1000 && rx == DBSQL_SUCCESS && res == 0);
			db->priorNewRowid = v;
			if (rx == DBSQL_SUCCESS && res == 0) {
				rc = DBSQL_FULL;
				goto abort_due_to_error;
			}
		}
		pC->recnoIsValid = 0;
		pC->deferredMoveto = 0;
	}
	pTos++;
	pTos->i = v;
	pTos->flags = MEM_Int;
	break;
}

/* Opcode: PutIntKey P1 P2 *
**
** Write an entry into the table of cursor P1.  A new entry is
** created if it doesn't already exist or the data for an existing
** entry is overwritten.  The data is the value on the top of the
** stack.  The key is the next value down on the stack.  The key must
** be an integer.  The stack is popped twice by this instruction.
**
** If P2==1 then the row change count is incremented.  If P2==0 the
** row change count is unmodified.  The rowid is stored for subsequent
** return by the dbsql_last_inserted_rowid() function if P2 is 1.
*/
/* Opcode: PutStrKey P1 * *
**
** Write an entry into the table of cursor P1.  A new entry is
** created if it doesn't already exist or the data for an existing
** entry is overwritten.  The data is the value on the top of the
** stack.  The key is the next value down on the stack.  The key must
** be a string.  The stack is popped twice by this instruction.
**
** P1 may not be a pseudo-table opened using the OpenPseudo opcode.
*/
case OP_PutIntKey: /* FALLTHROUGH */
case OP_PutStrKey: {
	mem_t *pNos = &pTos[-1];
	int i = pOp->p1;
	cursor_t *pC;
	DBSQL_ASSERT(pNos >= p->aStack);
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	if (((pC = &p->aCsr[i])->pCursor != 0 || pC->pseudoTable)) {
		char *zKey;
		int nKey, iKey;
		if (pOp->opcode == OP_PutStrKey) {
			__entity_as_string(pNos);
			nKey = pNos->n;
			zKey = pNos->z;
		} else {
			DBSQL_ASSERT(pNos->flags & MEM_Int);
			nKey = sizeof(int);
			iKey = INT_TO_KEY(pNos->i);
			zKey = (char*)&iKey;
			if (pOp->p2) {
				db->_num_last_changes++;
				db->_num_total_changes++;
				db->lastRowid = pNos->i;
			}
			if (pC->nextRowidValid && pTos->i >= pC->nextRowid) {
				pC->nextRowidValid = 0;
			}
		}
		if (pC->pseudoTable) {
			/*
			 * PutStrKey does not work for pseudo-tables.
			 * The following DBSQL_ASSERT makes sure we are not
			 * trying to use PutStrKey on a pseudo-table
			 */
			DBSQL_ASSERT(pOp->opcode == OP_PutIntKey);
			__dbsql_free(NULL, pC->pData);
			pC->iKey = iKey;
			pC->nData = pTos->n;
			if (pTos->flags & MEM_Dyn) {
				pC->pData = pTos->z;
				pTos->flags = MEM_Null;
			} else {
				if (__dbsql_calloc(NULL, 1, pC->nData,
						&pC->pData) != ENOMEM) {
					memcpy(pC->pData, pTos->z, pC->nData);
				}
			}
			pC->nullRow = 0;
		} else {
			rc = __sm_insert(pC->pCursor, zKey, nKey, pTos->z,
					 pTos->n);
		}
		pC->recnoIsValid = 0;
		pC->deferredMoveto = 0;
	}
	__pop_stack(&pTos, 2);
	break;
}

/* Opcode: Delete P1 P2 *
**
** Delete the record at which the P1 cursor is currently pointing.
**
** The cursor will be left pointing at either the next or the previous
** record in the table. If it is left pointing at the next record, then
** the next Next instruction will be a no-op.  Hence it is OK to delete
** a record from within an Next loop.
**
** The row change counter is incremented if P2==1 and is unmodified
** if P2==0.
**
** If P1 is a pseudo-table, then this instruction is a no-op.
*/
case OP_Delete: {
	int i = pOp->p1;
	cursor_t *pC;
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	pC = &p->aCsr[i];
	if (pC->pCursor != 0) {
		__vdbe_cursor_moveto(pC);
		rc = __sm_delete(pC->pCursor);
		pC->nextRowidValid = 0;
	}
	if (pOp->p2) {
		db->_num_last_changes++;
		db->_num_total_changes++;
	}
	break;
}

/* Opcode: KeyAsData P1 P2 *
**
** Turn the key-as-data mode for cursor P1 either on (if P2==1) or
** off (if P2==0).  In key-as-data mode, the OP_Column opcode pulls
** data off of the key rather than the data.  This is used for
** processing compound selects.
*/
case OP_KeyAsData: {
	int i = pOp->p1;
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	p->aCsr[i].keyAsData = pOp->p2;
	break;
}

/* Opcode: RowData P1 * *
**
** Push onto the stack the complete row data for cursor P1.
** There is no interpretation of the data.  It is just copied
** onto the stack exactly as it is found in the database file.
**
** If the cursor is not pointing to a valid row, a NULL is pushed
** onto the stack.
*/
/* Opcode: RowKey P1 * *
**
** Push onto the stack the complete row key for cursor P1.
** There is no interpretation of the key.  It is just copied
** onto the stack exactly as it is found in the database file.
**
** If the cursor is not pointing to a valid row, a NULL is pushed
** onto the stack.
*/
case OP_RowKey: /* FALLTHROUGH */
case OP_RowData: {
	int i = pOp->p1;
	cursor_t *pC;
	int n;

	pTos++;
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	pC = &p->aCsr[i];
	if (pC->nullRow) {
		pTos->flags = MEM_Null;
	} else if (pC->pCursor != 0) {
		sm_cursor_t *pCrsr = pC->pCursor;
		__vdbe_cursor_moveto(pC);
		if (pC->nullRow) {
			pTos->flags = MEM_Null;
			break;
		} else if (pC->keyAsData || pOp->opcode == OP_RowKey) {
			__sm_key_size(pCrsr, &n);
		} else {
			__sm_data_size(pCrsr, &n);
		}
		pTos->n = n;
		if (n <= NBFS) {
			pTos->flags = MEM_Str | MEM_Short;
			pTos->z = pTos->zShort;
		} else {
			if (__dbsql_calloc(NULL, 1, n, &pTos->z) == ENOMEM)
				goto no_mem;
			pTos->flags = MEM_Str | MEM_Dyn;
		}
		if (pC->keyAsData || pOp->opcode == OP_RowKey) {
			__sm_key(pCrsr, 0, n, pTos->z);
		} else {
			__sm_data(pCrsr, 0, n, pTos->z);
		}
	} else if (pC->pseudoTable) {
		pTos->n = pC->nData;
		pTos->z = pC->pData;
		pTos->flags = MEM_Str|MEM_Ephem;
	} else {
		pTos->flags = MEM_Null;
	}
	break;
}

/* Opcode: Column P1 P2 *
**
** Interpret the data that cursor P1 points to as
** a structure built using the MakeRecord instruction.
** (See the MakeRecord opcode for additional information about
** the format of the data.)
** Push onto the stack the value of the P2-th column contained
** in the data.
**
** If the KeyAsData opcode has previously executed on this cursor,
** then the field might be extracted from the key rather than the
** data.
**
** If P1 is negative, then the record is stored on the stack rather
** than in a table.  For P1==-1, the top of the stack is used.
** For P1==-2, the next on the stack is used.  And so forth.  The
** value pushed is always just a pointer into the record which is
** stored further down on the stack.  The column value is not copied.
*/
case OP_Column: {
	int amt, offset, end, payloadSize;
	int i = pOp->p1;
	int p2 = pOp->p2;
	cursor_t *pC;
	char *zRec;
	sm_cursor_t *pCrsr;
	int idxWidth;
	unsigned char aHdr[10];

	DBSQL_ASSERT(i < p->nCursor);
	pTos++;
	if (i < 0) {
		DBSQL_ASSERT(&pTos[i] >= p->aStack);
		DBSQL_ASSERT(pTos[i].flags & MEM_Str);
		zRec = pTos[i].z;
		payloadSize = pTos[i].n;
	} else if ((pC = &p->aCsr[i])->pCursor != 0) {
		__vdbe_cursor_moveto(pC);
		zRec = 0;
		pCrsr = pC->pCursor;
		if (pC->nullRow) {
			payloadSize = 0;
		} else if (pC->keyAsData) {
			__sm_key_size(pCrsr, &payloadSize);
		} else {
			__sm_data_size(pCrsr, &payloadSize);
		}
	} else if (pC->pseudoTable) {
		payloadSize = pC->nData;
		zRec = pC->pData;
		DBSQL_ASSERT(payloadSize == 0 || zRec != 0);
	} else {
		payloadSize = 0;
	}

	/*
	 * Figure out how many bytes in the column data and where the column
	 * data begins.
	 */
	if (payloadSize == 0) {
		pTos->flags = MEM_Null;
		break;
	} else if (payloadSize < 256) {
		idxWidth = 1;
	} else if (payloadSize < 65536) {
		idxWidth = 2;
	} else {
		idxWidth = 3;
	}

	/*
	 * Figure out where the requested column is stored and how big it is.
	 */
	if (payloadSize < (idxWidth * (p2 + 1))) {
		rc = DBSQL_CORRUPT;
		goto abort_due_to_error;
	}
	if (zRec) {
		memcpy(aHdr, &zRec[idxWidth * p2], idxWidth * 2);
	} else if (pC->keyAsData) {
		__sm_key(pCrsr, idxWidth * p2, idxWidth * 2, (char*)aHdr);
	} else {
		__sm_data(pCrsr, idxWidth * p2, idxWidth * 2, (char*)aHdr);
	}
	offset = aHdr[0];
	end = aHdr[idxWidth];
	if (idxWidth > 1) {
		offset |= aHdr[1] << 8;
		end |= aHdr[idxWidth + 1] << 8;
		if (idxWidth > 2) {
			offset |= aHdr[2] << 16;
			end |= aHdr[idxWidth + 2] << 16;
		}
	}
	amt = end - offset;
	if (amt < 0 || offset < 0 || end > payloadSize) {
		rc = DBSQL_CORRUPT;
		goto abort_due_to_error;
	}

	/*
	 * 'amt' and 'offset' now hold the offset to the start of data and the
	 * amount of data.  Go get the data and put it on the stack.
	 */
	pTos->n = amt;
	if (amt == 0) {
		pTos->flags = MEM_Null;
	} else if (zRec) {
		pTos->flags = MEM_Str | MEM_Ephem;
		pTos->z = &zRec[offset];
	} else {
		if (amt <= NBFS) {
			pTos->flags = MEM_Str | MEM_Short;
			pTos->z = pTos->zShort;
		} else {
			if (__dbsql_calloc(NULL, 1, amt, &pTos->z) == ENOMEM)
				goto no_mem;
			pTos->flags = MEM_Str | MEM_Dyn;
		}
		if (pC->keyAsData) {
			__sm_key(pCrsr, offset, amt, pTos->z);
		} else {
			__sm_data(pCrsr, offset, amt, pTos->z);
		}
	}
	break;
}

/* Opcode: Recno P1 * *
**
** Push onto the stack an integer which is the first 4 bytes of the
** the key to the current entry in a sequential scan of the database
** file P1.  The sequential scan should have been started using the 
** Next opcode.
*/
case OP_Recno: {
	int i = pOp->p1;
	cursor_t *pC;
	int v;

	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	pC = &p->aCsr[i];
	__vdbe_cursor_moveto(pC);
	pTos++;
	if (pC->recnoIsValid) {
		v = pC->lastRecno;
	} else if (pC->pseudoTable) {
		v = KEY_TO_INT(pC->iKey);
	} else if (pC->nullRow || pC->pCursor == 0) {
		pTos->flags = MEM_Null;
		break;
	} else {
		DBSQL_ASSERT(pC->pCursor != 0);
		__sm_key(pC->pCursor, 0, sizeof(u_int32_t), (char*)&v);
		v = KEY_TO_INT(v);
	}
	pTos->i = v;
	pTos->flags = MEM_Int;
	break;
}

/* Opcode: FullKey P1 * *
**
** Extract the complete key from the record that cursor P1 is currently
** pointing to and push the key onto the stack as a string.
**
** Compare this opcode to Recno.  The Recno opcode extracts the first
** 4 bytes of the key and pushes those bytes onto the stack as an
** integer.  This instruction pushes the entire key as a string.
**
** This opcode may not be used on a pseudo-table.
*/
case OP_FullKey: {
	int i = pOp->p1;
	sm_cursor_t *pCrsr;

	DBSQL_ASSERT(p->aCsr[i].keyAsData);
	DBSQL_ASSERT(!p->aCsr[i].pseudoTable);
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	pTos++;
	if ((pCrsr = p->aCsr[i].pCursor) != 0) {
		int amt;
		char *z;

		__vdbe_cursor_moveto(&p->aCsr[i]);
		__sm_key_size(pCrsr, &amt);
		if (amt <= 0) {
			rc = DBSQL_CORRUPT;
			goto abort_due_to_error;
		}
		if (amt > NBFS) {
			if (__dbsql_calloc(NULL, 1, amt, &z) == ENOMEM)
				goto no_mem;
			pTos->flags = MEM_Str | MEM_Dyn;
		} else {
			z = pTos->zShort;
			pTos->flags = MEM_Str | MEM_Short;
		}
		__sm_key(pCrsr, 0, amt, z);
		pTos->z = z;
		pTos->n = amt;
	}
	break;
}

/* Opcode: NullRow P1 * *
**
** Move the cursor P1 to a null row.  Any OP_Column operations
** that occur while the cursor is on the null row will always push 
** a NULL onto the stack.
*/
case OP_NullRow: {
	int i = pOp->p1;

	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	p->aCsr[i].nullRow = 1;
	p->aCsr[i].recnoIsValid = 0;
	break;
}

/* Opcode: Last P1 P2 *
**
** The next use of the Recno or Column or Next instruction for P1 
** will refer to the last entry in the database table or index.
** If the table or index is empty and P2>0, then jump immediately to P2.
** If P2 is 0 or if the table or index is not empty, fall through
** to the following instruction.
*/
case OP_Last: {
	int i = pOp->p1;
	cursor_t *pC;
	sm_cursor_t *pCrsr;

	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	pC = &p->aCsr[i];
	if ((pCrsr = pC->pCursor) != 0) {
		int res;
		rc = __sm_last(pCrsr, &res);
		pC->nullRow = res;
		pC->deferredMoveto = 0;
		if (res && pOp->p2 > 0) {
			pc = pOp->p2 - 1;
		}
	} else {
		pC->nullRow = 0;
	}
	break;
}

/* Opcode: Rewind P1 P2 *
**
** The next use of the Recno or Column or Next instruction for P1 
** will refer to the first entry in the database table or index.
** If the table or index is empty and P2>0, then jump immediately to P2.
** If P2 is 0 or if the table or index is not empty, fall through
** to the following instruction.
*/
case OP_Rewind: {
	int i = pOp->p1;
	cursor_t *pC;
	sm_cursor_t *pCrsr;

	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	pC = &p->aCsr[i];
	if ((pCrsr = pC->pCursor) != 0) {
		int res;
		rc = __sm_first(pCrsr, &res);
		pC->atFirst = (res == 0);
		pC->nullRow = res;
		pC->deferredMoveto = 0;
		if (res && pOp->p2 > 0) {
			pc = pOp->p2 - 1;
		}
	} else {
		pC->nullRow = 0;
	}
	break;
}

/* Opcode: Next P1 P2 *
**
** Advance cursor P1 so that it points to the next key/data pair in its
** table or index.  If there are no more key/value pairs then fall through
** to the following instruction.  But if the cursor advance was successful,
** jump immediately to P2.
**
** See also: Prev
*/
/* Opcode: Prev P1 P2 *
**
** Back up cursor P1 so that it points to the previous key/data pair in its
** table or index.  If there is no previous key/value pairs then fall through
** to the following instruction.  But if the cursor backup was successful,
** jump immediately to P2.
*/
case OP_Prev: /* FALLTHROUGH */
case OP_Next: {
	cursor_t *pC;
	sm_cursor_t *pCrsr;

	CHECK_FOR_INTERRUPT;
	DBSQL_ASSERT(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = &p->aCsr[pOp->p1];
	if ((pCrsr = pC->pCursor) != 0) {
		int res;
		if (pC->nullRow) {
			res = 1;
		} else {
			DBSQL_ASSERT(pC->deferredMoveto == 0);
			rc = pOp->opcode==OP_Next ? __sm_next(pCrsr, &res) :
				__sm_prev(pCrsr, &res);
			pC->nullRow = res;
		}
		if (res == 0) {
			pc = pOp->p2 - 1;
#ifdef CONFIG_TEST
		dbsql_search_count++;
#endif
		}
	} else {
		pC->nullRow = 1;
	}
	pC->recnoIsValid = 0;
	break;
}

/* Opcode: IdxPut P1 P2 P3
**
** The top of the stack holds a SQL index key made using the
** MakeIdxKey instruction.  This opcode writes that key into the
** index P1.  Data for the entry is nil.
**
** If P2==1, then the key must be unique.  If the key is not unique,
** the program aborts with a DBSQL_CONSTRAINT error and the database
** is rolled back.  If P3 is not null, then it becomes part of the
** error message returned with the DBSQL_CONSTRAINT.
*/
case OP_IdxPut: {
	int i = pOp->p1;
	sm_cursor_t *pCrsr;
	DBSQL_ASSERT(pTos >= p->aStack);
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	DBSQL_ASSERT(pTos->flags & MEM_Str);
	if ((pCrsr = p->aCsr[i].pCursor) != 0) {
		int nKey = pTos->n;
		const char *zKey = pTos->z;
		if (pOp->p2) {
			int res, n;
			DBSQL_ASSERT(nKey >= 4);
			rc = __sm_moveto(pCrsr, zKey, (nKey - 4), &res);
			if (rc != DBSQL_SUCCESS)
				goto abort_due_to_error;
			while(res != 0) {
				int c;
				__sm_key_size(pCrsr, &n);
				if (n == nKey &&
				    __sm_key_compare(pCrsr, zKey, (nKey - 4),
						     4, &c) == DBSQL_SUCCESS &&
				    c == 0) {
					rc = DBSQL_CONSTRAINT;
					if (pOp->p3 && pOp->p3[0]) {
						__str_append(&p->zErrMsg,
							    pOp->p3, (char*)0);
					}
					goto abort_due_to_error;
				}
				if (res < 0) {
					__sm_next(pCrsr, &res);
					res = +1;
				} else {
					break;
				}
			}
		}
		rc = __sm_insert(pCrsr, zKey, nKey, "", 0);
		DBSQL_ASSERT(p->aCsr[i].deferredMoveto == 0);
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: IdxDelete P1 * *
**
** The top of the stack is an index key built using the MakeIdxKey opcode.
** This opcode removes that entry from the index.
*/
case OP_IdxDelete: {
	int i = pOp->p1;
	sm_cursor_t *pCrsr;
	DBSQL_ASSERT(pTos >= p->aStack);
	DBSQL_ASSERT(pTos->flags & MEM_Str);
	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	if ((pCrsr = p->aCsr[i].pCursor) != 0) {
		int rx, res;
		rx = __sm_moveto(pCrsr, pTos->z, pTos->n, &res);
		if (rx == DBSQL_SUCCESS && res == 0) {
			rc = __sm_delete(pCrsr);
		}
		DBSQL_ASSERT(p->aCsr[i].deferredMoveto == 0);
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: IdxRecno P1 * *
**
** Push onto the stack an integer which is the last 4 bytes of the
** the key to the current entry in index P1.  These 4 bytes should
** be the record number of the table entry to which this index entry
** points.
**
** See also: Recno, MakeIdxKey.
*/
case OP_IdxRecno: {
	int i = pOp->p1;
	sm_cursor_t *pCrsr;

	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	pTos++;
	if ((pCrsr = p->aCsr[i].pCursor) != 0) {
		int v;
		int sz;
		DBSQL_ASSERT(p->aCsr[i].deferredMoveto == 0);
		__sm_key_size(pCrsr, &sz);
		if (sz < sizeof(u_int32_t)) {
			pTos->flags = MEM_Null;
		} else {
			__sm_key(pCrsr, sz - sizeof(u_int32_t),
				 sizeof(u_int32_t), (char*)&v);
			v = KEY_TO_INT(v);
			pTos->i = v;
			pTos->flags = MEM_Int;
		}
	} else {
		pTos->flags = MEM_Null;
	}
	break;
}

/* Opcode: IdxGT P1 P2 *
**
** Compare the top of the stack against the key on the index entry that
** cursor P1 is currently pointing to.  Ignore the last 4 bytes of the
** index entry.  If the index entry is greater than the top of the stack
** then jump to P2.  Otherwise fall through to the next instruction.
** In either case, the stack is popped once.
*/
/* Opcode: IdxGE P1 P2 *
**
** Compare the top of the stack against the key on the index entry that
** cursor P1 is currently pointing to.  Ignore the last 4 bytes of the
** index entry.  If the index entry is greater than or equal to 
** the top of the stack
** then jump to P2.  Otherwise fall through to the next instruction.
** In either case, the stack is popped once.
*/
/* Opcode: IdxLT P1 P2 *
**
** Compare the top of the stack against the key on the index entry that
** cursor P1 is currently pointing to.  Ignore the last 4 bytes of the
** index entry.  If the index entry is less than the top of the stack
** then jump to P2.  Otherwise fall through to the next instruction.
** In either case, the stack is popped once.
*/
case OP_IdxLT: /* FALLTHROUGH */
case OP_IdxGT: /* FALLTHROUGH */
case OP_IdxGE: {
	int i= pOp->p1;
	sm_cursor_t *pCrsr;

	DBSQL_ASSERT(i >= 0 && i < p->nCursor);
	DBSQL_ASSERT(pTos >= p->aStack);
	if ((pCrsr = p->aCsr[i].pCursor) != 0) {
		int res, rc;
 
		__entity_as_string(pTos);
		DBSQL_ASSERT(p->aCsr[i].deferredMoveto == 0);
		rc = __sm_key_compare(pCrsr, pTos->z, pTos->n, 4, &res);
		if (rc != DBSQL_SUCCESS) {
			break;
		}
		if (pOp->opcode == OP_IdxLT) {
			res = -res;
		} else if (pOp->opcode == OP_IdxGE) {
			res++;
		}
		if (res > 0) {
			pc = pOp->p2 - 1 ;
		}
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: IdxIsNull P1 P2 *
**
** The top of the stack contains an index entry such as might be generated
** by the MakeIdxKey opcode.  This routine looks at the first P1 fields of
** that key.  If any of the first P1 fields are NULL, then a jump is made
** to address P2.  Otherwise we fall straight through.
**
** The index entry is always popped from the stack.
*/
case OP_IdxIsNull: {
	int i = pOp->p1;
	int k, n;
	const char *z;

	DBSQL_ASSERT(pTos >= p->aStack);
	DBSQL_ASSERT(pTos->flags & MEM_Str);
	z = pTos->z;
	n = pTos->n;
	for (k = 0; k < n && i > 0; i--) {
		if (z[k] == 'a') {
			pc = pOp->p2-1;
			break;
		}
		while(k < n && z[k]) {
			k++;
		}
		k++;
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: Destroy P1 P2 *
**
** Delete an entire database table or index whose root page in the database
** file is given by P1.
**
** The table being destroyed is in the main database file if P2==0.  If
** P2==1 then the table to be clear is in the auxiliary database file
** that is used to store tables create using CREATE TEMPORARY TABLE.
**
** See also: Clear
*/
case OP_Destroy: {
	rc = __sm_drop_table(db->aDb[pOp->p2].pBt, pOp->p1);
	break;
}

/* Opcode: Clear P1 P2 *
**
** Delete all contents of the database table or index whose root page
** in the database file is given by P1.  But, unlike Destroy, do not
** remove the table or index from the database file.
**
** The table being clear is in the main database file if P2==0.  If
** P2==1 then the table to be clear is in the auxiliary database file
** that is used to store tables create using CREATE TEMPORARY TABLE.
**
** See also: Destroy
*/
case OP_Clear: {
	rc = __sm_clear_table(db->aDb[pOp->p2].pBt, pOp->p1);
	break;
}

/* Opcode: CreateTable * P2 P3
**
** Allocate a new table in the main database file if P2==0 or in the
** auxiliary database file if P2==1.  Push the page number
** for the root page of the new table onto the stack.
**
** The root page number is also written to a memory location that P3
** points to.  This is the mechanism is used to write the root page
** number into the parser's internal data structures that describe the
** new table.
**
** The difference between a table and an index is this:  A table must
** have a 4-byte integer key and can have arbitrary data.  An index
** has an arbitrary key but no data.
**
** See also: CreateIndex
*/
/* Opcode: CreateIndex * P2 P3
**
** Allocate a new index in the main database file if P2==0 or in the
** auxiliary database file if P2==1.  Push the page number of the
** root page of the new index onto the stack.
**
** See documentation on OP_CreateTable for additional information.
*/
case OP_CreateIndex: /* FALLTHROUGH */
case OP_CreateTable: {
	int pgno;
	DBSQL_ASSERT(pOp->p3 != 0 && pOp->p3type == P3_POINTER);
	DBSQL_ASSERT(pOp->p2 >= 0 && pOp->p2 < db->nDb);
	DBSQL_ASSERT(db->aDb[pOp->p2].pBt != 0);
	if (pOp->opcode == OP_CreateTable) {
		rc = __sm_create_table(db->aDb[pOp->p2].pBt, &pgno);
	} else {
		rc = __sm_create_index(db->aDb[pOp->p2].pBt, &pgno);
	}
	pTos++;
	if (rc == DBSQL_SUCCESS) {
		pTos->i = pgno;
		pTos->flags = MEM_Int;
		*(u_int32_t*)pOp->p3 = pgno;
		pOp->p3 = 0;
	} else {
		pTos->flags = MEM_Null;
	}
	break;
}

/* Opcode: ListWrite * * *
**
** Write the integer on the top of the stack
** into the temporary storage list.
*/
case OP_ListWrite: {
	keylist_t *pKeylist;
	DBSQL_ASSERT(pTos >= p->aStack);
	pKeylist = p->pList;
	if (pKeylist == 0 || pKeylist->nUsed >= pKeylist->nKey) {
		if (__dbsql_calloc(NULL, 1, sizeof(keylist_t) +
				(999 * sizeof(pKeylist->aKey[0])),
				&pKeylist) == ENOMEM)
			goto no_mem;
		pKeylist->nKey = 1000;
		pKeylist->nRead = 0;
		pKeylist->nUsed = 0;
		pKeylist->pNext = p->pList;
		p->pList = pKeylist;
	}
	__entity_to_int(pTos);
	pKeylist->aKey[pKeylist->nUsed++] = pTos->i;
	DBSQL_ASSERT(pTos->flags==MEM_Int);
	pTos--;
	break;
}

/* Opcode: ListRewind * * *
**
** Rewind the temporary buffer back to the beginning.  This is 
** now a no-op.
*/
case OP_ListRewind: {
	/* This is now a no-op */
	break;
}

/* Opcode: ListRead * P2 *
**
** Attempt to read an integer from the temporary storage buffer
** and push it onto the stack.  If the storage buffer is empty, 
** push nothing but instead jump to P2.
*/
case OP_ListRead: {
	keylist_t *pKeylist;
	CHECK_FOR_INTERRUPT;
	pKeylist = p->pList;
	if (pKeylist != 0) {
		DBSQL_ASSERT(pKeylist->nRead >= 0);
		DBSQL_ASSERT(pKeylist->nRead < pKeylist->nUsed);
		DBSQL_ASSERT(pKeylist->nRead < pKeylist->nKey);
		pTos++;
		pTos->i = pKeylist->aKey[pKeylist->nRead++];
		pTos->flags = MEM_Int;
		if (pKeylist->nRead >= pKeylist->nUsed) {
			p->pList = pKeylist->pNext;
			__dbsql_free(NULL, pKeylist);
		}
	} else {
		pc = pOp->p2 - 1;
	}
	break;
}

/* Opcode: ListReset * * *
**
** Reset the temporary storage buffer so that it holds nothing.
*/
case OP_ListReset: {
	if (p->pList) {
		__vdbe_keylist_free(p->pList);
		p->pList = 0;
	}
	break;
}

/* Opcode: ListPush * * * 
**
** Save the current vdbe_t list such that it can be restored by a ListPop
** opcode. The list is empty after this is executed.
*/
case OP_ListPush: {
	p->keylistStackDepth++;
	DBSQL_ASSERT(p->keylistStackDepth > 0);
	if (__dbsql_realloc(NULL, sizeof(keylist_t *) * p->keylistStackDepth,
			 &p->keylistStack) == ENOMEM)
		goto no_mem;
	p->keylistStack[p->keylistStackDepth - 1] = p->pList;
	p->pList = 0;
	break;
}

/* Opcode: ListPop * * * 
**
** Restore the vdbe_t list to the state it was in when ListPush was last
** executed.
*/
case OP_ListPop: {
	DBSQL_ASSERT(p->keylistStackDepth > 0);
	p->keylistStackDepth--;
	__vdbe_keylist_free(p->pList);
	p->pList = p->keylistStack[p->keylistStackDepth];
	p->keylistStack[p->keylistStackDepth] = 0;
	if (p->keylistStackDepth == 0) {
		__dbsql_free(NULL, p->keylistStack);
		p->keylistStack = 0;
	}
	break;
}

/* Opcode: SortPut * * *
**
** The TOS is the key and the NOS is the data.  Pop both from the stack
** and put them on the sorter.  The key and data should have been
** made using SortMakeKey and SortMakeRec, respectively.
*/
case OP_SortPut: {
	mem_t *pNos = &pTos[-1];
	sorter_t *pSorter;
	DBSQL_ASSERT(pNos >= p->aStack);
	if (__entity_to_string(pTos) || __entity_to_string(pNos))
		goto no_mem;
	if (__dbsql_calloc(NULL, 1, sizeof(sorter_t), &pSorter) == ENOMEM)
		goto no_mem;
	pSorter->pNext = p->pSort;
	p->pSort = pSorter;
	DBSQL_ASSERT(pTos->flags & MEM_Dyn);
	pSorter->nKey = pTos->n;
	pSorter->zKey = pTos->z;
	DBSQL_ASSERT(pNos->flags & MEM_Dyn);
	pSorter->nData = pNos->n;
	pSorter->pData = pNos->z;
	pTos -= 2;
	break;
}

/* Opcode: SortMakeRec P1 * *
**
** The top P1 elements are the arguments to a callback.  Form these
** elements into a single data entry that can be stored on a sorter
** using SortPut and later fed to a callback using SortCallback.
*/
case OP_SortMakeRec: {
	char *z;
	char **azArg;
	int nByte;
	int nField;
	int i;
	mem_t *pRec;

	nField = pOp->p1;
	pRec = &pTos[1 - nField];
	DBSQL_ASSERT( pRec>=p->aStack );
	nByte = 0;
	for(i = 0; i < nField; i++, pRec++) {
		if ((pRec->flags & MEM_Null) == 0) {
			__entity_as_string(pRec);
			nByte += pRec->n;
		}
	}
	nByte += sizeof(char*) * (nField + 1);
	if (__dbsql_calloc(NULL, 1, nByte, &azArg) == ENOMEM)
		goto no_mem;
	z = (char*)&azArg[nField + 1];
	for (pRec = &pTos[1 - nField], i = 0; i < nField; i++, pRec++) {
		if (pRec->flags & MEM_Null) {
			azArg[i] = 0;
		} else {
			azArg[i] = z;
			memcpy(z, pRec->z, pRec->n);
			z += pRec->n;
		}
	}
	__pop_stack(&pTos, nField);
	pTos++;
	pTos->n = nByte;
	pTos->z = (char*)azArg;
	pTos->flags = MEM_Str | MEM_Dyn;
	break;
}

/* Opcode: SortMakeKey * * P3
**
** Convert the top few entries of the stack into a sort key.  The
** number of stack entries consumed is the number of characters in 
** the string P3.  One character from P3 is prepended to each entry.
** The first character of P3 is prepended to the element lowest in
** the stack and the last character of P3 is prepended to the top of
** the stack.  All stack entries are separated by a \000 character
** in the result.  The whole key is terminated by two \000 characters
** in a row.
**
** "N" is substituted in place of the P3 character for NULL values.
**
** See also the MakeKey and MakeIdxKey opcodes.
*/
case OP_SortMakeKey: {
	char *zNewKey;
	int nByte;
	int nField;
	int i, j, k;
	mem_t *pRec;

	nField = strlen(pOp->p3);
	pRec = &pTos[1 - nField];
	nByte = 1;
	for (i = 0; i < nField; i++, pRec++) {
		if (pRec->flags & MEM_Null) {
			nByte += 2;
		} else {
			__entity_as_string(pRec);
			nByte += pRec->n+2;
		}
	}
	if (__dbsql_calloc(NULL, 1, nByte, &zNewKey) == ENOMEM)
		goto no_mem;
	j = 0;
	k = 0;
	for (pRec = &pTos[1 - nField], i = 0; i < nField; i++, pRec++) {
		if (pRec->flags & MEM_Null) {
			zNewKey[j++] = 'N';
			zNewKey[j++] = 0;
			k++;
		} else {
			zNewKey[j++] = pOp->p3[k++];
			memcpy(&zNewKey[j], pRec->z, pRec->n - 1);
			j += pRec->n - 1;
			zNewKey[j++] = 0;
		}
	}
	zNewKey[j] = 0;
	DBSQL_ASSERT(j < nByte);
	__pop_stack(&pTos, nField);
	pTos++;
	pTos->n = nByte;
	pTos->flags = MEM_Str | MEM_Dyn;
	pTos->z = zNewKey;
	break;
}

/* Opcode: Sort * * *
**
** Sort all elements on the sorter.  The algorithm is a
** mergesort.
*/
case OP_Sort: {
	int i;
	sorter_t *pElem;
	sorter_t *apSorter[NSORT];
	for(i = 0; i < NSORT; i++) {
		apSorter[i] = 0;
	}
	while(p->pSort) {
		pElem = p->pSort;
		p->pSort = pElem->pNext;
		pElem->pNext = 0;
		for (i = 0; i < NSORT-1; i++) {
			if (apSorter[i] == 0) {
				apSorter[i] = pElem;
				break;
			} else {
				pElem = __sorted_merge(apSorter[i], pElem);
				apSorter[i] = 0;
			}
		}
		if (i >= NSORT - 1) {
			apSorter[NSORT - 1] =
				__sorted_merge(apSorter[NSORT - 1],pElem);
		}
	}
	pElem = 0;
	for(i=0; i < NSORT; i++) {
		pElem = __sorted_merge(apSorter[i], pElem);
	}
	p->pSort = pElem;
	break;
}

/* Opcode: SortNext * P2 *
**
** Push the data for the topmost element in the sorter onto the
** stack, then remove the element from the sorter.  If the sorter
** is empty, push nothing on the stack and instead jump immediately 
** to instruction P2.
*/
case OP_SortNext: {
	sorter_t *pSorter = p->pSort;
	CHECK_FOR_INTERRUPT;
	if (pSorter != 0) {
		p->pSort = pSorter->pNext;
		pTos++;
		pTos->z = pSorter->pData;
		pTos->n = pSorter->nData;
		pTos->flags = MEM_Str | MEM_Dyn;
		__dbsql_free(NULL, pSorter->zKey);
		__dbsql_free(NULL, pSorter);
	} else {
		pc = pOp->p2 - 1;
	}
	break;
}

/* Opcode: SortCallback P1 * *
**
** The top of the stack contains a callback record built using
** the SortMakeRec operation with the same P1 value as this
** instruction.  Pop this record from the stack and invoke the
** callback on it.
*/
case OP_SortCallback: {
	DBSQL_ASSERT(pTos >= p->aStack);
	DBSQL_ASSERT(pTos->flags & MEM_Str);
	if (p->xCallback == 0) {
		p->pc = pc + 1;
		p->azResColumn = (char**)pTos->z;
		p->nResColumn = pOp->p1;
		p->popStack = 1;
		p->pTos = pTos;
		return DBSQL_ROW;
	} else {
		if (__safety_off(db))
			goto abort_due_to_misuse;
		if (p->xCallback(p->pCbArg, pOp->p1, (char**)pTos->z,
				 p->azColName) != 0) {
			rc = DBSQL_ABORT;
		}
		if (__safety_on(db))
			goto abort_due_to_misuse;
		p->nCallback++;
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: SortReset * * *
**
** Remove any elements that remain on the sorter.
*/
case OP_SortReset: {
	__vdbe_sorter_reset(p);
	break;
}

/* Opcode: FileOpen * * P3
**
** Open the file named by P3 for reading using the FileRead opcode.
** If P3 is "stdin" then open standard input for reading.
*/
case OP_FileOpen: {
	DBSQL_ASSERT(pOp->p3 != 0);
	if (p->pFile) {
		if (p->pFile != stdin)
			fclose(p->pFile);
		p->pFile = 0;
	}
	if (strcasecmp(pOp->p3, "stdin") == 0) {
		p->pFile = stdin;
	} else {
		p->pFile = fopen(pOp->p3, "r");
	}
	if (p->pFile == 0) {
		__str_append(&p->zErrMsg, "unable to open file: ",
			     pOp->p3, (char*)0);
		rc = DBSQL_ERROR;
	}
	break;
}

/* Opcode: FileRead P1 P2 P3
**
** Read a single line of input from the open file (the file opened using
** FileOpen).  If we reach end-of-file, jump immediately to P2.  If
** we are able to get another line, split the line apart using P3 as
** a delimiter.  There should be P1 fields.  If the input line contains
** more than P1 fields, ignore the excess.  If the input line contains
** fewer than P1 fields, assume the remaining fields contain NULLs.
**
** Input ends if a line consists of just "\.".  A field containing only
** "\N" is a null field.  The backslash \ character can be used be used
** to escape newlines or the delimiter.
*/
case OP_FileRead: {
	int n, eol, nField, i, c, nDelim;
	char *zDelim, *z;
	CHECK_FOR_INTERRUPT;
	if (p->pFile == 0)
		goto fileread_jump;
	nField = pOp->p1;
	if (nField <= 0)
		goto fileread_jump;
	if (nField != p->nField || p->azField == 0) {
		if (__dbsql_realloc(NULL, (sizeof(char*) * nField) + 1,
				 &p->azField) == ENOMEM)
			goto no_mem;
		p->nField = nField;
	}
	n = 0;
	eol = 0;
	while(eol == 0) {
		if (p->zLine == 0 || n + 200 > p->nLineAlloc) {
			p->nLineAlloc = (p->nLineAlloc * 2) + 300;
			if (__dbsql_realloc(NULL, p->nLineAlloc,
					 &p->zLine) == ENOMEM) {
				p->nLineAlloc = 0;
				__dbsql_free(NULL, p->zLine);
				p->zLine = 0;
				goto no_mem;
			}
		}
		if (__fgets(&p->zLine[n], p->nLineAlloc-n, p->pFile) == 0) {
			eol = 1;
			p->zLine[n] = 0;
		} else {
			int c;
			while((c = p->zLine[n]) != 0) {
				if (c == '\\') {
					if (p->zLine[n + 1] == 0)
						break;
					n += 2;
				} else if (c == '\n') {
					p->zLine[n] = 0;
					eol = 1;
					break;
				} else {
					n++;
				}
			}
		}
	}
	if (n == 0)
		goto fileread_jump;
	z = p->zLine;
	if (z[0] == '\\' && z[1] == '.' && z[2] == 0) {
		goto fileread_jump;
	}
	zDelim = pOp->p3;
	if (zDelim == 0)
		zDelim = "\t";
	c = zDelim[0];
	nDelim = strlen(zDelim);
	p->azField[0] = z;
	for(i = 1; *z != 0 && i <= nField; i++) {
		int from, to;
		from = to = 0;
		if (z[0] == '\\' && z[1] == 'N' &&
		    (z[2] == 0 || strncmp(&z[2], zDelim, nDelim) == 0)) {
			if (i <= nField)
				p->azField[i - 1] = 0;
			z += 2 + nDelim;
			if (i < nField)
				p->azField[i] = z;
			continue;
		}
		while(z[from]) {
			if(z[from] == '\\' && z[from + 1] != 0) {
				int tx = z[from + 1];
				switch(tx) {
				case 'b':  tx = '\b'; break;
				case 'f':  tx = '\f'; break;
				case 'n':  tx = '\n'; break;
				case 'r':  tx = '\r'; break;
				case 't':  tx = '\t'; break;
				case 'v':  tx = '\v'; break;
				default:   break;
				}
				z[to++] = tx;
				from += 2;
				continue;
			}
			if (z[from] == c &&
			    strncmp(&z[from], zDelim, nDelim) == 0)
				break;
			z[to++] = z[from++];
		}
		if (z[from]) {
			z[to] = 0;
			z += from + nDelim;
			if (i < nField)
				p->azField[i] = z;
		} else {
			z[to] = 0;
			z = "";
		}
	}
	while(i < nField) {
		p->azField[i++] = 0;
	}
	break;

	/*
	 * If we reach end-of-file, or if anything goes wrong, jump here.
	 * This code will cause a jump to P2.
	 */
fileread_jump:
	pc = pOp->p2 - 1;
	break;
}

/* Opcode: FileColumn P1 * *
**
** Push onto the stack the P1-th column of the most recently read line
** from the input file.
*/
case OP_FileColumn: {
	int i = pOp->p1;
	char *z;
	DBSQL_ASSERT(i >= 0 && i < p->nField);
	if (p->azField) {
		z = p->azField[i];
	} else {
		z = 0;
	}
	pTos++;
	if (z) {
		pTos->n = strlen(z) + 1;
		pTos->z = z;
		pTos->flags = MEM_Str | MEM_Ephem;
	} else {
		pTos->flags = MEM_Null;
	}
	break;
}

/* Opcode: MemStore P1 P2 *
**
** Write the top of the stack into memory location P1.
** P1 should be a small integer since space is allocated
** for all memory locations between 0 and P1 inclusive.
**
** After the data is stored in the memory location, the
** stack is popped once if P2 is 1.  If P2 is zero, then
** the original data remains on the stack.
*/
case OP_MemStore: {
	int i = pOp->p1;
	mem_t *pMem;
	DBSQL_ASSERT(pTos >= p->aStack);
	if (i >= p->nMem) {
		int nOld = p->nMem;
		mem_t *oldMem;
		p->nMem = i + 5;
		oldMem = p->aMem;
		if (__dbsql_realloc(NULL, p->nMem * sizeof(p->aMem[0]),
				 &p->aMem) == ENOMEM)
			goto no_mem;
		if (oldMem != p->aMem) { /* realloc moved the memory */
			int j;
			for(j = 0; j < nOld; j++) {
				if (p->aMem[j].flags & MEM_Short) {
					p->aMem[j].z = p->aMem[j].zShort;
				}
			}
		}
		if (nOld < p->nMem) {
			memset(&p->aMem[nOld], 0,
			       sizeof(p->aMem[0]) * (p->nMem - nOld));
		}
	}
	if (__entity_ephem_to_dyn(pTos) == 1)
		goto no_mem;
	pMem = &p->aMem[i];
	__entity_release_mem(pMem);
	*pMem = *pTos;
	if (pMem->flags & MEM_Dyn) {
		if (pOp->p2) {
			pTos->flags = MEM_Null;
		} else {
			if (__dbsql_malloc(NULL, pMem->n, &pMem->z) == ENOMEM)
				goto no_mem;
			memcpy(pMem->z, pTos->z, pMem->n);
		}
	} else if (pMem->flags & MEM_Short) {
		pMem->z = pMem->zShort;
	}
	if (pOp->p2) {
		__entity_release_mem(pTos);
		pTos--;
	}
	break;
}

/* Opcode: MemLoad P1 * *
**
** Push a copy of the value in memory location P1 onto the stack.
**
** If the value is a string, then the value pushed is a pointer to
** the string that is stored in the memory location.  If the memory
** location is subsequently changed (using OP_MemStore) then the
** value pushed onto the stack will change too.
*/
case OP_MemLoad: {
	int i = pOp->p1;
	DBSQL_ASSERT(i >= 0 && i < p->nMem);
	pTos++;
	memcpy(pTos, &p->aMem[i], sizeof(pTos[0]) - NBFS);;
	if (pTos->flags & MEM_Str) {
		pTos->flags |= MEM_Ephem;
		pTos->flags &= ~(MEM_Dyn | MEM_Static | MEM_Short);
	}
	break;
}

/* Opcode: MemIncr P1 P2 *
**
** Increment the integer valued memory cell P1 by 1.  If P2 is not zero
** and the result after the increment is greater than zero, then jump
** to P2.
**
** This instruction throws an error if the memory cell is not initially
** an integer.
*/
case OP_MemIncr: {
	int i = pOp->p1;
	mem_t *pMem;
	DBSQL_ASSERT(i >= 0 && i < p->nMem);
	pMem = &p->aMem[i];
	DBSQL_ASSERT(pMem->flags == MEM_Int);
	pMem->i++;
	if (pOp->p2 > 0 && pMem->i > 0) {
		pc = pOp->p2 - 1;
	}
	break;
}

/* Opcode: AggReset * P2 *
**
** Reset the aggregator so that it no longer contains any data.
** Future aggregator elements will contain P2 values each.
*/
case OP_AggReset: {
	__vdbe_agg_reset(&p->agg);
	p->agg.nMem = pOp->p2;
	if (__dbsql_calloc(NULL, p->agg.nMem, sizeof(p->agg.apFunc[0]),
			&p->agg.apFunc) == ENOMEM)
		goto no_mem;
	break;
}

/* Opcode: AggInit * P2 P3
**
** Initialize the function parameters for an aggregate function.
** The aggregate will operate out of aggregate column P2.
** P3 is a pointer to the func_def_t structure for the function.
*/
case OP_AggInit: {
	int i = pOp->p2;
	DBSQL_ASSERT(i >= 0 && i < p->agg.nMem);
	p->agg.apFunc[i] = (func_def_t*)pOp->p3;
	break;
}

/* Opcode: AggFunc * P2 P3
**
** Execute the step function for an aggregate.  The
** function has P2 arguments.  P3 is a pointer to the func_def_t
** structure that specifies the function.
**
** The top of the stack must be an integer which is the index of
** the aggregate column that corresponds to this aggregate function.
** Ideally, this index would be another parameter, but there are
** no free parameters left.  The integer is popped from the stack.
*/
case OP_AggFunc: {
	int n = pOp->p2;
	int i;
	mem_t *pMem, *pRec;
	char **azArgv = p->zArgv;
	dbsql_func_t ctx;

	DBSQL_ASSERT(n >= 0);
	DBSQL_ASSERT(pTos->flags == MEM_Int);
	pRec = &pTos[-n];
	DBSQL_ASSERT(pRec >= p->aStack);
	for(i = 0; i < n; i++, pRec++) {
		if (pRec->flags & MEM_Null) {
			azArgv[i] = 0;
		} else {
			__entity_as_string(pRec);
			azArgv[i] = pRec->z;
		}
	}
	i = pTos->i;
	DBSQL_ASSERT(i >= 0 && i < p->agg.nMem);
	ctx.pFunc = (func_def_t*)pOp->p3;
	pMem = &p->agg.pCurrent->aMem[i];
	ctx.s.z = pMem->zShort;  /* Space used for small aggregate contexts */
	ctx.pAgg = pMem->z;
	ctx.cnt = ++pMem->i;
	ctx.isError = 0;
	ctx.isStep = 1;
	(ctx.pFunc->xStep)(&ctx, n, (const char**)azArgv);
	pMem->z = ctx.pAgg;
	pMem->flags = MEM_AggCtx;
	__pop_stack(&pTos, n + 1);
	if (ctx.isError) {
		rc = DBSQL_ERROR;
	}
	break;
}

/* Opcode: AggFocus * P2 *
**
** Pop the top of the stack and use that as an aggregator key.  If
** an aggregator with that same key already exists, then make the
** aggregator the current aggregator and jump to P2.  If no aggregator
** with the given key exists, create one and make it current but
** do not jump.
**
** The order of aggregator opcodes is important.  The order is:
** AggReset AggFocus AggNext.  In other words, you must execute
** AggReset first, then zero or more AggFocus operations, then
** zero or more AggNext operations.  You must not execute an AggFocus
** in between an AggNext and an AggReset.
*/
case OP_AggFocus: {
	agg_elem_t *pElem;
	char *zKey;
	int nKey;

	DBSQL_ASSERT(pTos >= p->aStack);
	__entity_as_string(pTos);
	zKey = pTos->z;
	nKey = pTos->n;
	pElem = __hash_find(&p->agg.hash, zKey, nKey);
	if (pElem) {
		p->agg.pCurrent = pElem;
		pc = pOp->p2 - 1;
	} else {
		__agg_insert(&p->agg, zKey, nKey);
	}
	__entity_release_mem(pTos);
	pTos--;
	break; 
}

/* Opcode: AggSet * P2 *
**
** Move the top of the stack into the P2-th field of the current
** aggregate.  String values are duplicated into new memory.
*/
case OP_AggSet: {
	agg_elem_t *pFocus = __agg_in_focus(&p->agg);
	mem_t *pMem;
	int i = pOp->p2;
	DBSQL_ASSERT(pTos>=p->aStack);
	if (pFocus == 0)
		goto no_mem;
	DBSQL_ASSERT(i >= 0 && i < p->agg.nMem);
	if (__entity_ephem_to_dyn(pTos) == 1)
		goto no_mem;
	pMem = &pFocus->aMem[i];
	__entity_release_mem(pMem);
	*pMem = *pTos;
	if (pMem->flags & MEM_Dyn) {
		pTos->flags = MEM_Null;
	} else if (pMem->flags & MEM_Short) {
		pMem->z = pMem->zShort;
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: AggGet * P2 *
**
** Push a new entry onto the stack which is a copy of the P2-th field
** of the current aggregate.  Strings are not duplicated so
** string values will be ephemeral.
*/
case OP_AggGet: {
	agg_elem_t *pFocus = __agg_in_focus(&p->agg);
	mem_t *pMem;
	int i = pOp->p2;
	if (pFocus == 0)
		goto no_mem;
	DBSQL_ASSERT(i >= 0 && i < p->agg.nMem);
	pTos++;
	pMem = &pFocus->aMem[i];
	*pTos = *pMem;
	if (pTos->flags & MEM_Str) {
		pTos->flags &= ~(MEM_Dyn|MEM_Static|MEM_Short);
		pTos->flags |= MEM_Ephem;
	}
	break;
}

/* Opcode: AggNext * P2 *
**
** Make the next aggregate value the current aggregate.  The prior
** aggregate is deleted.  If all aggregate values have been consumed,
** jump to P2.
**
** The order of aggregator opcodes is important.  The order is:
** AggReset AggFocus AggNext.  In other words, you must execute
** AggReset first, then zero or more AggFocus operations, then
** zero or more AggNext operations.  You must not execute an AggFocus
** in between an AggNext and an AggReset.
*/
case OP_AggNext: {
	CHECK_FOR_INTERRUPT;
	if (p->agg.pSearch == 0) {
		p->agg.pSearch = __hash_first(&p->agg.hash);
	} else {
		p->agg.pSearch = __hash_next(p->agg.pSearch);
	}
	if (p->agg.pSearch == 0) {
		pc = pOp->p2 - 1;
	} else {
		int i;
		dbsql_func_t ctx;
		mem_t *aMem;
		p->agg.pCurrent = __hash_data(p->agg.pSearch);
		aMem = p->agg.pCurrent->aMem;
		for(i = 0; i < p->agg.nMem; i++){
			int freeCtx;
			if (p->agg.apFunc[i] == 0)
				continue;
			if (p->agg.apFunc[i]->xFinalize == 0)
				continue;
			ctx.s.flags = MEM_Null;
			ctx.s.z = aMem[i].zShort;
			ctx.pAgg = (void*)aMem[i].z;
			freeCtx = aMem[i].z && aMem[i].z!=aMem[i].zShort;
			ctx.cnt = aMem[i].i;
			ctx.isStep = 0;
			ctx.pFunc = p->agg.apFunc[i];
			(*p->agg.apFunc[i]->xFinalize)(&ctx);
			if (freeCtx) {
				__dbsql_free(NULL, aMem[i].z);
			}
			aMem[i] = ctx.s;
			if (aMem[i].flags & MEM_Short) {
				aMem[i].z = aMem[i].zShort;
			}
		}
	}
	break;
}

/* Opcode: SetInsert P1 * P3
**
** If Set P1 does not exist then create it.  Then insert value
** P3 into that set.  If P3 is NULL, then insert the top of the
** stack into the set.
*/
case OP_SetInsert: {
	int i = pOp->p1;
	if (p->nSet <= i) {
		int k;
		if (__dbsql_realloc(NULL, (i+1) * sizeof(p->aSet[0]),
				 &p->aSet) == ENOMEM)
			goto no_mem;
		for(k = p->nSet; k <= i; k++) {
			__hash_init(&p->aSet[k].hash, DBSQL_HASH_BINARY, 1);
		}
		p->nSet = i + 1;
	}
	if (pOp->p3) {
		__hash_insert(&p->aSet[i].hash, pOp->p3, strlen(pOp->p3)+1, p);
	} else {
		DBSQL_ASSERT(pTos >= p->aStack);
		__entity_as_string(pTos);
		__hash_insert(&p->aSet[i].hash, pTos->z, pTos->n, p);
		__entity_release_mem(pTos);
		pTos--;
	}
	break;
}

/* Opcode: SetFound P1 P2 *
**
** Pop the stack once and compare the value popped off with the
** contents of set P1.  If the element popped exists in set P1,
** then jump to P2.  Otherwise fall through.
*/
case OP_SetFound: {
	int i = pOp->p1;
	DBSQL_ASSERT(pTos >= p->aStack);
	__entity_as_string(pTos);
	if (i >= 0 && i < p->nSet &&
	    __hash_find(&p->aSet[i].hash, pTos->z, pTos->n)) {
		pc = pOp->p2 - 1;
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: SetNotFound P1 P2 *
**
** Pop the stack once and compare the value popped off with the
** contents of set P1.  If the element popped does not exists in 
** set P1, then jump to P2.  Otherwise fall through.
*/
case OP_SetNotFound: {
	int i = pOp->p1;
	DBSQL_ASSERT(pTos>=p->aStack);
	__entity_as_string(pTos);
	if (i < 0 || i >= p->nSet ||
	    __hash_find(&p->aSet[i].hash, pTos->z, pTos->n) == 0) {
		pc = pOp->p2 - 1;
	}
	__entity_release_mem(pTos);
	pTos--;
	break;
}

/* Opcode: SetFirst P1 P2 *
**
** Read the first element from set P1 and push it onto the stack.  If the
** set is empty, push nothing and jump immediately to P2.  This opcode is
** used in combination with OP_SetNext to loop over all elements of a set.
*/
/* Opcode: SetNext P1 P2 *
**
** Read the next element from set P1 and push it onto the stack.  If there
** are no more elements in the set, do not do the push and fall through.
** Otherwise, jump to P2 after pushing the next set element.
*/
case OP_SetFirst:  /* FALLTHROUGH */
case OP_SetNext: {
	set_t *pSet;
	CHECK_FOR_INTERRUPT;
	if (pOp->p1 < 0 || pOp->p1 >= p->nSet) {
		if (pOp->opcode == OP_SetFirst)
			pc = pOp->p2 - 1;
		break;
	}
	pSet = &p->aSet[pOp->p1];
	if (pOp->opcode == OP_SetFirst) {
		pSet->prev = __hash_first(&pSet->hash);
		if (pSet->prev == 0) {
			pc = pOp->p2 - 1;
			break;
		}
	} else {
		DBSQL_ASSERT(pSet->prev);
		pSet->prev = __hash_next(pSet->prev);
		if (pSet->prev == 0) {
			break;
		} else {
			pc = pOp->p2 - 1;
		}
	}
	pTos++;
	pTos->z = __hash_key(pSet->prev);
	pTos->n = __hash_keysize(pSet->prev);
	pTos->flags = MEM_Str | MEM_Ephem;
	break;
}

/* Opcode: Vacuum * * *
**
** Vacuum the entire database.  This opcode will cause other virtual
** machines to be created and run.  It may not be called from within
** a transaction.
*/
case OP_Vacuum: {
	if (__safety_off(db))
		goto abort_due_to_misuse; 
	rc = __execute_vacuum(&p->zErrMsg, db);
	if (__safety_on(db))
		goto abort_due_to_misuse;
	break;
}

/* Any other opcode is illegal...
*/
default: {
	snprintf(zBuf, sizeof(zBuf), "%d", pOp->opcode);
	__str_append(&p->zErrMsg, "unknown opcode ", zBuf, (char*)0);
	rc = DBSQL_INTERNAL;
	break;
}

/*****************************************************************************
** The cases of the switch statement above this line should all be indented
** by 6 spaces.  But the left-most 6 spaces have been removed to improve the
** readability.  From this point on down, the normal indentation rules are
** restored.
*****************************************************************************/
    }

#ifdef VDBE_PROFILE
    {
	    long long elapse = __os_hwtime() - start;
	    pOp->cycles += elapse;
	    pOp->cnt++;
#if 0
	    fprintf(stdout, "%10lld ", elapse);
	    __vdbe_print_op(stdout, origPc, &p->aOp[origPc]);
#endif
    }
#endif

    /*
     * The following code adds nothing to the actual functionality
     * of the program.  It is only here for testing and debugging.
     * On the other hand, it does burn CPU cycles every time through
     * the evaluator loop.  So we can leave it out when NDEBUG is defined.
     */
#ifndef NDEBUG
    /* Sanity checking on the top element of the stack. */
    if (pTos >= p->aStack) {
	    DBSQL_ASSERT(pTos->flags != 0);  /* Must define some type */
	    if (pTos->flags & MEM_Str) {
		    int x = pTos->flags &
			    (MEM_Static | MEM_Dyn | MEM_Ephem | MEM_Short);
		    DBSQL_ASSERT(x != 0);            /* Strings must define
							a string subtype. */
		    DBSQL_ASSERT((x & (x-1)) == 0);  /* Only one string subtype
							can be defined. */
		    DBSQL_ASSERT(pTos->z != 0);      /* Strings must have a
							value. */
		    /*
		     * mem_t.z points to mem_t.zShort iff the subtype is
		     * MEM_Short
		     */
		    DBSQL_ASSERT((pTos->flags & MEM_Short) == 0 ||
			   pTos->z==pTos->zShort);
		    DBSQL_ASSERT((pTos->flags & MEM_Short) != 0 ||
			   pTos->z!=pTos->zShort);
	    } else {
		    /*
		     * Cannot define a string subtype for non-string objects.
		     */
		    DBSQL_ASSERT((pTos->flags &
		         (MEM_Static | MEM_Dyn | MEM_Ephem | MEM_Short)) == 0);
	    }
	    /* MEM_Null excludes all other types. */
	    DBSQL_ASSERT(pTos->flags == MEM_Null || (pTos->flags&MEM_Null) == 0);
    }
    if (pc < -1 || pc >= p->nOp) {
	    __str_append(&p->zErrMsg, "jump destination out of range",
			 (char*)0);
	    rc = DBSQL_INTERNAL;
    }
    if (p->trace && pTos >= p->aStack) {
	    int i;
	    fprintf(p->trace, "Stack:");
	    for(i = 0; i > -5 && &pTos[i] >= p->aStack; i--) {
		    if (pTos[i].flags & MEM_Null) {
			    fprintf(p->trace, " NULL");
		    } else if ((pTos[i].flags &
				(MEM_Int | MEM_Str)) == (MEM_Int | MEM_Str)) {
			    fprintf(p->trace, " si:%d", pTos[i].i);
		    } else if (pTos[i].flags & MEM_Int) {
			    fprintf(p->trace, " i:%d", pTos[i].i);
		    } else if (pTos[i].flags & MEM_Real) {
			    fprintf(p->trace, " r:%g", pTos[i].r);
		    } else if (pTos[i].flags & MEM_Str) {
			    int j, k;
			    char zBuf[100];
			    zBuf[0] = ' ';
			    if (pTos[i].flags & MEM_Dyn) {
				    zBuf[1] = 'z';
				    DBSQL_ASSERT((pTos[i].flags &
					    (MEM_Static | MEM_Ephem)) == 0);
			    } else if (pTos[i].flags & MEM_Static) {
				    zBuf[1] = 't';
				    DBSQL_ASSERT( (pTos[i].flags &
					     (MEM_Dyn | MEM_Ephem)) == 0);
			    } else if (pTos[i].flags & MEM_Ephem) {
				    zBuf[1] = 'e';
				    DBSQL_ASSERT((pTos[i].flags &
					    (MEM_Static | MEM_Dyn)) == 0);
			    } else {
				    zBuf[1] = 's';
			    }
			    zBuf[2] = '[';
			    k = 3;
			    for(j = 0; j < 20 && j < pTos[i].n; j++) {
				    int c = pTos[i].z[j];
				    if (c == 0 && j == pTos[i].n - 1)
					    break;
				    if (isprint(c) && !isspace(c)) {
					    zBuf[k++] = c;
				    } else {
					    zBuf[k++] = '.';
				    }
			    }
			    zBuf[k++] = ']';
			    zBuf[k++] = 0;
			    fprintf(p->trace, "%s", zBuf);
		    } else {
			    fprintf(p->trace, " ???");
		    }
	    }
	    if (rc != 0)
		    fprintf(p->trace," rc=%d",rc);
	    fprintf(p->trace, "\n");
    }
#endif
	}  /* The end of the for(;;) loop the loops through opcodes */

  /*
   * If we reach this point, it means that execution is finished.
   */
vdbe_halt:
	if (rc) {
		p->rc = rc;
		rc = DBSQL_ERROR;
	} else {
		rc = DBSQL_DONE;
	}
	p->magic = VDBE_MAGIC_HALT;
	p->pTos = pTos;
	return rc;

	/*
	 * Jump to here if a __dbsql_malloc() fails.  It's hard to get a
	 * __dbsql_malloc() to fail on a modern VM computer, so this code
	 * is untested.
	 */
no_mem:
	__str_append(&p->zErrMsg, "out of memory", (char*)0); /* TODO rm */
	rc = DBSQL_NOMEM;
	goto vdbe_halt;

	/*
	 * Jump to here for an DBSQL_MISUSE error.
	 */
abort_due_to_misuse:
	rc = DBSQL_MISUSE;
	/* Fall thru into abort_due_to_error. */

	/*
	 * Jump to here for any other kind of fatal error.  The "rc" variable
	 * should hold the error number.
	 */
abort_due_to_error:
	if (p->zErrMsg == 0) {
		__str_append(&p->zErrMsg, dbsql_strerror(rc), (char*)0);
	}
	goto vdbe_halt;

	/*
	 * Jump to here if the dbsql_interrupt() API sets the interrupt
	 * flag.
	 */
abort_due_to_interrupt:
	DBSQL_ASSERT(db->flags & DBSQL_Interrupt);
	db->flags &= ~DBSQL_Interrupt;
	if (db->magic != DBSQL_STATUS_BUSY) {
		rc = DBSQL_MISUSE;
	} else {
		rc = DBSQL_INTERRUPTED;
	}
	__str_append(&p->zErrMsg, dbsql_strerror(rc), (char*)0);
	goto vdbe_halt;
}
