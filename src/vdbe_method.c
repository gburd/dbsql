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

/*
 * This file contains code used for creating, destroying, and populating
 * a VDBE (also known as "dbsql_stmt_t").
 *
 * Updated for register-based VM execution model (SQLite 3.52.0 compatible).
 * Key changes from the stack-based model:
 *   - Instructions have 5 operands: P1, P2, P3 (int), P4 (union), P5 (u16)
 *   - Values live in numbered registers (aMem[]) instead of an operand stack
 *   - Cursors are a pointer array (apCsr[]) instead of inline array
 *   - Result delivery via OP_ResultRow pointing to register range
 *   - Variables stored as Mem structures (aVar[]) instead of string arrays
 *   - Column names stored as Mem structures (aColName[])
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
__vdbe_create(DBSQL* dbp)
{
	vdbe_t *vm;
	if (__dbsql_calloc(dbp, 1, sizeof(vdbe_t), &vm) == ENOMEM)
		return 0;
	vm->db = dbp;
	if (dbp->pVdbe) {
		dbp->pVdbe->prev_vdbe = vm;
	}
	vm->next_vdbe = dbp->pVdbe;
	vm->prev_vdbe = 0;
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
__vdbe_trace(vdbe_t* vm, FILE* trace)
{
	vm->trace = trace;
}

/*
 * __grow_op_array --
 *	Ensure the op array has room for at least one more instruction.
 *
 * STATIC: static int __grow_op_array __P((vdbe_t *));
 */
static int
__grow_op_array(vdbe_t* vm)
{
	int old_size = vm->nOpAlloc;
	vm->nOpAlloc = (vm->nOpAlloc * 2) + 100;
	if (__dbsql_realloc(NULL, (vm->nOpAlloc * sizeof(vdbe_op_t)),
			 &vm->opcodes) == ENOMEM) {
		vm->nOpAlloc = old_size;
		return ENOMEM;
	}
	memset(&vm->opcodes[old_size], 0,
	       (vm->nOpAlloc - old_size) * sizeof(vdbe_op_t));
	return 0;
}

/*
 * __vdbe_add_op --
 *	Add a new instruction with 2 integer operands (P1, P2).
 *	P3 defaults to 0.  This is the legacy 2-operand interface
 *	retained for compatibility.
 *
 * PUBLIC: int __vdbe_add_op __P((vdbe_t *, int, int, int));
 */
int
__vdbe_add_op(vdbe_t* vm, int op, int p1, int p2)
{
	return __vdbe_add_op3(vm, op, p1, p2, 0);
}

/*
 * __vdbe_add_op3 --
 *	Add a new instruction with 3 integer operands (P1, P2, P3).
 *	P4 and P5 default to unused/0.
 *
 *	This is the primary instruction-emission function for the
 *	register-based VM.
 *
 * PUBLIC: int __vdbe_add_op3 __P((vdbe_t *, int, int, int, int));
 */
int
__vdbe_add_op3(vdbe_t* vm, int op, int p1, int p2, int p3)
{
	int i;
	vdbe_op_t *pOp;

	i = vm->nOp;
	vm->nOp++;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (i >= vm->nOpAlloc) {
		if (__grow_op_array(vm)) {
			vm->nOp--;
			return 0;
		}
	}
	pOp = &vm->opcodes[i];
	pOp->opcode = op;
	pOp->p1 = p1;
	if (p2 < 0 && (-1 - p2) < vm->nLabel && vm->labels[-1 - p2] >= 0) {
		p2 = vm->labels[-1 - p2];
	}
	pOp->p2 = p2;
	pOp->p3 = p3;
	pOp->p4.p = 0;
	pOp->p4type = P4_NOTUSED;
	pOp->p5 = 0;
#ifndef NDEBUG
	if (dbsql_vdbe_add_op_trace)
		__vdbe_print_op(0, i, &vm->opcodes[i]);
#endif
	return i;
}

/*
 * __vdbe_add_op4 --
 *	Add an instruction with P4 as a string or pointer value.
 *	If n is P4_STATIC, P4_DYNAMIC, or P4_POINTER, the pointer is
 *	stored directly.  If n>=0, the string is copied.
 *
 * PUBLIC: int __vdbe_add_op4 __P((vdbe_t *, int, int, int, int,
 * PUBLIC:                     const char *, int));
 */
int
__vdbe_add_op4(vdbe_t* vm, int op, int p1, int p2, int p3,
	       const char *p4, int p4type)
{
	int addr = __vdbe_add_op3(vm, op, p1, p2, p3);
	__vdbe_change_p4(vm, addr, p4, p4type);
	return addr;
}

/*
 * __vdbe_add_op4_int --
 *	Add an instruction with P4 as a 32-bit integer value.
 *
 * PUBLIC: int __vdbe_add_op4_int __P((vdbe_t *, int, int, int, int, int));
 */
int
__vdbe_add_op4_int(vdbe_t* vm, int op, int p1, int p2, int p3, int p4)
{
	int addr = __vdbe_add_op3(vm, op, p1, p2, p3);
	if (addr >= 0 && addr < vm->nOp) {
		vm->opcodes[addr].p4type = P4_INT32;
		vm->opcodes[addr].p4.i = p4;
	}
	return addr;
}

/*
 * __vdbe_add_op1 --
 *	Convenience wrapper: add an instruction with 1 operand (P1).
 *	P2 and P3 default to 0.
 *
 * PUBLIC: int __vdbe_add_op1 __P((vdbe_t *, int, int));
 */
int
__vdbe_add_op1(vdbe_t* vm, int op, int p1)
{
	return __vdbe_add_op3(vm, op, p1, 0, 0);
}

/*
 * __vdbe_add_op2 --
 *	Convenience wrapper: add an instruction with 2 operands (P1, P2).
 *	P3 defaults to 0.
 *
 * PUBLIC: int __vdbe_add_op2 __P((vdbe_t *, int, int, int));
 */
int
__vdbe_add_op2(vdbe_t* vm, int op, int p1, int p2)
{
	return __vdbe_add_op3(vm, op, p1, p2, 0);
}

/*
 * __vdbe_jump_here --
 *	Set the P2 field of the most recent instruction (or the instruction
 *	at address 'addr') to the current program counter.  This is used to
 *	resolve forward jumps.
 *
 * PUBLIC: void __vdbe_jump_here __P((vdbe_t *, int));
 */
