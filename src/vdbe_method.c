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
 * $Id: vdbe_method.c 7 2007-02-03 13:34:17Z gburd $
 */

/*
 * This file contains code used for creating, destroying, and populating
 * a VDBE (also known as "dbsql_stmt_t").
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#endif

#include "dbsql_int.h"
#include "inc/vdbe_int.h"


/*
 * When debugging the code generator in a symbolic debugger, one can
 * set the dbsql_vdbe_add_op_trace to 1 and all opcodes will be printed
 * as they are added to the instruction stream.
 */
#ifndef NDEBUG /* TODO: if DEBUG and DIAGNOSTIC (?) */
int dbsql_vdbe_add_op_trace = 0;
#endif


/*
 * __vdbe_create --
 *	Create a new virtual database engine.
 *
 * PUBLIC: vdbe_t *__vdbe_create __P((DBSQL *));
 */
vdbe_t *
__vdbe_create(dbp)
	DBSQL *dbp;
{
	vdbe_t *vm;
	if (__dbsql_calloc(dbp, 1, sizeof(vdbe_t), &vm) == ENOMEM)
		return 0;
	vm->db = dbp;
	if (dbp->pVdbe) {
		dbp->pVdbe->pPrev = vm;
	}
	vm->pNext = dbp->pVdbe;
	vm->pPrev = 0;
	dbp->pVdbe = vm;
	vm->magic = VDBE_MAGIC_INIT;
	return vm;
}

/*
 * __vdbe_trace --
 *	Turn tracing on or off.
 *
 * PUBLIC: void __vdbe_trace __P((vdbe_t *, FILE *));
 */
void
__vdbe_trace(vm, trace)
	vdbe_t *vm;
	FILE *trace;
{
	vm->trace = trace;
}

/*
 * __vdbe_add_op --
 *	Add a new instruction to the list of instructions current in the
 *	VDBE.  Return the address of the new instruction.
 *
 *	Parameters:
 *
 *	   p               Pointer to the VDBE
 *
 *	   op              The opcode for this instruction
 *
 *	   p1, p2          First two of the three possible operands.
 *
 *	Use the __vdbe_resolve_label() function to fix an address and
 *	the __vdbe_change_p3() function to change the value of the P3
 *	operand.
 *
 * PUBLIC: int __vdbe_add_op __P((vdbe_t *, int, int, int));
 */
int
__vdbe_add_op(vm, op, p1, p2)
	vdbe_t *vm;
	int op;
	int p1;
	int p2;
{
	int i;

	i = vm->nOp;
	vm->nOp++;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (i >= vm->nOpAlloc) {
		int old_size = vm->nOpAlloc;
		vdbe_op_t *new;
		vm->nOpAlloc = (vm->nOpAlloc * 2) + 100;
		if (__dbsql_realloc(NULL, (vm->nOpAlloc * sizeof(vdbe_op_t)),
				 &vm->aOp) == ENOMEM) {
			vm->nOpAlloc = old_size;
			return 0;
		}
		memset(&vm->aOp[old_size], 0,
		       (vm->nOpAlloc - old_size) * sizeof(vdbe_op_t));
	}
	vm->aOp[i].opcode = op;
	vm->aOp[i].p1 = p1;
	if (p2 < 0 && (-1 - p2) < vm->nLabel && vm->aLabel[-1 -p2] >= 0) {
		p2 = vm->aLabel[-1 - p2];
	}
	vm->aOp[i].p2 = p2;
	vm->aOp[i].p3 = 0;
	vm->aOp[i].p3type = P3_NOTUSED;
#ifndef NDEBUG
	if (dbsql_vdbe_add_op_trace)
		__vdbe_print_op(0, i, &vm->aOp[i]);
#endif
	return i;
}

/*
 * __vdbe_make_label --
 *	Create a new symbolic label for an instruction that has yet to be
 *	coded.  The symbolic label is really just a negative number.  The
 *	label can be used as the P2 value of an operation.  Later, when
 *	the label is resolved to a specific address, the VDBE will scan
 *	through its operation list and change all values of P2 which match
 *	the label into the resolved address.
 *
 *	The VDBE knows that a P2 value is a label because labels are
 *	always negative and P2 values are suppose to be non-negative.
 *	Hence, a negative P2 value is a label that has yet to be resolved.
 *
 * PUBLIC: int __vdbe_make_label __P((vdbe_t *));
 */
int
__vdbe_make_label(vm)
	vdbe_t *vm;
{
	int i;
	i = vm->nLabel++;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (i >= vm->nLabelAlloc) {
		int *new;
		vm->nLabelAlloc = (vm->nLabelAlloc * 2) + 10;
		if (__dbsql_realloc(NULL, vm->nLabelAlloc * sizeof(vm->aLabel[0]),
				 &vm->aLabel) == ENOMEM) {
			__dbsql_free(NULL, vm->aLabel);
		}
	}
	if (vm->aLabel == 0) {
		vm->nLabel = 0;
		vm->nLabelAlloc = 0;
		return 0;
	}
	vm->aLabel[i] = -1;
	return -1 - i;
}

/*
 * __vdbe_resolve_label --
 *	Resolve label "x" to be the address of the next instruction to
 *	be inserted.  The parameter "x" must have been obtained from
 *	a prior call to __vdbe_make_label().
 *
 * PUBLIC: void __vdbe_resolve_label __P((vdbe_t *, int));
 */
