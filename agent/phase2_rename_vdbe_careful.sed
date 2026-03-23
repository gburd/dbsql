#!/usr/bin/sed -f
# Phase 2 vdbe_t field renames - CAREFUL VERSION
# Only renames fields that are clearly vdbe_t members based on context

# Handle explicit vdbe_t/vdbe_frame_t field accesses with -> or .
# These patterns match: (struct_ptr)->field or (struct_var).field

# vdbe_t linked list (pPrev, pNext on vdbe_t only - very distinctive pattern)
s/\(vdbe_t *\*\)\{0,1\}\([a-zA-Z_][a-zA-Z0-9_]*\)->pPrev\b/\1\2->prev_vdbe/g
s/\(vdbe_t *\*\)\{0,1\}\([a-zA-Z_][a-zA-Z0-9_]*\)->pNext\b/\1\2->next_vdbe/g
s/\(vdbe_t *\*\)\{0,1\}\([a-zA-Z_][a-zA-Z0-9_]*\)\.pPrev\b/\1\2.prev_vdbe/g
s/\(vdbe_t *\*\)\{0,1\}\([a-zA-Z_][a-zA-Z0-9_]*\)\.pNext\b/\1\2.next_vdbe/g

# vdbe_t single pointers
s/->pParse\b/->parser/g
s/->pResultRow\b/->result_row/g
s/->pFree\b/->free_context/g

# vdbe_t and vdbe_frame_t shared fields
s/->pFrame\b/->parent_frame/g
s/->pDelFrame\b/->frame_free_list/g
s/->pProgram\b/->sub_programs/g
s/->pAuxData\b/->aux_data/g

# vdbe_t arrays
s/->aMem\b/->registers/g
s/->apArg\b/->arg_ptrs/g
s/->apCsr\b/->cursors/g
s/->aVar\b/->variables/g
s/->aOp\b/->opcodes/g
s/->aColName\b/->column_names/g
s/->aLabel\b/->labels/g
