/*
 * Sample SQLite 3.52.0 code for transpilation testing
 * This file contains representative patterns from SQLite 3.52.0
 */

#include "sqlite3.h"

/* VdbeOp structure usage */
void sqliteVdbeAddOp(Parse *pParse, int opcode, int p1, int p2) {
    Vdbe *v = sqliteGetVdbe(pParse);
    VdbeOp *pOp;

    pOp = sqliteVdbeGetOp(v, v->nOp++);
    pOp->opcode = opcode;
    pOp->p1 = p1;
    pOp->p2 = p2;
    pOp->p3 = 0;
    pOp->p4.p = 0;
    pOp->p4type = P4_NOTUSED;
    pOp->p5 = 0;
}

/* Mem structure usage */
void sqliteMemSetStr(Mem *pMem, const char *z, int n) {
    if (pMem->flags & MEM_Dyn) {
        if (pMem->xDel) {
            pMem->xDel((void *)pMem->z);
        }
    }
    pMem->z = z;
    pMem->n = n;
    pMem->flags = MEM_Str | MEM_Ephem;
    pMem->enc = SQLITE_UTF8;
    pMem->eSubtype = 0;
}

/* Parser structure usage */
int sqliteParseAllocateLabel(Parse *pParse) {
    int i;
    if (pParse->nLabel >= 0) {
        pParse->nLabel = -(1 + pParse->nMem);
    }
    i = pParse->nLabel--;
    if ((-pParse->nLabel) > pParse->nLabelAlloc) {
        int *aLabel;
        int n = pParse->nLabelAlloc * 2 + 10;
        aLabel = sqliteRealloc(pParse->aLabel, n * sizeof(int));
        if (aLabel == 0) {
            return 0;
        }
        pParse->aLabel = aLabel;
        pParse->nLabelAlloc = n;
    }
    if (pParse->iRangeReg > 0) {
        pParse->nRangeReg++;
    }
    return i;
}

/* ExprList structure usage */
ExprList *sqliteExprListAppend(ExprList *pList, Expr *pExpr) {
    if (pList == 0) {
        pList = sqliteMalloc(sizeof(ExprList));
        if (pList == 0) {
            return 0;
        }
        pList->nExpr = 0;
        pList->nAlloc = 1;
    } else if (pList->nExpr >= pList->nAlloc) {
        ExprListItem *a;
        int n = pList->nAlloc * 2 + 4;
        a = sqliteRealloc(pList->a, n * sizeof(pList->a[0]));
        if (a == 0) {
            return pList;
        }
        pList->a = a;
        pList->nAlloc = n;
    }
    pList->a[pList->nExpr].pExpr = pExpr;
    pList->a[pList->nExpr].zName = 0;
    pList->nExpr++;
    return pList;
}

/* Opcode with P5 flags */
void sqliteVdbeAddSeekOp(Vdbe *v, int cursor, int reg, u16 flags) {
    VdbeOp *pOp = sqliteVdbeGetOp(v, v->nOp++);
    pOp->opcode = OP_SeekGE;
    pOp->p1 = cursor;
    pOp->p2 = reg;
    pOp->p3 = 0;
    pOp->p5 = flags | OPFLAG_USESEEKRESULT;
}

/* Type conversions */
static u8 sqliteGetColumnType(Table *pTab, int iCol) {
    u32 flags;
    u64 mask;
    i64 value;

    flags = pTab->aCol[iCol].affinity;
    mask = 0xFFFFFFFFULL;
    value = (i64)flags & mask;
    return (u8)(value & 0xFF);
}