void
__vdbe_resolve_label(vm, x)
	vdbe_t *vm;
	int x;
{
	int j;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (x < 0 && (-x) <= vm->nLabel && vm->aOp) {
		if (vm->aLabel[-1 - x] == vm->nOp)
			return;
		DBSQL_ASSERT(vm->aLabel[-1 - x] < 0);
		vm->aLabel[-1 - x] = vm->nOp;
		for (j = 0; j < vm->nOp; j++) {
			if (vm->aOp[j].p2 == x)
				vm->aOp[j].p2 = vm->nOp;
		}
	}
}

/*
 * __vdbe_current_addr --
 *	Return the address of the next instruction to be inserted.
 *
 * PUBLIC: int __vdbe_current_addr __P((vdbe_t *));
 */
int
__vdbe_current_addr(vm)
	vdbe_t *vm;
{
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	return vm->nOp;
}

/*
 * __vdbe_add_op_list --
 *	Add a whole list of operations to the operation stack.  Return the
 *	address of the first operation added.
 *
 * PUBLIC: int __vdbe_add_op_list __P((vdbe_t *, int, const vdbe_op_t *));
 */
int
__vdbe_add_op_list(vm, num_op, op)
	vdbe_t *vm;
	int num_op;
	const vdbe_op_t *op;
{
	int addr;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm->nOp + num_op >= vm->nOpAlloc) {
		int old_size = vm->nOpAlloc;
		vdbe_op_t *new;
		vm->nOpAlloc = (vm->nOpAlloc * 2) + num_op + 10;
		if (__dbsql_realloc(NULL, vm->nOpAlloc * sizeof(vdbe_op_t),
				 &vm->aOp) == ENOMEM) {
			vm->nOpAlloc = old_size;
			return 0;
		}
		memset(&vm->aOp[old_size], 0,
		       (vm->nOpAlloc - old_size) * sizeof(vdbe_op_t));
	}
	addr = vm->nOp;
	if (num_op > 0) {
		int i;
		for (i = 0; i < num_op; i++) {
			int p2 = op[i].p2;
			vm->aOp[i + addr] = op[i];
			if (p2 < 0)
				vm->aOp[i + addr].p2 = addr + ADDR(p2);
			vm->aOp[i + addr].p3type = op[i].p3 ?
				P3_STATIC : P3_NOTUSED;
#ifndef NDEBUG
			if (dbsql_vdbe_add_op_trace) {
				__vdbe_print_op(0, i + addr, &vm->aOp[i+addr]);
			}
#endif
		}
		vm->nOp += num_op;
	}
	return addr;
}

/*
 * __vdbe_change_p1 --
 *	Change the value of the P1 operand for a specific instruction.
 *	This routine is useful when a large program is loaded from a
 *	static array using __vdbe_add_op_list but we want to make a
 *	few minor changes to the program.
 *
 * PUBLIC: void __vdbe_change_p1 __P((vdbe_t *, int, int));
 */
void
__vdbe_change_p1(vm, addr, val)
	vdbe_t *vm;
	int addr;
	int val;
{
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm && addr >= 0 && vm->nOp > addr && vm->aOp) {
		vm->aOp[addr].p1 = val;
	}
}

/*
 * __vdbe_change_p2 --
 *	Change the value of the P2 operand for a specific instruction.
 *	This routine is useful for setting a jump destination.
 *
 * PUBLIC: void __vdbe_change_p2 __P((vdbe_t *, int, int));
 */
void
__vdbe_change_p2(vm, addr, val)
	vdbe_t *vm;
	int addr;
	int val;
{
	DBSQL_ASSERT(val >= 0);
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm && addr >= 0 && vm->nOp > addr && vm->aOp) {
		vm->aOp[addr].p2 = val;
	}
}

/*
 * __vdbe_change_p3 --
 *	Change the value of the P3 operand for a specific instruction.
 *	This routine is useful when a large program is loaded from a
 *	static array using __vdbe_add_op_list but we want to make a
 *	few minor changes to the program.
 *
 *	If n>=0 then the P3 operand is dynamic, meaning that a copy of
 *	the string is made into memory obtained from __dbsql_calloc().
 *	A value of n==0 means copy bytes of 'p3' up to and including the
 *	first null byte.  If n>0 then copy n+1 bytes of zP3.
 *
 *	If n==P3_STATIC  it means that zP3 is a pointer to a constant static
 *	string and we can just copy the pointer.  n==P3_POINTER means 'p3' is
 *	a pointer to some object other than a string.
 *
 *	If addr<0 then change P3 on the most recently inserted instruction.
 *
 * PUBLIC: void __vdbe_change_p3 __P((vdbe_t *, int, const char *, int));
 */
void
__vdbe_change_p3(vm, addr, p3, n)
	vdbe_t *vm;
	int addr;
	const char *p3;
	int n;
{
	vdbe_op_t *op;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm == 0 || vm->aOp == 0)
		return;
	if (addr < 0 || addr >= vm->nOp) {
		addr = vm->nOp - 1;
		if (addr < 0)
			return;
	}
	op = &vm->aOp[addr];
	if (op->p3 && op->p3type == P3_DYNAMIC) {
		__dbsql_free(NULL, op->p3);
		op->p3 = 0;
	}
	if (p3 == 0) {
		op->p3 = 0;
		op->p3type = P3_NOTUSED;
	} else if (n < 0) {
		op->p3 = (char*)p3;
		op->p3type = n;
	} else {
		__str_nappend(&op->p3, p3, n, NULL);
		op->p3type = P3_DYNAMIC;
	}
}

