/*
 * Stub implementations for unconverted functions
 *
 * These stubs allow the dbsql binary to link successfully for testing
 * core CRUD operations (CREATE, INSERT, SELECT, DELETE, ORDER BY).
 *
 * Functions from unconverted files:
 * - cg_attach.c: ATTACH/DETACH database support
 * - cg_update.c: UPDATE statement support
 * - cg_trigger.c: TRIGGER support
 * - cg_insert.c: Advanced constraint checking
 */

#include "dbsql_int.h"

/* ATTACH/DETACH stubs (from cg_attach.c) */
void __attach(parser_t *parser, token_t *name, token_t *type) {
	(void)parser; (void)name; (void)type;
	/* ATTACH not yet implemented in register-based code */
}

void __detach(parser_t *parser, token_t *db_name) {
	(void)parser; (void)db_name;
	/* DETACH not yet implemented in register-based code */
}

int __ref_normalizer_ctx_init(ref_normalizer_ctx_t *normctx, parser_t *parser,
                               int iDb, const char *db_name,
                               const token_t *name) {
	(void)normctx; (void)parser; (void)iDb; (void)db_name; (void)name;
	return 0;
}

int __ref_normalize_src_list(ref_normalizer_ctx_t *normctx,
                              src_list_t *src_list) {
	(void)normctx; (void)src_list;
	return 0;
}

int __ref_normalize_select(ref_normalizer_ctx_t *normctx, select_t *select) {
	(void)normctx; (void)select;
	return 0;
}

int __ref_normalize_expr(ref_normalizer_ctx_t *normctx, expr_t *expr) {
	(void)normctx; (void)expr;
	return 0;
}

int __ref_normalize_expr_list(ref_normalizer_ctx_t *normctx,
                               expr_list_t *elist) {
	(void)normctx; (void)elist;
	return 0;
}

int __ref_normalize_trigger_step(ref_normalizer_ctx_t *normctx,
                                  trigger_step_t *step) {
	(void)normctx; (void)step;
	return 0;
}

/* UPDATE stubs (from cg_update.c) */
void __update(parser_t *parser, src_list_t *tab_list, expr_list_t *changes,
              expr_t *where, int on_error) {
	(void)parser; (void)tab_list; (void)changes; (void)where; (void)on_error;
	/* UPDATE not yet implemented in register-based code */
}

/* TRIGGER stubs (from cg_trigger.c) */
void __begin_trigger(parser_t *parser, token_t *name1, int tr_tm, int op,
                     id_list_t *columns, src_list_t *tab_list, int isTemp,
                     expr_t *when, int noErr) {
	(void)parser; (void)name1; (void)tr_tm; (void)op;
	(void)columns; (void)tab_list; (void)isTemp; (void)when; (void)noErr;
	/* CREATE TRIGGER not yet implemented */
}

void __finish_trigger(parser_t *parser, trigger_step_t *step_list,
                      token_t *all) {
	(void)parser; (void)step_list; (void)all;
	/* CREATE TRIGGER not yet implemented */
}

void __drop_trigger(parser_t *parser, src_list_t *name) {
	(void)parser; (void)name;
	/* DROP TRIGGER not yet implemented */
}

void __drop_trigger_ptr(parser_t *parser, trigger_t *trigger, int nested) {
	(void)parser; (void)trigger; (void)nested;
	/* DROP TRIGGER not yet implemented */
}

trigger_step_t *__trigger_select_step(select_t *select) {
	(void)select;
	return NULL;
}

trigger_step_t *__trigger_insert_step(token_t *b, id_list_t *columns,
                                       expr_list_t *elist, select_t *select,
                                       int orconf) {
	(void)b; (void)columns; (void)elist; (void)select; (void)orconf;
	return NULL;
}

trigger_step_t *__trigger_update_step(token_t *b, expr_list_t *elist,
                                       expr_t *where, int orconf) {
	(void)b; (void)elist; (void)where; (void)orconf;
	return NULL;
}

trigger_step_t *__trigger_delete_step(token_t *b, expr_t *where) {
	(void)b; (void)where;
	return NULL;
}

void __vdbe_delete_trigger_step(trigger_step_t *trigger_step) {
	(void)trigger_step;
	/* Trigger cleanup not yet implemented */
}

void __vdbe_delete_trigger(trigger_t *trigger) {
	(void)trigger;
	/* Trigger cleanup not yet implemented */
}

int __triggers_exist(parser_t *parser, trigger_t *trigger, int op,
                     int before, int after, expr_list_t *changes) {
	(void)parser; (void)trigger; (void)op; (void)before; (void)after;
	(void)changes;
	return 0; /* No triggers */
}

int __code_row_trigger(void) {
	/* Trigger execution not yet implemented */
	return 0;
}

/* Trigger setup variable */
int always_code_trigger_setup = 0;

/* Advanced constraint checking stubs (from cg_insert.c old version) */
void __generate_constraint_checks(parser_t *parser, table_t *table,
                                   int base_reg, char *zWhere,
                                   int rowidChng, int pkChng,
                                   int overrideError, int ignoreDest) {
	(void)parser; (void)table; (void)base_reg; (void)zWhere;
	(void)rowidChng; (void)pkChng; (void)overrideError; (void)ignoreDest;
	/* Advanced constraint checking not yet implemented */
}

void __complete_insertion(parser_t *parser, table_t *table, int base_reg,
                          char *zWhere, int rowidChng, int isUpdate,
                          int newIdx) {
	(void)parser; (void)table; (void)base_reg; (void)zWhere;
	(void)rowidChng; (void)isUpdate; (void)newIdx;
	/* Completion logic not yet implemented */
}

void __dbsql_assert(void) {
	/* Assert stub */
}
