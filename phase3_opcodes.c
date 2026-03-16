/* Phase 3: Cursor Navigation Opcodes - Register-Based Implementation
 * To be integrated into src/vdbe.c's __vdbe_exec() switch statement
 *
 * These replace the old stack-based OP_Rewind, OP_Next, OP_Prev, OP_Column, OP_Recno
 * with register-based versions using VdbeCursor and Mem structures.
 */

/* Opcode: OP_Rewind P1 P2 * * *
 * Synopsis: rewind cursor P1, jump to P2 if empty
 *
 * Move cursor P1 to the first entry in its table or index.
 * If the table or index is empty, jump to P2.
 * P1: cursor number
 * P2: jump destination if empty
 *
 * This replaces the old stack-based OP_Rewind.
 */
case OP_Rewind: {
	VdbeCursor *pC;
	sm_cursor_t *pCrsr;
	int res;

	DBSQL_ASSERT(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	res = 1;  /* Assume empty */

	if (pC != 0) {
		if (pC->eCurType == CURTYPE_BTREE) {
			pCrsr = pC->uc.pCursor;
			if (pCrsr != 0) {
				rc = __sm_first(pCrsr, &res);
				if (rc != DBSQL_SUCCESS)
					goto abort_due_to_error;
				pC->cacheStatus = CACHE_STALE;
			}
		}
		pC->nullRow = (u8)res;
	}

	if (pOp->p2 > 0 && res) {
		pc = pOp->p2 - 1;
	}
	break;
}

/* Opcode: OP_Next P1 P2 * * *
 * Synopsis: advance cursor P1, jump to P2 if not EOF
 *
 * Advance cursor P1 to the next entry.
 * If successful, jump to P2. If at EOF, fall through.
 * P1: cursor number
 * P2: jump destination if not EOF
 */
case OP_Next: {
	VdbeCursor *pC;
	sm_cursor_t *pCrsr;
	int res;

	DBSQL_ASSERT(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];

	if (pC && pC->eCurType == CURTYPE_BTREE) {
		pCrsr = pC->uc.pCursor;
		if (pCrsr) {
			rc = __sm_next(pCrsr, &res);
			if (rc != DBSQL_SUCCESS)
				goto abort_due_to_error;

			pC->cacheStatus = CACHE_STALE;
			pC->nullRow = (u8)res;

			if (res == 0) {
				/* Not EOF - jump to P2 */
				pc = pOp->p2 - 1;
#ifdef CONFIG_TEST
				dbsql_search_count++;
#endif
			}
		} else {
			pC->nullRow = 1;
		}
	}
	break;
}

/* Opcode: OP_Prev P1 P2 * * *
 * Synopsis: move cursor P1 to previous, jump to P2 if not BOF
 *
 * Move cursor P1 to the previous entry.
 * If successful, jump to P2. If at BOF, fall through.
 * P1: cursor number
 * P2: jump destination if not BOF
 */
case OP_Prev: {
	VdbeCursor *pC;
	sm_cursor_t *pCrsr;
	int res;

	DBSQL_ASSERT(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];

	if (pC && pC->eCurType == CURTYPE_BTREE) {
		pCrsr = pC->uc.pCursor;
		if (pCrsr) {
			rc = __sm_prev(pCrsr, &res);
			if (rc != DBSQL_SUCCESS)
				goto abort_due_to_error;

			pC->cacheStatus = CACHE_STALE;
			pC->nullRow = (u8)res;

			if (res == 0) {
				/* Not BOF - jump to P2 */
				pc = pOp->p2 - 1;
#ifdef CONFIG_TEST
				dbsql_search_count++;
#endif
			}
		} else {
			pC->nullRow = 1;
		}
	}
	break;
}

/* Opcode: OP_Column P1 P2 P3 * *
 * Synopsis: r[P3] = column P2 from cursor P1
 *
 * Read column P2 from the current row of cursor P1 and store into register P3.
 * P1: cursor number
 * P2: column number
 * P3: destination register
 *
 * This is a simplified version that reads the column data using __sm_payload().
 * A full implementation would parse the SQLite 3.52 record format with serial types.
 *
 * For now, this implementation:
 * - Supports null rows (returns NULL)
 * - Supports pseudo-cursors (reads from register)
 * - Reads raw data from SM using __sm_payload()
 * - Does NOT parse SQLite 3.52 record format (needs future work)
 */
case OP_Column: {
	VdbeCursor *pC;
	sm_cursor_t *pCrsr;
	u32 payloadSize;
	u32 offset, amt;
	Mem *pDest;
	int p2;

	DBSQL_ASSERT(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	DBSQL_ASSERT(pOp->p3 > 0 && pOp->p3 <= p->nMem);

	pC = p->apCsr[pOp->p1];
	p2 = pOp->p2;
	pDest = &aMem[pOp->p3];

	/* Check cache first */
	if (pC && pC->cacheStatus == p->cacheCtr) {
		/* Cache hit - would use cached column data here */
		/* For now, fall through to re-read */
	}

	/* Handle null row */
	if (pC == 0 || pC->nullRow) {
		__vdbe_mem_set_null(pDest);
		break;
	}

	/* Handle pseudo-cursor (data in register) */
	if (pC->eCurType == CURTYPE_PSEUDO && pC->seekResult > 0) {
		Mem *pReg = &aMem[pC->seekResult];
		DBSQL_ASSERT(pReg->flags & MEM_Blob);
		/* Would parse record format here */
		/* For now, just copy the whole blob */
		__vdbe_mem_copy(pDest, pReg);
		break;
	}

	/* Read from btree cursor */
	if (pC->eCurType == CURTYPE_BTREE) {
		pCrsr = pC->uc.pCursor;
		if (pCrsr == 0) {
			__vdbe_mem_set_null(pDest);
			break;
		}

		/* Get payload size */
		if (pC->isTable) {
			/* Table: data is the payload */
			int sz;
			__sm_data_size(pCrsr, &sz);
			payloadSize = (u32)sz;
		} else {
			/* Index: key is the payload */
			int sz;
			__sm_key_size(pCrsr, &sz);
			payloadSize = (u32)sz;
		}

		if (payloadSize == 0) {
			__vdbe_mem_set_null(pDest);
			break;
		}

		/* Simplified column extraction (would parse record format in full impl) */
		/* For now, read the entire payload as a blob */
		/* FUTURE WORK: Parse SQLite 3.52 record format with serial types */

		if (p2 == 0) {
			/* First column - read entire payload for now */
			char *buf;
			if (__dbsql_calloc(NULL, 1, payloadSize, &buf) == ENOMEM)
				goto no_mem;

			if (pC->isTable) {
				rc = __sm_data(pCrsr, 0, payloadSize, buf);
			} else {
				rc = __sm_key(pCrsr, 0, payloadSize, buf);
			}

			if (rc != DBSQL_SUCCESS) {
				__dbsql_free(NULL, buf);
				goto abort_due_to_error;
			}

			__vdbe_mem_set_str(pDest, buf, payloadSize,
			                   DBSQL_UTF8_ENCODED, __dbsql_free);
		} else {
			/* Other columns - would parse record here */
			/* For now, return NULL */
			__vdbe_mem_set_null(pDest);
		}

		/* Update cache status */
		pC->cacheStatus = p->cacheCtr;
	}

	break;
}

/* Opcode: OP_Rowid P1 P2 * * *
 * Synopsis: r[P2] = rowid of cursor P1
 *
 * Store the integer rowid (key) from cursor P1 into register P2.
 * P1: cursor number
 * P2: destination register
 *
 * This replaces the old OP_Recno and uses __sm_integer_key() for direct
 * i64 rowid access (vs the old KEY_TO_INT byte-swap encoding).
 */
case OP_Rowid: {
	VdbeCursor *pC;
	i64 v;
	Mem *pOut;

	DBSQL_ASSERT(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	DBSQL_ASSERT(pOp->p2 > 0 && pOp->p2 <= p->nMem);

	pC = p->apCsr[pOp->p1];
	pOut = &aMem[pOp->p2];

	/* Handle null row or missing cursor */
	if (pC == 0 || pC->nullRow) {
		__vdbe_mem_set_null(pOut);
		break;
	}

	/* Handle deferred moveto (rowid known without actual seek) */
	if (pC->deferredMoveto) {
		v = pC->movetoTarget;
	}
	/* Handle pseudo-cursor (rowid in seekResult) */
	else if (pC->eCurType == CURTYPE_PSEUDO) {
		/* For pseudo-cursors, seekResult can indicate the register
		 * containing the data, not the rowid. Use 0 for now. */
		v = 0;
	}
	/* Read rowid from btree cursor */
	else if (pC->eCurType == CURTYPE_BTREE) {
		sm_cursor_t *pCrsr = pC->uc.pCursor;
		if (pCrsr == 0) {
			__vdbe_mem_set_null(pOut);
			break;
		}

		/* Use __sm_integer_key() for direct i64 rowid access */
		v = __sm_integer_key(pCrsr);
	}
	else {
		__vdbe_mem_set_null(pOut);
		break;
	}

	__vdbe_mem_set_int64(pOut, v);
	break;
}