/*
 * __vdbe_dequote_p3 --
 *	If the P3 operand to the specified instruction appears
 *	to be a quoted string token, then this procedure removes 
 *	the quotes.
 *
 *	The quoting operator can be either a grave ascent (ASCII 0x27)
 *	or a double quote character (ASCII 0x22).  Two quotes in a row
 *	resolve to be a single actual quote character within the string.
 *
 * PUBLIC: void __vdbe_dequote_p3 __P((vdbe_t *, int));
 */
void
__vdbe_dequote_p3(vm, addr)
	vdbe_t *vm;
	int addr;
{
	vdbe_op_t *op;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm->aOp == 0)
		return;
	if (addr < 0 || addr >= vm->nOp) {
		addr = vm->nOp - 1;
		if (addr < 0)
			return;
	}
	op = &vm->aOp[addr];
	if (op->p3 == 0 || op->p3[0] == 0)
		return;
	if (op->p3type == P3_POINTER)
		return;
	if (op->p3type != P3_DYNAMIC) {
		__dbsql_strdup(NULL, op->p3, &op->p3);
		op->p3type = P3_DYNAMIC;
	}
	__str_unquote(op->p3);
}

/*
 * __vdbe_compress_space --
 *	On the P3 argument of the given instruction, change all
 *	strings of whitespace characters into a single space and
 *	delete leading and trailing whitespace.
 *
 * PUBLIC: void __vdbe_compress_space __P((vdbe_t *, int));
 */
void
__vdbe_compress_space(vm, addr)
	vdbe_t *vm;
	int addr;
{
	unsigned char *z;
	int i, j;
	vdbe_op_t *op;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm->aOp == 0 || addr < 0 || addr >= vm->nOp)
		return;
	op = &vm->aOp[addr];
	if (op->p3type == P3_POINTER) {
		return;
	}
	if (op->p3type != P3_DYNAMIC) {
		__dbsql_strdup(NULL, op->p3, &op->p3);
		op->p3type = P3_DYNAMIC;
	}
	z = (unsigned char*)op->p3;
	if (z == 0)
		return;
	i = j = 0;
	while(isspace(z[i])) {
		i++;
	}
	while(z[i]) {
		if (isspace(z[i])) {
			z[j++] = ' ';
			while(isspace(z[++i])) {}
		} else {
			z[j++] = z[i++];
		}
	}
	while(j > 0 && isspace(z[j - 1])) {
		j--;
	}
	z[j] = 0;
}

/*
 * __vdbe_find_op --
 *	Search for the current program for the given opcode and P2
 *	value.  Return the address plus 1 if found and 0 if not found.
 *
 * PUBLIC: int __vdbe_find_op __P((vdbe_t *, int, int));
 */
int
__vdbe_find_op(vm, op, p2)
	vdbe_t *vm;
	int op;
	int p2;
{
	int i;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	for (i = 0; i < vm->nOp; i++) {
		if (vm->aOp[i].opcode == op && vm->aOp[i].p2 == p2)
			return i + 1;
	}
	return 0;
}

/*
 * __vdbe_get_op --
 *	Return the opcode for a given address.
 *
 * PUBLIC: vdbe_op_t *__vdbe_get_op __P((vdbe_t *, int));
 */
vdbe_op_t *
__vdbe_get_op(vm, addr)
	vdbe_t *vm;
	int addr;
{
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	DBSQL_ASSERT(addr >= 0 && addr < vm->nOp);
	return &vm->aOp[addr];
}

/*
 * dbsql_set_result_string --
 *	The following group or routines are employed by installable functions
 *	to return their results.
 *
 *	The dbsql_set_result_string() routine can be used to return a string
 *	value or to return a NULL.  To return a NULL, pass in NULL for
 *	'result'.
 *	A copy is made of the string before this routine returns so it is safe
 *	to pass in an ephemeral string.
 *
 *	dbsql_set_result_error() works like dbsql_set_result_string() except
 *	that it signals a fatal error.  The string argument, if any, is the
 *	error message.  If the argument is NULL a generic substitute error
 *	message is used.
 *
 *	The dbsql_set_result_int() and dbsql_set_result_double() set the
 *	return value of the user function to an integer or a double.
 *
 *	These routines are defined here in vdbe_aux.c because they depend on
 *	knowing the internals of the dbsql_func_t structure which is only
 *	defined in this source file.
 *
 * EXTERN: char *dbsql_set_result_string __P((dbsql_func_t *, const char *,
 * EXTERN:                               int));
 */
char *
dbsql_set_result_string(p, result, n)
	dbsql_func_t *p;
	const char *result;
	int n;
{
	DBSQL_ASSERT(!p->isStep);
	if (p->s.flags & MEM_Dyn) {
		__dbsql_free(NULL, p->s.z);
	}
	if (result == 0) {
		p->s.flags = MEM_Null;
		n = 0;
		p->s.z = 0;
		p->s.n = 0;
	} else {
		if (n < 0)
			n = strlen(result);
		if (n < NBFS - 1) {
			memcpy(p->s.zShort, result, n);
			p->s.zShort[n] = 0;
			p->s.flags = MEM_Str | MEM_Short;
			p->s.z = p->s.zShort;
		} else {
			if (__dbsql_calloc(NULL, 1, n + 1, &p->s.z) != ENOMEM) {
				memcpy(p->s.z, result, n);
				p->s.z[n] = 0;
			}
			p->s.flags = MEM_Str | MEM_Dyn;
		}
		p->s.n = n + 1;
	}
	return p->s.z;
}

