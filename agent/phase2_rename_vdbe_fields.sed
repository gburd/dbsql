#!/usr/bin/sed -f
# Phase 2 vdbe_t field renames
# Usage: sed -i.bak -f phase2_rename_vdbe_fields.sed <file>

# Linked list navigation (must come before pNext/pPrev checks in other contexts)
s/\bpPrev\b/prev_vdbe/g
s/\bpNext\b/next_vdbe/g

# Single pointers
s/\bpParse\b/parser/g
s/\bpResultRow\b/result_row/g
s/\bpFrame\b/parent_frame/g
s/\bpDelFrame\b/frame_free_list/g
s/\bpProgram\b/sub_programs/g
s/\bpAuxData\b/aux_data/g
s/\bpFree\b/free_context/g

# Arrays and array pointers
s/\baMem\b/registers/g
s/\bapArg\b/arg_ptrs/g
s/\bapCsr\b/cursors/g
s/\baVar\b/variables/g
s/\baOp\b/opcodes/g
s/\baColName\b/column_names/g
s/\baLabel\b/labels/g
