/*
 * Sample SQLite 3.52.0 code for transpilation testing
 * This file contains representative patterns from SQLite 3.52.0
 */

#include "sqlite3.h"

/* VdbeOp structure usage */
void __vdbe_add_op(parser_t *pParse, int opcode, int p1, int p2) {
    vdbe_t *v = __get_vdbe(pParse);
    VdbeOp *pOp;

    pOp = __vdbe_get_op(v, v->n_op++);
    pOp->opcode = opcode;
    pOp->p1 = p1;
    pOp->p2 = p2;
    pOp->p3 = 0;
    pOp->p4.p = 0;
    pOp->p4type = P4_NOTUSED;
    pOp->p5 = 0;
}

/* mem_t structure usage */
void __mem_set_str(mem_t *pMem, const char *z, int n) {
    if (pMem->flags & MEM_Dyn) {
        if (pMem->x_del) {
            pMem->x_del((void *)pMem->z);
        }
    }
    pMem->z = z;
    pMem->n = n;
    pMem->flags = MEM_Str | MEM_Ephem;
    pMem->enc = DBSQL_UTF8;
    pMem->e_subtype = 0;
}

/* Parser structure usage */
int __parse_allocate_label(parser_t *pParse) {
    int i;
    if (pParse->n_label >= 0) {
        pParse->n_label = -(1 + pParse->n_mem);
    }
    i = pParse->n_label--;
    if ((-pParse->n_label) > pParse->n_label_alloc) {
        int *aLabel;
        int n = pParse->n_label_alloc * 2 + 10;
        aLabel = __realloc(pParse->a_label, n * sizeof(int));
        if (aLabel == 0) {
            return 0;
        }
        pParse->a_label = aLabel;
        pParse->n_label_alloc = n;
    }
    if (pParse->i_range_reg > 0) {
        pParse->n_range_reg++;
    }
    return i;
}

/* expr_list_t structure usage */
expr_list_t *__expr_list_append(expr_list_t *pList, expr_t *pExpr) {
    if (pList == 0) {
        pList = __malloc(sizeof(expr_list_t));
        if (pList == 0) {
            return 0;
        }
        pList->n_expr = 0;
        pList->n_alloc = 1;
    } else if (pList->n_expr >= pList->n_alloc) {
        ExprListItem *a;
        int n = pList->n_alloc * 2 + 4;
        a = __realloc(pList->a, n * sizeof(pList->a[0]));
        if (a == 0) {
            return pList;
        }
        pList->a = a;
        pList->n_alloc = n;
    }
    pList->a[pList->n_expr].pExpr = pExpr;
    pList->a[pList->n_expr].zName = 0;
    pList->n_expr++;
    return pList;
}

/* Opcode with P5 flags */
void __vdbe_add_seek_op(vdbe_t *v, int cursor, int reg, u_int16_t flags) {
    VdbeOp *pOp = __vdbe_get_op(v, v->n_op++);
    pOp->opcode = OP_SeekGE;
    pOp->p1 = cursor;
    pOp->p2 = reg;
    pOp->p3 = 0;
    pOp->p5 = flags | OPFLAG_USESEEKRESULT;
}

/* Type conversions */
static u_int8_t __get_column_type(table_t *pTab, int iCol) {
    u_int32_t flags;
    uint64_t mask;
    int64_t value;

    flags = pTab->a_col[iCol].affinity;
    mask = 0xFFFFFFFFULL;
    value = (int64_t)flags & mask;
    return (u_int8_t)(value & 0xFF);
}