/*
 * dbsql_set_result_null --
 *
 * EXTERN: void dbsql_set_result_null __P((dbsql_func_t *));
 */
void
dbsql_set_result_null(p)
	dbsql_func_t *p;
{
	DBSQL_ASSERT(!p->isStep);
	if (p->s.flags & MEM_Dyn) {
		__dbsql_free(NULL, p->s.z);
	}
	p->s.flags = MEM_Null;
	p->s.z = 0;
	p->s.n = 0;
}

/*
 * dbsql_set_result_int --
 *
 * EXTERN: void dbsql_set_result_int __P((dbsql_func_t *, int));
 */
void
dbsql_set_result_int(p, result)
	dbsql_func_t *p;
	int result;
{
	DBSQL_ASSERT(!p->isStep);
	if (p->s.flags & MEM_Dyn) {
		__dbsql_free(NULL, p->s.z);
	}
	p->s.i = result;
	p->s.flags = MEM_Int;
}

/*
 * dbsql_set_result_int64 --
 *
 * EXTERN: void dbsql_set_result_int64 __P((dbsql_func_t *, int64_t));
 */
void
dbsql_set_result_int64(p, result) /*TODO*/
	dbsql_func_t *p;
	int64_t result;
{
	DBSQL_ASSERT(0);
}

/*
 * dbsql_set_result_double --
 *
 * EXTERN: void dbsql_set_result_double __P((dbsql_func_t *, double));
 */
void
dbsql_set_result_double(p, result)
	dbsql_func_t *p;
	double result;
{
	DBSQL_ASSERT(!p->isStep);
	if (p->s.flags & MEM_Dyn) {
		__dbsql_free(NULL, p->s.z);
	}
	p->s.r = result;
	p->s.flags = MEM_Real;
}

/*
 * dbsql_set_result_error --
 *
 * EXTERN: void dbsql_set_result_error __P((dbsql_func_t *, const char *,
 * EXTERN:                             int));
 */
void
dbsql_set_result_error(p, msg, n)
	dbsql_func_t *p;
	const char *msg;
	int n;
{
	DBSQL_ASSERT(!p->isStep);
	dbsql_set_result_string(p, msg, n);
	p->isError = 1;
}

/*
 * dbsql_set_result_blob --
 *
 * EXTERN: void dbsql_set_result_blob __P((dbsql_func_t *, const void *,
 * EXTERN:                            size_t, void(*)(void*)));
 */
void
dbsql_set_result_blob(p, result, size, finalize) /*TODO*/
	dbsql_func_t *p;
	const void *result;
	size_t size;
	void(*finalize)(void*);
{
	DBSQL_ASSERT(0);
}

/*
 * dbsql_set_result_varchar --
 *
 * EXTERN: void dbsql_set_result_varchar __P((dbsql_func_t *, const char *,
 * EXTERN:                            size_t, void(*)(void*)));
 */
void
dbsql_set_result_varchar(p, result, size, finalize) /*TODO*/
	dbsql_func_t *p;
	const char *result;
	size_t size;
	void(*finalize)(void*);
{
	DBSQL_ASSERT(0);
}

/*
 * dbsql_user_data --
 *	Extract the user data from a dbsql_func_t structure and return a
 *	pointer to it.
 *
 * EXTERN: void *dbsql_user_data __P((dbsql_func_t *));
 */
void *
dbsql_user_data(p)
	dbsql_func_t *p;
{
	DBSQL_ASSERT(p && p->pFunc);
	return p->pFunc->pUserData;
}

/*
 * dbsql_aggregate_context --
 *	Allocate or return the aggregate context for a user function.  A new
 *	context is allocated on the first call.  Subsequent calls return the
 *	same context that was returned on prior calls.
 *
 *	This routine is defined here in vdbe.c because it depends on knowing
 *	the internals of the dbsql_func_t structure which is only defined in
 *	this source file.
 *
 * EXTERN: void *dbsql_aggregate_context __P((dbsql_func_t *, int));
 */
void *
dbsql_aggregate_context(p, num_bytes)
	dbsql_func_t *p;
	int num_bytes;
{
	DBSQL_ASSERT(p && p->pFunc && p->pFunc->xStep);
	if (p->pAgg == 0) {
		if (num_bytes <= NBFS) {
			p->pAgg = (void *)p->s.z;
			memset(p->pAgg, 0, num_bytes);
		} else {
			__dbsql_calloc(NULL, 1, num_bytes, &p->pAgg);
		}
	}
	return p->pAgg;
}

/*
 * dbsql_aggregate_count --
 *	Return the number of times the Step function of a aggregate has been 
 *	called.
 *
 *	This routine is defined here in vdbe.c because it depends on knowing
 *	the internals of the dbsql_func_t structure which is only defined in
 *	this source file.
 *
 * EXTERN: int dbsql_aggregate_count __P((dbsql_func_t *));
 */
int
dbsql_aggregate_count(p)
	dbsql_func_t *p;
{
	DBSQL_ASSERT(p && p->pFunc && p->pFunc->xStep);
	return p->cnt;
}

#if !defined(NDEBUG) || defined(VDBE_PROFILE)
/*
 * __vdbe_print_op --
 *	Print a single opcode.  This routine is used for debugging only.
 *
 * PUBLIC: void __vdbe_print_op __P((FILE *, int, vdbe_op_t *));
 */