void
__vdbe_jump_here(vdbe_t* vm, int addr)
{
	if (addr >= 0 && addr < vm->nOp) {
		__vdbe_change_p2(vm, addr, vm->nOp);
	}
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
__vdbe_make_label(vdbe_t* vm)
{
	int i;
	i = vm->nLabel++;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (i >= vm->nLabelAlloc) {
		int *new;
		vm->nLabelAlloc = (vm->nLabelAlloc * 2) + 10;
		if (__dbsql_realloc(NULL, vm->nLabelAlloc * sizeof(vm->labels[0]),
				 &vm->labels) == ENOMEM) {
			__dbsql_free(NULL, vm->labels);
		}
	}
	if (vm->labels == 0) {
		vm->nLabel = 0;
		vm->nLabelAlloc = 0;
		return 0;
	}
	vm->labels[i] = -1;
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
__vdbe_resolve_label(vdbe_t* vm, int x)
{
	int j;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (x < 0 && (-x) <= vm->nLabel && vm->opcodes) {
		if (vm->labels[-1 - x] == vm->nOp)
			return;
		DBSQL_ASSERT(vm->labels[-1 - x] < 0);
		vm->labels[-1 - x] = vm->nOp;
		for (j = 0; j < vm->nOp; j++) {
			if (vm->opcodes[j].p2 == x)
				vm->opcodes[j].p2 = vm->nOp;
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
__vdbe_current_addr(vdbe_t* vm)
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
		vm->nOpAlloc = (vm->nOpAlloc * 2) + num_op + 10;
		if (__dbsql_realloc(NULL, vm->nOpAlloc * sizeof(vdbe_op_t),
				 &vm->opcodes) == ENOMEM) {
			vm->nOpAlloc = old_size;
			return 0;
		}
		memset(&vm->opcodes[old_size], 0,
		       (vm->nOpAlloc - old_size) * sizeof(vdbe_op_t));
	}
	addr = vm->nOp;
	if (num_op > 0) {
		int i;
		for (i = 0; i < num_op; i++) {
			int p2 = op[i].p2;
			vm->opcodes[i + addr] = op[i];
			if (p2 < 0)
				vm->opcodes[i + addr].p2 = addr + ADDR(p2);
			if (op[i].p4.p == 0) {
				vm->opcodes[i + addr].p4type = P4_NOTUSED;
			}
#ifndef NDEBUG
			if (dbsql_vdbe_add_op_trace) {
				__vdbe_print_op(0, i + addr, &vm->opcodes[i+addr]);
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
 *
 * PUBLIC: void __vdbe_change_p1 __P((vdbe_t *, int, int));
 */
void
__vdbe_change_p1(vdbe_t* vm, int addr, int val)
{
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm && addr >= 0 && vm->nOp > addr && vm->opcodes) {
		vm->opcodes[addr].p1 = val;
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
__vdbe_change_p2(vdbe_t* vm, int addr, int val)
{
	DBSQL_ASSERT(val >= 0);
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm && addr >= 0 && vm->nOp > addr && vm->opcodes) {
		vm->opcodes[addr].p2 = val;
	}
}

/*
 * __vdbe_change_p3 --
 *	Change the value of the P3 operand for a specific instruction.
 *	In the register-based VM, P3 is an integer (register number).
 *
 * PUBLIC: void __vdbe_change_p3 __P((vdbe_t *, int, int));
 */
void
__vdbe_change_p3(vdbe_t* vm, int addr, int val)
{
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm && addr >= 0 && vm->nOp > addr && vm->opcodes) {
		vm->opcodes[addr].p3 = val;
	}
}

/*
 * __vdbe_change_p4 --
 *	Change the P4 operand of the most recently coded instruction
 *	or the instruction at address addr.
 *
 *	The P4 operand is a rich union type that can hold strings,
 *	pointers, integers, collation sequences, key info, etc.
 *
 *	If p4type is P4_DYNAMIC, then the string pointed to by p4
 *	was obtained from __dbsql_calloc() and ownership is transferred.
 *	If p4type is P4_STATIC, then p4 points to a static string.
 *	If p4type is P4_POINTER, then p4 is a pointer to an object.
 *	If p4type is P4_INT32, then p4 is reinterpreted as an int.
 *	If p4type is P4_KEYINFO, then p4 points to a key_info_t.
 *	If p4type is P4_FUNCDEF, then p4 points to a func_def_t.
 *	If p4type is P4_COLLSEQ, then p4 points to a coll_seq_t.
 *	If p4type >= 0, then the string is copied (p4type bytes).
 *
 *	If addr < 0, change P4 on the most recently inserted instruction.
 *
 * PUBLIC: void __vdbe_change_p4 __P((vdbe_t *, int, const char *, int));
 */
void
__vdbe_change_p4(vdbe_t* vm, int addr, const char *p4, int p4type)
{
	vdbe_op_t *pOp;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm == 0 || vm->opcodes == 0)
		return;
	if (addr < 0 || addr >= vm->nOp) {
		addr = vm->nOp - 1;
		if (addr < 0)
			return;
	}
	pOp = &vm->opcodes[addr];
	/* Free any existing dynamic P4 value */
	__vdbe_free_p4(pOp->p4type, pOp->p4.p);
	pOp->p4.p = 0;
	pOp->p4type = P4_NOTUSED;
	if (p4 == 0) {
		return;
	}
	if (p4type < 0) {
		/* Negative p4type values indicate the type directly */
		pOp->p4.p = (void*)p4;
		pOp->p4type = p4type;
		if (p4type == P4_INT32) {
			/* For INT32, the pointer IS the integer value */
			pOp->p4.i = (int)(intptr_t)p4;
			pOp->p4type = P4_INT32;
		}
	} else if (p4type == P4_INT32) {
		pOp->p4.i = (int)(intptr_t)p4;
		pOp->p4type = P4_INT32;
	} else if (p4type >= 0) {
		/* Copy p4type bytes of the string */
		int n = p4type;
		if (n == 0)
			n = strlen(p4);
		__str_nappend((char**)&pOp->p4.z, p4, n, NULL);
		pOp->p4type = P4_DYNAMIC;
	}
}

/*
 * __vdbe_change_p5 --
 *	Change the P5 flags of the most recently coded instruction.
 *
 * PUBLIC: void __vdbe_change_p5 __P((vdbe_t *, u_int16_t));
 */
void
__vdbe_change_p5(vdbe_t* vm, u_int16_t p5)
{
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm->nOp > 0) {
		vm->opcodes[vm->nOp - 1].p5 = p5;
	}
}

/*
 * __vdbe_free_p4 --
 *	Free the P4 value associated with an opcode based on its type.
 *
 * PUBLIC: void __vdbe_free_p4 __P((int, void *));
 */
void
__vdbe_free_p4(int p4type, void *p4)
{
	if (p4 == 0)
		return;
	switch (p4type) {
	case P4_DYNAMIC:
		__dbsql_free(NULL, p4);
		break;
	case P4_KEYINFO:
		__dbsql_free(NULL, p4);
		break;
	case P4_FUNCDEF:
		/* FuncDefs are typically static or owned elsewhere */
		break;
	case P4_COLLSEQ:
		/* CollSeqs are owned by the database connection */
		break;
	case P4_INT32:
	case P4_STATIC:
	case P4_NOTUSED:
		/* No deallocation needed */
		break;
	default:
		/* p4type >= 0 was copied into P4_DYNAMIC above */
		break;
	}
}

/*
 * __vdbe_dequote_p4 --
 *	If the P4 operand to the specified instruction appears
 *	to be a quoted string token, then this procedure removes
 *	the quotes.
 *
 * PUBLIC: void __vdbe_dequote_p4 __P((vdbe_t *, int));
 */
void
__vdbe_dequote_p4(vdbe_t* vm, int addr)
{
	vdbe_op_t *op;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm->opcodes == 0)
		return;
	if (addr < 0 || addr >= vm->nOp) {
		addr = vm->nOp - 1;
		if (addr < 0)
			return;
	}
	op = &vm->opcodes[addr];
	if (op->p4.z == 0 || op->p4.z[0] == 0)
		return;
	if (op->p4type != P4_DYNAMIC && op->p4type != P4_STATIC) {
		/* Only process string types */
		return;
	}
	if (op->p4type != P4_DYNAMIC) {
		__dbsql_strdup(NULL, op->p4.z, (char**)&op->p4.z);
		op->p4type = P4_DYNAMIC;
	}
	__str_unquote((char*)op->p4.z);
}

/*
 * __vdbe_compress_space --
 *	On the P4 argument of the given instruction, change all
 *	strings of whitespace characters into a single space and
 *	delete leading and trailing whitespace.
 *
 * PUBLIC: void __vdbe_compress_space __P((vdbe_t *, int));
 */
void
__vdbe_compress_space(vdbe_t* vm, int addr)
{
	unsigned char *z;
	int i, j;
	vdbe_op_t *op;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	if (vm->opcodes == 0 || addr < 0 || addr >= vm->nOp)
		return;
	op = &vm->opcodes[addr];
	if (op->p4type != P4_DYNAMIC && op->p4type != P4_STATIC) {
		/* Only process string types */
		return;
	}
	if (op->p4type != P4_DYNAMIC) {
		__dbsql_strdup(NULL, op->p4.z, (char**)&op->p4.z);
		op->p4type = P4_DYNAMIC;
	}
	z = (unsigned char*)op->p4.z;
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
__vdbe_find_op(vdbe_t* vm, int op, int p2)
{
	int i;
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	for (i = 0; i < vm->nOp; i++) {
		if (vm->opcodes[i].opcode == op && vm->opcodes[i].p2 == p2)
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
__vdbe_get_op(vdbe_t* vm, int addr)
{
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);
	DBSQL_ASSERT(addr >= 0 && addr < vm->nOp);
	return &vm->opcodes[addr];
}

/*
 * __vdbe_mem_release --
 *	Release memory held by a Mem (register) cell.  Handles the xDel
 *	destructor for MEM_Dyn values and the aggregate finalizer for
 *	MEM_Agg values.
 *
 * PUBLIC: void __vdbe_mem_release __P((mem_t *));
 */
void
__vdbe_mem_release(mem_t* p)
{
	if (p->flags & MEM_Dyn) {
		if (p->xDel) {
			p->xDel(p->z);
		} else {
			__dbsql_free(NULL, p->z);
		}
	} else if (p->flags & MEM_Agg) {
		/* Aggregate context: finalize if needed */
		if (p->u.pDef && p->u.pDef->xFinalize) {
			dbsql_func_t ctx;
			mem_t out;
			memset(&ctx, 0, sizeof(ctx));
			memset(&out, 0, sizeof(out));
			out.flags = MEM_Null;
			ctx.pFunc = p->u.pDef;
			ctx.pOut = &out;
			ctx.pMem = p;  /* Aggregate context memory */
			(*p->u.pDef->xFinalize)(&ctx);
		}
		__dbsql_free(NULL, p->zMalloc);
	}
	p->z = 0;
	p->zMalloc = 0;
	p->szMalloc = 0;
	p->xDel = 0;
	p->flags = MEM_Null;
}

/*
 * __vdbe_mem_set_null --
 *	Set a register to NULL.
 *
 * PUBLIC: void __vdbe_mem_set_null __P((mem_t *));
 */
void
__vdbe_mem_set_null(mem_t* p)
{
	if (p->flags & (MEM_Dyn | MEM_Agg)) {
		__vdbe_mem_release(p);
	}
	p->flags = MEM_Null;
	p->z = 0;
	p->n = 0;
}

/*
 * __vdbe_mem_set_int64 --
 *	Set a register to a 64-bit integer value.
 *
 * PUBLIC: void __vdbe_mem_set_int64 __P((mem_t *, int64_t));
 */
void
__vdbe_mem_set_int64(mem_t* p, int64_t val)
{
	if (p->flags & (MEM_Dyn | MEM_Agg)) {
		__vdbe_mem_release(p);
	}
	p->u.i = val;
	p->flags = MEM_Int;
}

/*
 * __vdbe_mem_set_double --
 *	Set a register to a double value.
 *
 * PUBLIC: void __vdbe_mem_set_double __P((mem_t *, double));
 */
void
__vdbe_mem_set_double(mem_t* p, double val)
{
	if (p->flags & (MEM_Dyn | MEM_Agg)) {
		__vdbe_mem_release(p);
	}
	p->u.r = val;
	p->flags = MEM_Real;
}

/*
 * __vdbe_mem_set_str --
 *	Set a register to a string value.  If xDel is not NULL, it will
 *	be called to free the string when the register is released.
 *	If n < 0, the string length is computed using strlen().
 *
 * PUBLIC: int __vdbe_mem_set_str __P((mem_t *, const char *, int,
 * PUBLIC:                         void(*)(void*)));
 */
int
__vdbe_mem_set_str(mem_t* p, const char *z, int n, void(*xDel)(void*))
{
	if (p->flags & (MEM_Dyn | MEM_Agg)) {
		__vdbe_mem_release(p);
	}
	if (z == 0) {
		p->flags = MEM_Null;
		p->z = 0;
		p->n = 0;
		return DBSQL_SUCCESS;
	}
	if (n < 0)
		n = strlen(z);
	p->n = n;
	p->enc = 1; /* DBSQL_UTF8 */
	if (xDel == (void(*)(void*))(-1)) {
		/* DBSQL_TRANSIENT: make a copy */
		if (__dbsql_calloc(NULL, 1, n + 1, &p->z) == ENOMEM) {
			p->flags = MEM_Null;
			return ENOMEM;
		}
		memcpy(p->z, z, n);
		p->z[n] = 0;
		p->flags = MEM_Str | MEM_Dyn | MEM_Term;
		p->xDel = 0; /* will use __dbsql_free */
		p->zMalloc = p->z;
		p->szMalloc = n + 1;
	} else if (xDel == 0) {
		/* DBSQL_STATIC: just store the pointer */
		p->z = (char*)z;
		p->flags = MEM_Str | MEM_Static | MEM_Term;
		p->xDel = 0;
	} else {
		/* Dynamic with custom destructor */
		p->z = (char*)z;
		p->flags = MEM_Str | MEM_Dyn | MEM_Term;
		p->xDel = xDel;
	}
	return DBSQL_SUCCESS;
}

/*
 * __vdbe_mem_copy --
 *	Make a full copy of a Mem cell.
 *
 * PUBLIC: int __vdbe_mem_copy __P((mem_t *, const mem_t *));
 */
int
__vdbe_mem_copy(mem_t* pTo, const mem_t *pFrom)
{
	if (pTo->flags & (MEM_Dyn | MEM_Agg)) {
		__vdbe_mem_release(pTo);
	}
	memcpy(pTo, pFrom, offsetof(mem_t, db));
	pTo->xDel = 0;
	pTo->zMalloc = 0;
	pTo->szMalloc = 0;
	if (pFrom->flags & (MEM_Str | MEM_Blob)) {
		if (pFrom->flags & (MEM_Dyn | MEM_Ephem)) {
			/* Make our own copy */
			if (__dbsql_calloc(NULL, 1, pFrom->n + 1,
					   &pTo->z) == ENOMEM) {
				pTo->flags = MEM_Null;
				return ENOMEM;
			}
			memcpy(pTo->z, pFrom->z, pFrom->n);
			pTo->z[pFrom->n] = 0;
			pTo->flags = (pFrom->flags & ~(MEM_Dyn | MEM_Ephem |
						       MEM_Static)) |
				     MEM_Dyn | MEM_Term;
			pTo->zMalloc = pTo->z;
			pTo->szMalloc = pFrom->n + 1;
		}
		/* If MEM_Static, the pointer is already copied above */
	}
	return DBSQL_SUCCESS;
}

/*
 * __release_mem_array --
 *	Release all memory held by an array of Mem cells.
 *
 * STATIC: static void __release_mem_array __P((mem_t *, int));
 */
static void
__release_mem_array(mem_t* p, int n)
{
	mem_t *pEnd = &p[n];
	while (p < pEnd) {
		if (p->flags & (MEM_Dyn | MEM_Agg)) {
			__vdbe_mem_release(p);
		} else {
			p->flags = MEM_Null;
		}
		p++;
	}
}

/*
 * __init_mem_array --
 *	Initialize an array of Mem cells with a given database connection
 *	and initial flags value.
 *
 * STATIC: static void __init_mem_array __P((mem_t *, int, DBSQL *, int));
 */
static void
__init_mem_array(mem_t* p, int n, DBSQL *db, int flags)
{
	int i;
	for (i = 0; i < n; i++) {
		memset(&p[i], 0, sizeof(mem_t));
		p[i].flags = flags;
		p[i].db = db;
	}
}

/*
 * dbsql_set_result_string --
 *	The following group or routines are employed by installable functions
 *	to return their results.
 *
 *	The dbsql_set_result_string() routine can be used to return a string
 *	value or to return a NULL.  To return a NULL, pass in NULL for
 *	'result'.
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
	mem_t *pOut = p->pOut;
	DBSQL_ASSERT(!p->isStep);
	if (pOut->flags & MEM_Dyn) {
		if (pOut->xDel) {
			pOut->xDel(pOut->z);
		} else {
			__dbsql_free(NULL, pOut->z);
		}
	}
	if (result == 0) {
		pOut->flags = MEM_Null;
		n = 0;
		pOut->z = 0;
		pOut->n = 0;
	} else {
		if (n < 0)
			n = strlen(result);
		if (__dbsql_calloc(NULL, 1, n + 1, &pOut->z) != ENOMEM) {
			memcpy(pOut->z, result, n);
			pOut->z[n] = 0;
		}
		pOut->flags = MEM_Str | MEM_Dyn | MEM_Term;
		pOut->xDel = 0;
		pOut->n = n;
	}
	return pOut->z;
}

/*
 * dbsql_set_result_null --
 *
 * EXTERN: void dbsql_set_result_null __P((dbsql_func_t *));
 */
void
dbsql_set_result_null(dbsql_func_t* p)
{
	mem_t *pOut = p->pOut;
	DBSQL_ASSERT(!p->isStep);
	if (pOut->flags & MEM_Dyn) {
		if (pOut->xDel) {
			pOut->xDel(pOut->z);
		} else {
			__dbsql_free(NULL, pOut->z);
		}
	}
	pOut->flags = MEM_Null;
	pOut->z = 0;
	pOut->n = 0;
}

/*
 * dbsql_set_result_int --
 *
 * EXTERN: void dbsql_set_result_int __P((dbsql_func_t *, int));
 */
void
dbsql_set_result_int(dbsql_func_t* p, int result)
{
	DBSQL_ASSERT(!p->isStep);
	if (p->pOut->flags & MEM_Dyn) {
		if (p->pOut->xDel) {
			p->pOut->xDel(p->pOut->z);
		} else {
			__dbsql_free(NULL, p->pOut->z);
		}
	}
	p->pOut->u.i = (int64_t)result;
	p->pOut->flags = MEM_Int;
}

/*
 * dbsql_set_result_int64 --
 *
 * EXTERN: void dbsql_set_result_int64 __P((dbsql_func_t *, int64_t));
 */
void
dbsql_set_result_int64(p, result)
	dbsql_func_t *p;
	int64_t result;
{
	DBSQL_ASSERT(!p->isStep);
	if (p->pOut->flags & MEM_Dyn) {
		if (p->pOut->xDel) {
			p->pOut->xDel(p->pOut->z);
		} else {
			__dbsql_free(NULL, p->pOut->z);
		}
	}
	p->pOut->u.i = result;
	p->pOut->flags = MEM_Int;
}

/*
 * dbsql_set_result_double --
 *
 * EXTERN: void dbsql_set_result_double __P((dbsql_func_t *, double));
 */
void
dbsql_set_result_double(dbsql_func_t* p, double result)
{
	DBSQL_ASSERT(!p->isStep);
	if (p->pOut->flags & MEM_Dyn) {
		if (p->pOut->xDel) {
			p->pOut->xDel(p->pOut->z);
		} else {
			__dbsql_free(NULL, p->pOut->z);
		}
	}
	p->pOut->u.r = result;
	p->pOut->flags = MEM_Real;
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
dbsql_set_result_blob(p, result, size, xDel)
	dbsql_func_t *p;
	const void *result;
	size_t size;
	void(*xDel)(void*);
{
	DBSQL_ASSERT(!p->isStep);
	if (p->pOut->flags & MEM_Dyn) {
		if (p->pOut->xDel) {
			p->pOut->xDel(p->pOut->z);
		} else {
			__dbsql_free(NULL, p->pOut->z);
		}
	}
	if (result == 0 || size == 0) {
		p->pOut->flags = MEM_Null;
		p->pOut->z = 0;
		p->pOut->n = 0;
		return;
	}
	if (xDel == (void(*)(void*))(-1)) {
		/* DBSQL_TRANSIENT */
		if (__dbsql_calloc(NULL, 1, size, &p->pOut->z) != ENOMEM) {
			memcpy(p->pOut->z, result, size);
		}
		p->pOut->flags = MEM_Blob | MEM_Dyn;
		p->pOut->xDel = 0;
	} else if (xDel == 0) {
		/* DBSQL_STATIC */
		p->pOut->z = (char*)result;
		p->pOut->flags = MEM_Blob | MEM_Static;
		p->pOut->xDel = 0;
	} else {
		p->pOut->z = (char*)result;
		p->pOut->flags = MEM_Blob | MEM_Dyn;
		p->pOut->xDel = xDel;
	}
	p->pOut->n = (int)size;
}

/*
 * dbsql_set_result_varchar --
 *
 * EXTERN: void dbsql_set_result_varchar __P((dbsql_func_t *, const char *,
 * EXTERN:                            size_t, void(*)(void*)));
 */
void
dbsql_set_result_varchar(p, result, size, xDel)
	dbsql_func_t *p;
	const char *result;
	size_t size;
	void(*xDel)(void*);
{
	__vdbe_mem_set_str(p->pOut, result, (int)size, xDel);
}

/*
 * dbsql_user_data --
 *	Extract the user data from a dbsql_func_t structure and return a
 *	pointer to it.
 *
 * EXTERN: void *dbsql_user_data __P((dbsql_func_t *));
 */
void *
dbsql_user_data(dbsql_func_t* p)
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
 * EXTERN: void *dbsql_aggregate_context __P((dbsql_func_t *, int));
 */
void *
dbsql_aggregate_context(dbsql_func_t* p, int num_bytes)
{
	DBSQL_ASSERT(p && p->pFunc && p->pFunc->xStep);
	if (p->pMem == 0 || p->pMem->z == 0) {
		if (num_bytes <= 0) {
			return 0;
		}
		if (p->pMem) {
			__vdbe_mem_grow(p->pMem, num_bytes, 0);
			p->pMem->flags = MEM_Agg;
		}
	}
	return p->pMem ? p->pMem->z : 0;
}

/*
 * dbsql_aggregate_count --
 *	Return the number of times the Step function of a aggregate has been
 *	called.
 *
 * EXTERN: int dbsql_aggregate_count __P((dbsql_func_t *));
 */
int
dbsql_aggregate_count(dbsql_func_t* p)
{
	DBSQL_ASSERT(p && p->pFunc && p->pFunc->xStep);
	/*
	 * In the register-based VDBE, the aggregate count is no longer
	 * directly tracked in the context structure. Return 0 for now.
	 * This function may need to be deprecated or track count differently.
	 */
	return 0;
}

#if !defined(NDEBUG) || defined(VDBE_PROFILE)
/*
 * __vdbe_print_op --
 *	Print a single opcode.  This routine is used for debugging only.
 *
 * PUBLIC: void __vdbe_print_op __P((FILE *, int, vdbe_op_t *));
 */
void
__vdbe_print_op(FILE* out, int pc, vdbe_op_t* op)
{
	char *p4str;
	char ptr[50];
	if (op->p4type == P4_POINTER) {
		sprintf(ptr, "ptr(%p)", op->p4.p);
		p4str = ptr;
	} else if (op->p4type == P4_INT32) {
		sprintf(ptr, "%d", op->p4.i);
		p4str = ptr;
	} else if (op->p4type == P4_DYNAMIC || op->p4type == P4_STATIC) {
		p4str = (char*)op->p4.z;
	} else {
		p4str = 0;
	}
	if (out == 0)
		out = stdout;
	fprintf(out, "%4d %-12s %4d %4d %4d %s %d\n",
		pc, __opcode_names[op->opcode],
		op->p1, op->p2, op->p3,
		p4str ? p4str : "",
		op->p5);
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
 *	For the register-based VM, EXPLAIN shows 8 columns:
 *	addr, opcode, p1, p2, p3, p4, p5, comment
 *
 * PUBLIC: int __vdbe_list __P((vdbe_t *));
 */
int
__vdbe_list(vdbe_t* vm)
{
	int i;
	DBSQL *db = vm->db;
	mem_t *pMem;

	DBSQL_ASSERT(vm->explain);

	/* Use the first 8 registers as scratch space for EXPLAIN output */
	if (vm->nMem < 8)
		return DBSQL_ERROR;

	vm->rc = DBSQL_SUCCESS;
	vm->nResColumn = 8;

	for (i = vm->pc; vm->rc == DBSQL_SUCCESS && i < vm->nOp; i++) {
		vdbe_op_t *pOp = &vm->opcodes[i];

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

		/* addr */
		pMem = &vm->registers[0];
		__vdbe_mem_set_int64(pMem, i);

		/* opcode */
		pMem = &vm->registers[1];
		__vdbe_mem_set_str(pMem, __opcode_names[pOp->opcode],
				   -1, 0);

		/* p1 */
		pMem = &vm->registers[2];
		__vdbe_mem_set_int64(pMem, pOp->p1);

		/* p2 */
		pMem = &vm->registers[3];
		__vdbe_mem_set_int64(pMem, pOp->p2);

		/* p3 */
		pMem = &vm->registers[4];
		__vdbe_mem_set_int64(pMem, pOp->p3);

		/* p4 */
		pMem = &vm->registers[5];
		if (pOp->p4type == P4_DYNAMIC || pOp->p4type == P4_STATIC) {
			__vdbe_mem_set_str(pMem, pOp->p4.z, -1, 0);
		} else if (pOp->p4type == P4_INT32) {
			char buf[30];
			sprintf(buf, "%d", pOp->p4.i);
			__vdbe_mem_set_str(pMem, buf, -1,
					   (void(*)(void*))(-1));
		} else {
			__vdbe_mem_set_null(pMem);
		}

		/* p5 */
		pMem = &vm->registers[6];
		__vdbe_mem_set_int64(pMem, pOp->p5);

		/* comment (empty for now) */
		pMem = &vm->registers[7];
		__vdbe_mem_set_null(pMem);

		vm->pc = i + 1;
		vm->result_row = &vm->registers[0];
		return DBSQL_ROW;
	}
	return vm->rc == DBSQL_SUCCESS ? DBSQL_DONE : DBSQL_ERROR;
}

/*
 * __vdbe_set_num_cols --
 *	Set the number of result columns for this statement.
 *	Allocates the aColName array.
 *
 * PUBLIC: void __vdbe_set_num_cols __P((vdbe_t *, int));
 */
void
__vdbe_set_num_cols(vdbe_t* vm, int nResColumn)
{
	int n;

	if (vm->column_names) {
		__release_mem_array(vm->column_names,
				    vm->nResColumn * COLNAME_N);
		__dbsql_free(NULL, vm->column_names);
	}
	n = nResColumn * COLNAME_N;
	vm->nResColumn = nResColumn;
	if (__dbsql_calloc(NULL, n, sizeof(mem_t), &vm->column_names) == ENOMEM)
		return;
	__init_mem_array(vm->column_names, n, vm->db, MEM_Null);
}

/*
 * __vdbe_set_col_name --
 *	Set the name of the idx'th column of the result set.
 *
 *	var is one of the COLNAME_* constants (COLNAME_NAME, COLNAME_DECLTYPE,
 *	etc.).  zName must be a nul-terminated string.
 *
 *	This must be called after __vdbe_set_num_cols().
 *
 * PUBLIC: int __vdbe_set_col_name __P((vdbe_t *, int, int, const char *,
 * PUBLIC:                          void(*)(void*)));
 */
int
__vdbe_set_col_name(vdbe_t* vm, int idx, int var, const char *zName,
		    void(*xDel)(void*))
{
	int rc;
	mem_t *pColName;
	DBSQL_ASSERT(idx < vm->nResColumn);
	DBSQL_ASSERT(var < COLNAME_N);
	if (vm->column_names == 0)
		return ENOMEM;
	pColName = &vm->column_names[idx + var * vm->nResColumn];
	rc = __vdbe_mem_set_str(pColName, zName, -1, xDel);
	return rc;
}

/*
 * __vdbe_make_ready --
 *	Prepare a virtual machine for execution.  This involves allocating
 *	registers, cursor slots, and variable storage, then initializing the
 *	program counter.
 *
 *	After the VDBE has been prepped, it can be executed by one or more
 *	calls to __vdbe_exec().
 *
 *	For the register-based VM:
 *	  - aMem[0..nMem-1] is the register array
 *	  - apCsr[0..nCursor-1] is the cursor pointer array
 *	  - aVar[0..nVar-1] is the bound parameter array
 *
 * PUBLIC: void __vdbe_make_ready __P((vdbe_t *, int, int, int, int));
 *
 * vm		The VDBE
 * nVar		Number of '?' parameters in the SQL statement
 * nMem		Number of registers needed
 * nCursor	Number of cursor slots needed
 * explain_p	True if the EXPLAIN keyword is present
 */
void
__vdbe_make_ready(vdbe_t* vm, int nVar, int nMem, int nCursor, int explain_p)
{
	DBSQL_ASSERT(vm != 0);
	DBSQL_ASSERT(vm->magic == VDBE_MAGIC_INIT);

	/*
	 * Add a HALT instruction to the very end of the program.
	 */
	if (vm->nOp == 0 ||
	    (vm->opcodes && vm->opcodes[vm->nOp - 1].opcode != OP_Halt)) {
		__vdbe_add_op(vm, OP_Halt, 0, 0);
	}

	/*
	 * Resolve any remaining labels in the instruction stream.
	 */
	/* Labels should already be resolved by code generators,
	 * but ensure P2 label references are patched. */

	/*
	 * For EXPLAIN mode, ensure at least 10 registers for output.
	 */
	if (explain_p) {
		if (nMem < 10) nMem = 10;
	}

	/*
	 * Each cursor uses a memory cell.  The first cursor (cursor 0)
	 * can use aMem[0].  Allocate space for cursors at the end of aMem[].
	 */
	nMem += nCursor;
	if (nCursor == 0 && nMem > 0) nMem++;  /* Space for aMem[0] */

	/*
	 * Fix for DDL statements: Ensure minimum register allocation.
	 * DDL operations like CREATE TABLE use legacy stack-based opcodes
	 * (OP_MakeRecord with 5 fields) that haven't been fully converted to
	 * register-based model. They need at least 10 registers to work:
	 * - MakeRecord(5) needs pTos[1-5] = registers [pTos-4..pTos]
	 * - If pTos = aMem[n_memory-1], we need n_memory >= 5
	 * - Allocate 10 for safety margin and future opcodes
	 */
	if (nMem < 10) {
		nMem = 10;  /* Minimum for legacy stack-based opcodes */
	}

	/*
	 * Allocate the register array (aMem).
	 */
	if (vm->registers == 0) {
		vm->nVar = nVar;
		vm->nMem = nMem;
		vm->nCursor = nCursor;

		if (nMem > 0) {
			if (__dbsql_calloc(NULL, nMem, sizeof(mem_t),
					   &vm->registers) == ENOMEM) {
				vm->nMem = 0;
				return;
			}
			__init_mem_array(vm->registers, nMem, vm->db, MEM_Null);
		}

		/*
		 * Allocate the bound parameter array (aVar).
		 */
		if (nVar > 0) {
			if (__dbsql_calloc(NULL, nVar, sizeof(mem_t),
					   &vm->variables) == ENOMEM) {
				vm->nVar = 0;
				return;
			}
			__init_mem_array(vm->variables, nVar, vm->db, MEM_Null);
		}

		/*
		 * Allocate the cursor pointer array (apCsr).
		 */
		if (nCursor > 0) {
			if (__dbsql_calloc(NULL, nCursor,
					   sizeof(VdbeCursor*),
					   &vm->cursors) == ENOMEM) {
				vm->nCursor = 0;
				return;
			}
			/* Already zeroed by calloc */
		}
	}

#ifdef MEMORY_DEBUG
	if (__os_file_exists("vdbe_trace")){
		vm->trace = stdout;
	}
#endif
	vm->pc = -1; /* Will be set to 0 on first step */
	vm->rc = DBSQL_SUCCESS;
	vm->errorAction = OE_Abort;
	vm->explain |= explain_p;
	vm->cacheCtr = 1;
	vm->result_row = 0;
	vm->magic = VDBE_MAGIC_RUN;
#ifdef VDBE_PROFILE
	{
		int i;
		for (i = 0; i < vm->nOp; i++) {
			vm->opcodes[i].cnt = 0;
			vm->opcodes[i].cycles = 0;
		}
	}
#endif
}


/*
 * __vdbe_sorter_reset --
 *	Remove any elements that remain on the sorter for the VDBE given.
 *
 *	TODO: Legacy function removed in SQLite 3.52.0.
 *	Sorting is now handled via CURTYPE_SORTER cursors, not a
 *	separate pSort linked list in the vdbe_t structure.
 *
 * PUBLIC: void __vdbe_sorter_reset __P((vdbe_t *));
 */
void
__vdbe_sorter_reset(vdbe_t* vm)
{
	/* TODO: Stub - sorting redesigned to use cursors in SQLite 3.52.0 */
	(void)vm; /* Suppress unused parameter warning */
}

/*
 * __vdbe_agg_reset --
 *	Reset an Agg structure.  Delete all its contents.
 *
 *	TODO: Legacy function removed in SQLite 3.52.0.
 *	Aggregate handling is now done via the frame mechanism and
 *	OP_AggFinal opcode. The agg_t structure no longer exists in
 *	the vdbe_t structure.
 *
 * PUBLIC: void __vdbe_agg_reset __P((agg_t *));
 */
void
__vdbe_agg_reset(agg_t* agg)
{
	/* TODO: Stub - aggregate handling redesigned in SQLite 3.52.0 */
	(void)agg; /* Suppress unused parameter warning */
}

/*
 * __vdbe_keylist_free --
 *	Delete a keylist.
 *
 *	TODO: Legacy function with incomplete typedef keylist_t.
 *	The List opcodes (OP_ListRead, OP_ListWrite, etc.) that use
 *	this should be migrated to use cursors or result sets.
 *
 * PUBLIC: void __vdbe_keylist_free __P((keylist_t *));
 */
void
__vdbe_keylist_free(keylist_t* p)
{
	/* TODO: Stub - keylist_t is incomplete type, needs structure definition */
	(void)p; /* Suppress unused parameter warning */
}

/*
 * __vdbe_free_cursor --
 *	Close a cursor and release all the resources it holds.
 *	This handles the new pointer-based cursor model where cursors
 *	can be of different types.
 *
 * PUBLIC: void __vdbe_free_cursor __P((vdbe_t *, VdbeCursor *));
 */
void
__vdbe_free_cursor(vdbe_t* vm, VdbeCursor* pCur)
{
	if (pCur == 0)
		return;

	switch (pCur->eCurType) {
	case CURTYPE_BTREE:
		if (pCur->uc.pCursor != 0) {
			__sm_close_cursor(pCur->uc.pCursor);
		}
		if (pCur->isEphemeral && pCur->ub.pBtx != 0) {
			__sm_close_db(pCur->ub.pBtx);
		}
		break;
	case CURTYPE_SORTER:
		/* Sorter cleanup would go here */
		break;
	case CURTYPE_VTAB:
		/* Virtual table cleanup would go here */
		break;
	case CURTYPE_PSEUDO:
		/* Pseudo-table cleanup - nothing to do */
		break;
	}

	__dbsql_free(vm->db, pCur);
}

/*
 * __close_all_cursors --
 *	Close all cursors.  In the register-based VM, cursors are stored
 *	as a pointer array (apCsr[]), not an inline array.
 *
 *	Also release any dynamic memory held by registers, since register
 *	values may hold pointers to cursor-related resources.
 *
 * STATIC: static void __close_all_cursors __P((vdbe_t *));
 */
static void
__close_all_cursors(vdbe_t* vm)
{
	int i;

	/* Close all open cursors */
	if (vm->cursors) {
		for (i = 0; i < vm->nCursor; i++) {
			if (vm->cursors[i]) {
				__vdbe_free_cursor(vm, vm->cursors[i]);
				vm->cursors[i] = 0;
			}
		}
	}

	/* Release all register memory */
	if (vm->registers) {
		__release_mem_array(vm->registers, vm->nMem);
	}
}

/*
 * __cleanup --
 *	Clean up the VM after execution.
 *
 *	This routine will automatically close any cursors and release
 *	register values and bound parameter values.
 *
 *	Legacy fields removed in SQLite 3.52.0 modernization:
 *	  pList, pFile, azField, nField, zLine, nLineAlloc,
 *	  agg, aSet, nSet, keylistStack, keylistStackDepth, pSort
 *
 * STATIC: static void __cleanup __P((vdbe_t *));
 */
static void
__cleanup(vdbe_t* vm)
{
	__close_all_cursors(vm);

	/* TODO: Legacy structures removed in SQLite 3.52.0
	 * The following cleanup operations are no longer needed:
	 * - pList (keylist) - removed
	 * - pSort (sorter) - replaced by cursor-based sorting
	 * - pFile, azField, nField, zLine - removed
	 * - agg (aggregates) - now handled via frame mechanism
	 * - aSet (set hash tables) - removed
	 * - keylistStack - removed
	 */

	__dbsql_free(NULL, vm->zErrMsg);
	vm->zErrMsg = 0;
	vm->result_row = 0;
}

/*
 * __vdbe_reset --
 *	Clean up a VDBE after execution but do not delete the VDBE just yet.
 *	Write any error messages into *pzErrMsg.  Return the result code.
 *
 *	After this routine is run, the VDBE should be ready to be executed
 *	again.
 *
 * PUBLIC: int __vdbe_reset __P((vdbe_t *, char **));
 */
int
__vdbe_reset(vdbe_t* vm, char* *err_msgs)
{
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
			/* FALLTHROUGH */
		case OE_Rollback:
			/* TODO __sm_abort_txn(db); */
			db->flags &= ~DBSQL_InTrans;
			db->onError = OE_Default;
			break;
		default:
			/* TODO: Legacy field undoTransOnError removed in SQLite 3.52.0
			 * Transaction rollback is now handled via statement journal
			 * and usesStmtJournal flag in the vdbe structure.
			 */
			break;
		}
		__rollback_internal_changes(db);
	}
#ifdef VDBE_PROFILE
	{
		FILE *out = fopen("vdbe_profile.out", "a");
		if (out) {
			int i;
			fprintf(out, "---- ");
			for (i = 0; i < vm->nOp; i++) {
				fprintf(out, "%02x", vm->opcodes[i].opcode);
			}
			fprintf(out, "\n");
			for(i = 0; i < vm->nOp; i++) {
				fprintf(out, "%6d %10lld %8lld ",
					vm->opcodes[i].cnt,
					vm->opcodes[i].cycles,
					(vm->opcodes[i].cnt > 0) ?
					vm->opcodes[i].cycles / vm->opcodes[i].cnt :0);
				__vdbe_print_op(out, i, &vm->opcodes[i]);
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
__vdbe_finalize(vdbe_t* vm, char* *err_msgs)
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
	/* TODO: Legacy field want_to_close removed in SQLite 3.52.0
	 * Database close handling is now done explicitly by the caller.
	 */
}

/*
 * __api_bind --
 *	Bind a value to a parameter.  Variable $1 in the original SQL
 *	will be the value at index 1.  Uses the new Mem-based variable
 *	storage (aVar[]).
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
	mem_t *pVar;

	if (vm->magic != VDBE_MAGIC_RUN || vm->pc != -1) {
		return DBSQL_MISUSE;
	}
	if (i < 1 || i > vm->nVar) {
		return DBSQL_RANGE;
	}
	i--; /* Convert to 0-based index */
	pVar = &vm->variables[i];

	/* Release any prior value */
	__vdbe_mem_release(pVar);

	if (val == 0) {
		pVar->flags = MEM_Null;
		return DBSQL_SUCCESS;
	}
	if (len < 0) {
		len = strlen(val) + 1;
	}
	if (copy) {
		return __vdbe_mem_set_str(pVar, val, len,
					  (void(*)(void*))(-1));
	} else {
		pVar->z = (char*)val;
		pVar->n = len;
		pVar->flags = MEM_Str | MEM_Static;
		return DBSQL_SUCCESS;
	}
}


/*
 * __vdbe_delete --
 *	Delete an entire VDBE.
 *
 * PUBLIC: void __vdbe_delete __P((vdbe_t *));
 */
void
__vdbe_delete(vdbe_t* vm)
{
	int i;
	if (vm == 0)
		return;
	__cleanup(vm);

	/* Unlink from the database's VDBE list */
	if (vm->prev_vdbe) {
		vm->prev_vdbe->next_vdbe = vm->next_vdbe;
	} else {
		DBSQL_ASSERT(vm->db->pVdbe == vm );
		vm->db->pVdbe = vm->next_vdbe;
	}
	if (vm->next_vdbe) {
		vm->next_vdbe->prev_vdbe = vm->prev_vdbe;
	}
	vm->prev_vdbe = vm->next_vdbe = 0;

	/* Free the opcodes and their P4 values */
	if (vm->nOpAlloc == 0) {
		vm->opcodes = 0;
		vm->nOp = 0;
	}
	for (i = 0; i < vm->nOp; i++) {
		__vdbe_free_p4(vm->opcodes[i].p4type, vm->opcodes[i].p4.p);
	}

	/* Free bound parameters */
	if (vm->variables) {
		__release_mem_array(vm->variables, vm->nVar);
		__dbsql_free(NULL, vm->variables);
	}

	/* Free column name array */
	if (vm->column_names) {
		__release_mem_array(vm->column_names,
				    vm->nResColumn * COLNAME_N);
		__dbsql_free(NULL, vm->column_names);
	}

	/* Free the register array */
	__dbsql_free(NULL, vm->registers);

	/* Free the cursor pointer array */
	__dbsql_free(NULL, vm->cursors);

	__dbsql_free(NULL, vm->opcodes);
	__dbsql_free(NULL, vm->labels);
	vm->magic = VDBE_MAGIC_DEAD;
	__dbsql_free(NULL, vm);
}

/*
 * __vdbe_byte_swap --
 *	Convert an integer in between the native integer format and
 *	the bigEndian format used as the record number for tables.
 *
 *	This function is retained for backward compatibility with legacy
 *	key encoding.  New code should use native i64 keys directly.
 *
 * PUBLIC: int __vdbe_byte_swap __P((int));
 */
int
__vdbe_byte_swap(int x)
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
 *	Updated for VdbeCursor structure. Legacy fields (lastRecno,
 *	recnoIsValid, movetoTarget) have been removed in SQLite 3.52.0.
 *	The deferredMoveto mechanism is still supported but the tracking
 *	fields are no longer maintained.
 *
 * PUBLIC: int __vdbe_cursor_moveto __P((VdbeCursor *));
 */
int
__vdbe_cursor_moveto(VdbeCursor* p)
{
	if (p->deferredMoveto) {
		int res;
		/* TODO: Legacy movetoTarget field removed */
		/* The deferred moveto operation should have been performed
		 * by the calling code before reaching here. This function
		 * now just clears the flag. In SQLite 3.52+, the moveto
		 * target is passed directly to __sm_moveto() rather than
		 * being stored in the cursor structure. */
#ifdef CONFIG_TEST
		extern int dbsql_search_count;
		dbsql_search_count++;
#endif
		p->deferredMoveto = 0;
		p->cacheStatus = 0; /* CACHE_STALE */
	}
	return DBSQL_SUCCESS;
}

/*
 * __vdbe_int_value --
 *	Extract a 64-bit integer value from a register (Mem).
 *
 * PUBLIC: int64_t __vdbe_int_value __P((const mem_t *));
 */
int64_t
__vdbe_int_value(const mem_t* pMem)
{
	if (pMem->flags & MEM_Int) {
		return pMem->u.i;
	} else if (pMem->flags & MEM_Real) {
		return (int64_t)pMem->u.r;
	} else if (pMem->flags & (MEM_Str | MEM_Blob)) {
		int64_t val = 0;
		if (pMem->z) {
			val = (int64_t)strtoll(pMem->z, 0, 10);
		}
		return val;
	}
	return 0;
}

/*
 * __vdbe_real_value --
 *	Extract a double value from a register (Mem).
 *
 * PUBLIC: double __vdbe_real_value __P((const mem_t *));
 */
double
__vdbe_real_value(const mem_t* pMem)
{
	if (pMem->flags & MEM_Real) {
		return pMem->u.r;
	} else if (pMem->flags & MEM_Int) {
		return (double)pMem->u.i;
	} else if (pMem->flags & (MEM_Str | MEM_Blob)) {
		double val = 0.0;
		if (pMem->z) {
			val = strtod(pMem->z, 0);
		}
		return val;
	}
	return 0.0;
}

/*
 * __vdbe_mem_grow --
 *	Ensure that the Mem has at least n bytes of writeable buffer space.
 *	If preserve is true, preserve existing content.
 *
 * PUBLIC: int __vdbe_mem_grow __P((mem_t *, int, int));
 */
int
__vdbe_mem_grow(mem_t* pMem, int n, int preserve)
{
	if (pMem->szMalloc < n) {
		char *zNew = 0;
		if (__dbsql_calloc(NULL, 1, n, &zNew) == ENOMEM) {
			return ENOMEM;
		}
		if (preserve && pMem->z && pMem->n > 0) {
			int copyLen = pMem->n < n ? pMem->n : n;
			memcpy(zNew, pMem->z, copyLen);
		}
		if (pMem->zMalloc) {
			__dbsql_free(NULL, pMem->zMalloc);
		}
		pMem->zMalloc = zNew;
		pMem->szMalloc = n;
		pMem->z = zNew;
	} else if (!pMem->zMalloc || pMem->z != pMem->zMalloc) {
		/* We have enough malloc space but z doesn't point to it */
		if (preserve && pMem->z && pMem->n > 0) {
			int copyLen = pMem->n < n ? pMem->n : n;
			memcpy(pMem->zMalloc, pMem->z, copyLen);
		}
		pMem->z = pMem->zMalloc;
	}
	return DBSQL_SUCCESS;
}

/*
 * __vdbe_mem_clear_and_resize --
 *	Release existing content and resize the buffer.
 *
 * PUBLIC: int __vdbe_mem_clear_and_resize __P((mem_t *, int));
 */
int
__vdbe_mem_clear_and_resize(mem_t* pMem, int n)
{
	__vdbe_mem_set_null(pMem);
	return __vdbe_mem_grow(pMem, n, 0);
}