void
__vdbe_print_op(out, pc, op)
	FILE *out;
	int pc;
	vdbe_op_t *op;
{
	char *p3;
	char ptr[40];
	if (op->p3type == P3_POINTER) {
		sprintf(ptr, "ptr(%p)", op->p3);
		p3 = ptr;
	} else {
		p3 = op->p3;
	}
	if (out == 0)
		out = stdout;
	fprintf(out,"%4d %-12s %4d %4d %s\n",
		pc, __opcode_names[op->opcode],
		op->p1, op->p2, p3 ? p3 : "");
	fflush(out);
}
#endif

/*
 * __vdbe_list --
 *	Give a listing of the program in the virtual machine.
 *
 *	The interface is the same as __vdbe_exec().  But instead of
 *	running the code, it invokes the callback once for each instruction.
 *	This feature is used to implement "EXPLAIN".
 *
 * PUBLIC: int __vdbe_list __P((vdbe_t *));
 */
int
__vdbe_list(vm)
	vdbe_t *vm;
{
	int i;
	DBSQL *db = vm->db;
	static char *column_names[] = {
		"addr", "opcode", "p1",  "p2",  "p3", 
		"int",  "text",   "int", "int", "text",
		0
	};

	DBSQL_ASSERT(vm->popStack == 0);
	DBSQL_ASSERT(vm->explain);
	vm->azColName = column_names;
	vm->azResColumn = vm->zArgv;
	for (i = 0; i < 5; i++) {
		vm->zArgv[i] = vm->aStack[i].zShort;
	}
	vm->rc = DBSQL_SUCCESS;
	for (i = vm->pc; vm->rc == DBSQL_SUCCESS && i < vm->nOp; i++) {
		if (db->flags & DBSQL_Interrupt) {
			db->flags &= ~DBSQL_Interrupt;
			if (db->magic != DBSQL_STATUS_BUSY) {
				vm->rc = DBSQL_MISUSE;
			} else {
				vm->rc = DBSQL_INTERRUPTED;
			}
			__str_append(&vm->zErrMsg, dbsql_strerror(vm->rc),
				     (char*)0);
			break;
		}
		sprintf(vm->zArgv[0], "%d", i);
		sprintf(vm->zArgv[2], "%d", vm->aOp[i].p1);
		sprintf(vm->zArgv[3], "%d", vm->aOp[i].p2);
		if (vm->aOp[i].p3type == P3_POINTER) {
			sprintf(vm->aStack[4].zShort, "ptr(%p)",
				vm->aOp[i].p3);
			vm->zArgv[4] = vm->aStack[4].zShort;
		} else {
			vm->zArgv[4] = vm->aOp[i].p3;
		}
		vm->zArgv[1] = __opcode_names[vm->aOp[i].opcode];
		if (vm->xCallback == 0) {
			vm->pc = i + 1;
			vm->azResColumn = vm->zArgv;
			vm->nResColumn = 5;
			return DBSQL_ROW;
		}
		if (__safety_off(db)) {
			vm->rc = DBSQL_MISUSE;
			break;
		}
		if (vm->xCallback(vm->pCbArg, 5, vm->zArgv, vm->azColName)) {
			vm->rc = DBSQL_ABORT;
		}
		if (__safety_on(db)) {
			vm->rc = DBSQL_MISUSE;
		}
	}
	return vm->rc == DBSQL_SUCCESS ? DBSQL_DONE : DBSQL_ERROR;
}

/*
 * __vdbe_make_ready --
 *	Prepare a virtual machine for execution.  This involves things such
 *	as allocating stack space and initializing the program counter.
 *	After the VDBE has be prepped, it can be executed by one or more
 *	calls to __vdbe_exec().  
 *
 *	The behavior of __vdbe_exec() is influenced by the parameters to
 *	this routine.  If 'callback' is NULL, then __vdbe_exec() will return
 *	with DBSQL_ROW whenever there is a row of the result set ready
 *	to be delivered.  vm->azResColumn will point to the row and 
 *	vm->nResColumn gives the number of columns in the row.  If 'callback'
 *	is not NULL, then the 'callback()' routine is invoked to process each
 *	row in the result set.
 *
 * PUBLIC: void __vdbe_make_ready __P((vdbe_t *, int, dbsql_callback, void *,
 * PUBLIC:                        int));
 *
 * vm				The VDBE
 * num_var			Number of '?' seen in the SQL statement
 * callback			Result callback
 * callback_arg			1st argument to callback()
 * explain_p			True if the EXPLAIN keywords is present
 */
void
__vdbe_make_ready(vm, num_var, callback, callback_arg, explain_p)
	vdbe_t *vm;
	int num_var;
	dbsql_callback callback;
	void *callback_arg;
	int explain_p;
{
	int n;

	DBSQL_ASSERT(vm != 0);
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);

	/*
	 * Add a HALT instruction to the very end of the program.
	 */
	if (vm->nOp == 0 ||
	    (vm->aOp && vm->aOp[vm->nOp - 1].opcode != OP_Halt)) {
		__vdbe_add_op(vm, OP_Halt, 0, 0);
	}

	/*
	 * No instruction ever pushes more than a single element onto the
	 * stack.  And the stack never grows on successive executions of the
	 * same loop.  So the total number of instructions is an upper bound
	 * on the maximum stack depth required.
	 *
	 * Allocation all the stack space we will ever need.
	 */
	if (vm->aStack == 0) {
		vm->nVar = num_var;
		DBSQL_ASSERT(num_var >= 0);
		n = explain_p ? 10 : vm->nOp;
		__dbsql_calloc(NULL, 1,
			 /* aStack and zArgv */
			 (n * (sizeof(vm->aStack[0]) + (2 * sizeof(char*))) +
			 /* azVar, anVar, abVar */
			 (vm->nVar * (sizeof(char*)) + sizeof(int) + 1)),
			 &vm->aStack);
			vm->zArgv = (char**)&vm->aStack[n];
			vm->azColName = (char**)&vm->zArgv[n];
			vm->azVar = (char**)&vm->azColName[n];
			vm->anVar = (int*)&vm->azVar[vm->nVar];
			vm->abVar = (u_int8_t*)&vm->anVar[vm->nVar];
	}

	__hash_init(&vm->agg.hash, DBSQL_HASH_BINARY, 0);
	vm->agg.pSearch = 0;
#ifdef MEMORY_DEBUG
	if (__os_file_exists("vdbe_trace")){
		vm->trace = stdout;
	}
#endif
	vm->pTos = &vm->aStack[-1];
	vm->pc = 0;
	vm->rc = DBSQL_SUCCESS;
	vm->uniqueCnt = 0;
	vm->returnDepth = 0;
	vm->errorAction = OE_Abort;
	vm->undoTransOnError = 0;
	vm->xCallback = callback;
	vm->pCbArg = callback_arg;
	vm->popStack =  0;
	vm->explain |= explain_p;
	vm->magic = VDBE_MAGIC_RUN;
#ifdef VDBE_PROFILE
	{
		int i;
		for (i = 0; i < vm->nOp; i++) {
			vm->aOp[i].cnt = 0;
			vm->aOp[i].cycles = 0;
		}
	}
#endif
}


/*
 * __vdbe_sorter_reset --
 *	Remove any elements that remain on the sorter for the VDBE given.
 *
 * PUBLIC: void __vdbe_sorter_reset __P((vdbe_t *));
 */
void
__vdbe_sorter_reset(vm)
	vdbe_t *vm;
{
	while(vm->pSort) {
		sorter_t *s = vm->pSort;
		vm->pSort = s->pNext;
		__dbsql_free(NULL, s->zKey);
		__dbsql_free(NULL, s->pData);
		__dbsql_free(NULL, s);
	}
}

/*
 * __vdbe_agg_reset --
 *	Reset an Agg structure.  Delete all its contents. 
 *
 *	For installable aggregate functions, if the step function has been
 *	called, make sure the finalizer function has also been called.  The
 *	finalizer might need to free memory that was allocated as part of its
 *	private context.  If the finalizer has not been called yet, call it
 *	now.
 *
 * PUBLIC: void __vdbe_agg_reset __P((agg_t *));
 */
void
__vdbe_agg_reset(agg)
	agg_t *agg;
{
	int i;
	hash_ele_t *p;
	for (p = __hash_first(&agg->hash); p; p = __hash_next(p)) {
		agg_elem_t *elem = __hash_data(p);
		DBSQL_ASSERT(agg->apFunc != 0);
		for (i = 0; i < agg->nMem; i++) {
			mem_t *mem = &elem->aMem[i];
			if (agg->apFunc[i] && (mem->flags & MEM_AggCtx) != 0) {
				dbsql_func_t ctx;
				ctx.pFunc = agg->apFunc[i];
				ctx.s.flags = MEM_Null;
				ctx.pAgg = mem->z;
				ctx.cnt = mem->i;
				ctx.isStep = 0;
				ctx.isError = 0;
				(*agg->apFunc[i]->xFinalize)(&ctx);
				if (mem->z != 0 && mem->z != mem->zShort) {
					__dbsql_free(NULL, mem->z);
				}
			} else if (mem->flags & MEM_Dyn) {
				__dbsql_free(NULL, mem->z);
			}
		}
		__dbsql_free(NULL, elem);
	}
	__hash_clear(&agg->hash);
	__dbsql_free(NULL, agg->apFunc);
	agg->apFunc = 0;
	agg->pCurrent = 0;
	agg->pSearch = 0;
	agg->nMem = 0;
}

/*
 * __vdbe_keylist_free --
 *	Delete a keylist
 *
 * PUBLIC: void __vdbe_keylist_free __P((keylist_t *));
 */
void
__vdbe_keylist_free(p)
	keylist_t *p;
{
	while(p) {
		keylist_t *next = p->pNext;
		__dbsql_free(NULL, p);
		p = next;
	}
}

/*
 * __vdbe_cleanup_cursor --
 *	Close a cursor and release all the resources that cursor happens
 *	to hold.
 *
 * PUBLIC: void __vdbe_cleanup_cursor __P((cursor_t *));
 */
void
__vdbe_cleanup_cursor(cx)
	cursor_t *cx;
{
	if (cx->pCursor) {
		__sm_close_cursor(cx->pCursor);
	}
	if (cx->pBt) {
		__sm_close_db(cx->pBt);
	}
	__dbsql_free(NULL, cx->pData);
	memset(cx, 0, sizeof(cursor_t));
}

/*
 * __close_all_cursors --
 *	Close all cursors
 *
 * STATIC: static void __close_all_cursors __P((vdbe_t *));
 */
static void
__close_all_cursors(vm)
	vdbe_t *vm;
{
	int i;
	for (i = 0; i < vm->nCursor; i++) {
		__vdbe_cleanup_cursor(&vm->aCsr[i]);
	}
	__dbsql_free(NULL, vm->aCsr);
	vm->aCsr = 0;
	vm->nCursor = 0;
}

/*
 * __cleanup --
 *	Clean up the VM after execution.
 *
 *	This routine will automatically close any cursors, lists, and/or
 *	sorters that were left open.  It also deletes the values of
 *	variables in the azVariable[] array.
 *
 * STATIC: static void __cleanup __P((vdbe_t *));
 */
static void
__cleanup(vm)
	vdbe_t *vm;
{
	int i;
	if (vm->aStack) {
		mem_t *tos = vm->pTos;
		while(tos >= vm->aStack) {
			if (tos->flags & MEM_Dyn) {
				__dbsql_free(NULL, tos->z);
			}
			tos--;
		}
		vm->pTos = tos;
	}
	__close_all_cursors(vm);
	if (vm->aMem) {
		for(i = 0; i < vm->nMem; i++) {
			if (vm->aMem[i].flags & MEM_Dyn) {
				__dbsql_free(NULL, vm->aMem[i].z);
			}
		}
	}
	__dbsql_free(NULL, vm->aMem);
	vm->aMem = 0;
	vm->nMem = 0;
	if (vm->pList) {
		__vdbe_keylist_free(vm->pList);
		vm->pList = 0;
	}
	__vdbe_sorter_reset(vm);
	if (vm->pFile) {
		if (vm->pFile != stdin)
			fclose(vm->pFile);
		vm->pFile = 0;
	}
	if (vm->azField) {
		__dbsql_free(NULL, vm->azField);
		vm->azField = 0;
	}
	vm->nField = 0;
	if (vm->zLine) {
		__dbsql_free(NULL, vm->zLine);
		vm->zLine = 0;
	}
	vm->nLineAlloc = 0;
	__vdbe_agg_reset(&vm->agg);
	if (vm->aSet) {
		for(i = 0; i < vm->nSet; i++) {
			__hash_clear(&vm->aSet[i].hash);
		}
	}
	__dbsql_free(NULL, vm->aSet);
	vm->aSet = 0;
	vm->nSet = 0;
	if (vm->keylistStack) {
		int ii;
		for(ii = 0; ii < vm->keylistStackDepth; ii++) {
			__vdbe_keylist_free(vm->keylistStack[ii]);
		}
		__dbsql_free(NULL, vm->keylistStack);
		vm->keylistStackDepth = 0;
		vm->keylistStack = 0;
	}
	__dbsql_free(NULL, vm->zErrMsg);
	vm->zErrMsg = 0;
}

/*
 * __vdbe_reset --
 *	Clean up a VDBE after execution but do not delete the VDBE just yet.
 *	Write any error messages into *pzErrMsg.  Return the result code.
 *
 *	After this routine is run, the VDBE should be ready to be executed
 *	again.
 * PUBLIC: int __vdbe_reset __P((vdbe_t *, char **));
 */
int
__vdbe_reset(vm, err_msgs)
	vdbe_t *vm;
	char **err_msgs;
{
	int i;
	DBSQL *db = vm->db;

	if (vm->magic != VDBE_MAGIC_RUN && vm->magic != VDBE_MAGIC_HALT) {
		__str_append(err_msgs, dbsql_strerror(DBSQL_MISUSE), (char*)0);
		return DBSQL_MISUSE;
	}
	if (vm->zErrMsg) {
		if (err_msgs && *err_msgs == 0) {
			*err_msgs = vm->zErrMsg;
		} else {
			__dbsql_free(NULL, vm->zErrMsg);
		}
		vm->zErrMsg = 0;
	}
	__cleanup(vm);
	if (vm->rc != DBSQL_SUCCESS) {
		switch(vm->errorAction) {
		case OE_Abort:
			/* FALLTHROUG */
		case OE_Rollback:
			/* TODO __sm_abort_txn(db); */
			db->flags &= ~DBSQL_InTrans;
			db->onError = OE_Default;
			break;
		default:
			if (vm->undoTransOnError) {
				/* TODO __sm_abort_txn(db); */
				db->flags &= ~DBSQL_InTrans;
				db->onError = OE_Default;
			}
			break;
		}
		__rollback_internal_changes(db);
	}
	DBSQL_ASSERT(vm->pTos < &vm->aStack[vm->pc]);
#ifdef VDBE_PROFILE
	{
		FILE *out = fopen("vdbe_profile.out", "a");
		if (out) {
			int i;
			fprintf(out, "---- ");
			for (i = 0; i < p->nOp; i++) {
				fprintf(out, "%02x", vm->aOp[i].opcode);
			}
			fprintf(out, "\n");
			for(i = 0; i < vm->nOp; i++) {
				fprintf(out, "%6d %10lld %8lld ",
					vm->aOp[i].cnt,
					vm->aOp[i].cycles,
					(vm->aOp[i].cnt > 0) ?
					vm->aOp[i].cycles / vm->aOp[i].cnt :0);
				__vdbe_print_op(out, i, &vm->aOp[i]);
			}
			fclose(out);
		}
	}
#endif
	vm->magic = VDBE_MAGIC_INIT;
	return vm->rc;
}

/*
 * __vdbe_finalize --
 *	Clean up and delete a VDBE after execution.  Return an integer which is
 *	the result code.  Write any error message text into *err_msgs.
 *
 * PUBLIC: int __vdbe_finalize __P((vdbe_t *, char **));
 */
int
__vdbe_finalize(vm, err_msgs)
	vdbe_t *vm;
	char **err_msgs;
{
	int rc;
	DBSQL *db;

	if (vm->magic != VDBE_MAGIC_RUN && vm->magic != VDBE_MAGIC_HALT) {
		__str_append(err_msgs, dbsql_strerror(DBSQL_MISUSE), (char*)0);
		return DBSQL_MISUSE;
	}
	db = vm->db;
	rc = __vdbe_reset(vm, err_msgs);
	__vdbe_delete(vm);
	if (db->want_to_close && db->pVdbe == 0) {
		db->close(db);
	}
	return rc;
}

/*
 * __api_bind --
 *	Set the values of all variables.  Variable $1 in the original SQL will
 *	be the string azValue[0].  $2 will have the value azValue[1].  And
 *	so forth.  If a value is out of range (for example $3 when nValue==2)
 *	then its value will be NULL.
 *	This routine overrides any prior call.
 *
 * PUBLIC: int __api_bind __P((dbsql_stmt_t *, int, const char *, int,
 * PUBLIC:                int));
 */
int
__api_bind(p, i, val, len, copy)
	dbsql_stmt_t *p;
	int i;
	const char *val;
	int len;
	int copy;
{
	vdbe_t *vm = (vdbe_t*)p;
	if (vm->magic != VDBE_MAGIC_RUN || vm->pc != 0) {
		return DBSQL_MISUSE;
	}
	if (i < 1 || i > vm->nVar) {
		return DBSQL_RANGE;
	}
	i--;
	if (vm->abVar[i]) {
		__dbsql_free(NULL, vm->azVar[i]);
	}
	if (val == 0) {
		copy = 0;
		len = 0;
	}
	if (len < 0) {
		len = strlen(val) + 1;
	}
	if (copy) {
		if (__dbsql_calloc(NULL, 1, len, &vm->azVar[i]) != ENOMEM)
			memcpy(vm->azVar[i], val, len);
	} else {
		vm->azVar[i] = (char*)val;
	}
	vm->abVar[i] = copy;
	vm->anVar[i] = len;
	return DBSQL_SUCCESS;
}


/*
 * __vdbe_delete --
 *	Delete an entire VDBE.
 *
 * PUBLIC: void __vdbe_delete __P((vdbe_t *));
 */
void
__vdbe_delete(vm)
	vdbe_t *vm;
{
	int i;
	if (vm == 0)
		return;
	__cleanup(vm);
	if (vm->pPrev) {
		vm->pPrev->pNext = vm->pNext;
	} else {
		DBSQL_ASSERT(vm->db->pVdbe == vm );
		vm->db->pVdbe = vm->pNext;
	}
	if (vm->pNext) {
		vm->pNext->pPrev = vm->pPrev;
	}
	vm->pPrev = vm->pNext = 0;
	if (vm->nOpAlloc == 0) {
		vm->aOp = 0;
		vm->nOp = 0;
	}
	for (i = 0; i < vm->nOp; i++) {
		if (vm->aOp[i].p3type == P3_DYNAMIC) {
			__dbsql_free(NULL, vm->aOp[i].p3);
		}
	}
	for(i = 0; i < vm->nVar; i++) {
		if (vm->abVar[i])
			__dbsql_free(NULL, vm->azVar[i]);
	}
	__dbsql_free(NULL, vm->aOp);
	__dbsql_free(NULL, vm->aLabel);
	__dbsql_free(NULL, vm->aStack);
	vm->magic = VDBE_MAGIC_DEAD;
	__dbsql_free(NULL, vm);
}

/*
 * __vdbe_byte_swap --
 *	Convert an integer in between the native integer format and
 *	the bigEndian format used as the record number for tables.
 *
 *	The bigEndian format (most significant byte first) is used for
 *	record numbers so that records will sort into the correct order
 *	even though memcmp() is used to compare the keys.  On machines
 *	whose native integer format is little endian (ex: i486) the
 *	order of bytes is reversed.  On native big-endian machines
 *	(ex: Alpha, Sparc, Motorola) the byte order is the same.
 *
 *	This function is its own inverse.  In other words
 *
 *         X == byteSwap(byteSwap(X))
 *
 * PUBLIC: int __vdbe_byte_swap __P((int));
 */
int
__vdbe_byte_swap(x)
	int x;
{
	union {
		char buf[sizeof(int)];
		int i;
	} ux;
	ux.buf[3] = x&0xff;
	ux.buf[2] = (x>>8)&0xff;
	ux.buf[1] = (x>>16)&0xff;
	ux.buf[0] = (x>>24)&0xff;
	return ux.i;
}

/*
 * __vdbe_cursor_moveto --
 *	If a MoveTo operation is pending on the given cursor, then do that
 *	MoveTo now.  Return an error code.  If no MoveTo is pending, this
 *	routine does nothing and returns DBSQL_SUCCESS.
 *
 * PUBLIC: int __vdbe_cursor_moveto __P((cursor_t *));
 */
int
__vdbe_cursor_moveto(p)
	cursor_t *p;
{
	if (p->deferredMoveto) {
		int res;
#ifdef CONFIG_TEST
		extern int dbsql_search_count;
#endif
		__sm_moveto(p->pCursor, (char*)&p->movetoTarget,
			    sizeof(int), &res);
		p->lastRecno = KEY_TO_INT(p->movetoTarget);
		p->recnoIsValid = (res == 0);
		if (res < 0) {
			__sm_next(p->pCursor, &res);
		}
#ifdef CONFIG_TEST
		dbsql_search_count++;
#endif
		p->deferredMoveto = 0;
	}
	return DBSQL_SUCCESS;
}
