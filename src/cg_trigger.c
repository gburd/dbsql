/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * There are special exceptions to the terms and conditions of the GPL as it
 * is applied to this software. View the full text of the exception in file
 * LICENSE_EXCEPTIONS in the directory of this software distribution.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * $Id: cg_trigger.c 7 2007-02-03 13:34:17Z gburd $
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __vdbe_delete_trigger_step --
 *	Delete a linked list of TriggerStep structures.
 *
 * PUBLIC: void __vdbe_delete_trigger_step __P((trigger_step_t *));
 */
void
__vdbe_delete_trigger_step(ts)
	trigger_step_t *ts;
{
	while(ts) {
		trigger_step_t * tmp = ts;
		ts = ts->pNext;

		if (tmp->target.dyn)
			__dbsql_free(NULL, (char *)tmp->target.z);
		__expr_delete(tmp->pWhere);
		__expr_list_delete(tmp->pExprList);
		__select_delete(tmp->pSelect);
		__id_list_delete(tmp->pIdList);

		__dbsql_free(NULL, tmp);
	}
}

/*
 * __begin_trigger --
 *	This is called by the parser when it sees a CREATE TRIGGER statement
 *	up to the point of the BEGIN before the trigger actions.  A trigger_t
 *	structure is generated based on the information available and stored
 *	in parser->pNewTrigger.  After the trigger actions have been parsed,
 *	the __finish_trigger() function is called to complete the trigger
 *	construction process.
 *
 * PUBLIC: void __begin_trigger __P((parser_t *, token_t *, int, int,
 * PUBLIC:      id_list_t *, src_list_t *, int, expr_t *, int));
 *
 * parser			The parse context of the CREATE TRIGGER
 *                              statement
 * trigger				The trigger of the trigger
 * tr_tm			One of TK_BEFORE, TK_AFTER, TK_INSTEAD
 * op				One of TK_INSERT, TK_UPDATE, TK_DELETE
 * columns			Column list if this is an UPDATE OF trigger
 * tab_name			The table name of the table/view for this
 *                              trigger
 * foreach			One of TK_ROW or TK_STATEMENT
 * when_clause			WHEN clause
 * temp_p			True if the TEMPORARY keyword is present
 */
void
__begin_trigger(parser, trigger, tr_tm, op, columns, tab_name, foreach,
		when_clause, temp_p)
	parser_t *parser;
	token_t *trigger;
	int tr_tm;
	int op;
	id_list_t *columns;
	src_list_t *tab_name;
	int foreach;
	expr_t *when_clause;
	int temp_p;
{
	trigger_t *nt;
	table_t   *table;
	ref_normalizer_ctx_t normctx;
	DBSQL *dbp = parser->db;
	char *name = 0;        /* Name of the trigger */
	int db_idx;            /* When database to store the trigger in */

	/*
	 * Check that: 
	 * 1. The trigger name does not already exist.
	 * 2. The table (or view) does exist in the same database as the
	 *    trigger.
	 * 3. That we are not trying to create a trigger on either the
	 *    master or master temp tables.
	 * 4. That we are not trying to create an INSTEAD OF trigger on a
	 *    table.
	 * 5. That we are not trying to create a BEFORE or AFTER trigger on
	 *    a view.
	 */
	if (parser->rc == ENOMEM)
		goto trigger_cleanup;
	DBSQL_ASSERT(tab_name->nSrc == 1);
	if (parser->initFlag &&
	    __ref_normalizer_ctx_init(&normctx, parser, parser->iDb,
				      "trigger", trigger) &&
	    __ref_normalize_src_list(&normctx, tab_name)) {
		goto trigger_cleanup;
	}
	table = __src_list_lookup(parser, tab_name);
	if (!table) {
		goto trigger_cleanup;
	}
	db_idx = (temp_p ? 1 : table->iDb);
	if (db_idx >= 2 && !parser->initFlag) {
		__error_msg(parser, "triggers may not be added to auxiliary "
			    "database %s", dbp->aDb[table->iDb].zName);
		goto trigger_cleanup;
	}

	__dbsql_strndup(dbp, trigger->z, &name, trigger->n);
	__str_unquote(name);
	if( __hash_find(&(dbp->aDb[db_idx].trigHash), name,trigger->n+1) ){
		__error_msg(parser, "trigger %T already exists", trigger);
		goto trigger_cleanup;
	}
	if (strncasecmp(table->zName, MASTER_NAME,
				   strlen(MASTER_NAME)) == 0) {
		__error_msg(parser, "cannot create trigger on system table");
		parser->nErr++;
		goto trigger_cleanup;
	}
	if (strncasecmp(table->zName, TEMP_MASTER_NAME,
				   strlen(TEMP_MASTER_NAME)) == 0) {
		__error_msg(parser, "cannot create trigger on system table");
		parser->nErr++;
		goto trigger_cleanup;
	}
	if (table->pSelect && tr_tm != TK_INSTEAD) {
		__error_msg(parser, "cannot create %s trigger on view: %S", 
			    ((tr_tm == TK_BEFORE) ? "BEFORE" : "AFTER"),
			    tab_name, 0);
		goto trigger_cleanup;
	}
	if (!table->pSelect && tr_tm == TK_INSTEAD) {
		__error_msg(parser, "cannot create INSTEAD OF"
			    " trigger on table: %S", tab_name, 0);
		goto trigger_cleanup;
	}
#ifndef DBSQL_NO_AUTH
	{
		int code = DBSQL_CREATE_TRIGGER;
		const char *db = dbp->aDb[table->iDb].zName;
		const char *db_trig = temp_p ? dbp->aDb[1].zName : db;
		if (table->iDb == 1 || temp_p)
			code = DBSQL_CREATE_TEMP_TRIGGER;
		if (__auth_check(parser, code, name, table->zName, db_trig)) {
			goto trigger_cleanup;
		}
		if (__auth_check(parser, DBSQL_INSERT,
				 SCHEMA_TABLE(table->iDb), 0, db)) {
			goto trigger_cleanup;
		}
	}
#endif

	/*
	 * INSTEAD OF triggers can only appear on views and BEGIN triggers
	 * cannot appear on views.  So we might as well translate every
	 * INSTEAD OF trigger into a BEFORE trigger.  It simplifies code
	 * elsewhere.
	 */
	if (tr_tm == TK_INSTEAD) {
		tr_tm = TK_BEFORE;
	}

	/*
	 * Build the Trigger object.
	 */
	__dbsql_calloc(dbp, 1, sizeof(trigger_t), &nt);
	if (nt == 0)
		goto trigger_cleanup;
	nt->name = name;
	name = 0;
	if (__dbsql_strdup(dbp, tab_name->a[0].zName, &nt->table) == ENOMEM)
		goto trigger_cleanup;
	nt->iDb = db_idx;
	nt->iTabDb = table->iDb;
	nt->op = op;
	nt->tr_tm = tr_tm;
	nt->pWhen = __expr_dup(when_clause);
	nt->pColumns = __id_list_dup(columns);
	nt->foreach = foreach;
	__token_copy(&nt->nameToken,trigger);
	DBSQL_ASSERT( parser->pNewTrigger==0 );
	parser->pNewTrigger = nt;

  trigger_cleanup:
	__dbsql_free(dbp, name);
	__src_list_delete(tab_name);
	__id_list_delete(columns);
	__expr_delete(when_clause);
}

/*
 * __finish_trigger --
 *	This routine is called after all of the trigger actions have been
 *	parsed in order to complete the process of building the trigger.
 *
 * PUBLIC: void __finish_trigger __P((parser_t *, trigger_step_t *,
 * PUBLIC:      token_t *));
 *
 * parser			The parse context of the CREATE TRIGGER
 *                              statement
 * steplist			The triggered program
 * all				Token that describes the complete
 *                              CREATE TRIGGER
 */
void
__finish_trigger(parser, steplist, all)
	parser_t *parser;
	trigger_step_t *steplist;
	token_t *all;
{
	ref_normalizer_ctx_t normctx;
	trigger_t *nt = 0; /* The trigger whose construction is finishing up */
	DBSQL *dbp = parser->db;

	if (parser->nErr || parser->pNewTrigger == 0)
		goto triggerfinish_cleanup;
	nt = parser->pNewTrigger;
	parser->pNewTrigger = 0;
	nt->step_list = steplist;
	while(steplist) {
		steplist->pTrig = nt;
		steplist = steplist->pNext;
	}
	if (__ref_normalizer_ctx_init(&normctx, parser, nt->iDb,
				      "trigger", &nt->nameToken) &&
	    __ref_normalize_trigger_step(&normctx, nt->step_list)) {
		goto triggerfinish_cleanup;
	}

	/*
	 * if we are not initializing, and this trigger is not on a TEMP
	 * table, build the master entry.
	 */
	if (!parser->initFlag) {
		static vdbe_op_t insert_trig[] = {
		{ OP_NewRecno,   0, 0,  0          },
		{ OP_String,     0, 0,  "trigger"  },
		{ OP_String,     0, 0,  0          },  /* 2: trigger name */
		{ OP_String,     0, 0,  0          },  /* 3: table name */
		{ OP_Integer,    0, 0,  0          },
		{ OP_String,     0, 0,  0          },  /* 5: SQL */
		{ OP_MakeRecord, 5, 0,  0          },
		{ OP_PutIntKey,  0, 0,  0          },
		};
		int addr;
		vdbe_t *v;

		/*
		 * Make an entry in the master table.
		 */
		v = __parser_get_vdbe(parser);
		if (v == 0)
			goto triggerfinish_cleanup;
		__vdbe_prepare_write(parser, 0, 0);
		__open_master_table(v, nt->iDb);
		addr = __vdbe_add_op_list(v, ARRAY_SIZE(insert_trig),
					  insert_trig);
		__vdbe_change_p3(v, (addr + 2), nt->name, 0); 
		__vdbe_change_p3(v, (addr + 3), nt->table, 0); 
		__vdbe_change_p3(v, (addr + 5), all->z, all->n);
		if (nt->iDb == 0) {
			__change_schema_signature(dbp, v);
		}
		__vdbe_add_op(v, OP_Close, 0, 0);
		__vdbe_conclude_write(parser);
	}

	if (!parser->explain) {
		table_t *table;
		__hash_insert(&dbp->aDb[nt->iDb].trigHash, 
			      nt->name, (strlen(nt->name) + 1), nt);
		table = __locate_table(parser, nt->table,
				       dbp->aDb[nt->iTabDb].zName);
		DBSQL_ASSERT(table != 0);
		nt->pNext = table->pTrigger;
		table->pTrigger = nt;
		nt = 0;
	}

  triggerfinish_cleanup:
	__vdbe_delete_trigger(nt);
	__vdbe_delete_trigger(parser->pNewTrigger);
	parser->pNewTrigger = 0;
	__vdbe_delete_trigger_step(steplist);
}

/*
 * __persist_trigger_step --
 *	Make a copy of all components of the given trigger step.  This has
 *	the effect of copying all expr_t.token.z values into memory obtained
 *	from __dbsql_calloc().  As initially created, the expr_t.token.z values
 *	all point to the input string that was fed to the parser.  But that
 *	string is ephemeral - it will go away as soon as the dbsql_exec()
 *	call that started the parser exits.  This routine makes a persistent
 *	copy of all the Expr.token.z strings so that the trigger_step_t
 *	structure will be valid even after the dbsql_exec() call returns.
 *
 * STATIC: static void __persist_trigger_step __P((trigger_step_t *));
 */
static void
__persist_trigger_step(ts)
	trigger_step_t *ts;
{
	if (ts->target.z) {
		__dbsql_strndup(NULL, ts->target.z, &ts->target.z, ts->target.n);
		ts->target.dyn = 1;
	}
	if (ts->pSelect) {
		select_t *new = __select_dup(ts->pSelect);
		__select_delete(ts->pSelect);
		ts->pSelect = new;
	}
	if( ts->pWhere ){
		expr_t *new = __expr_dup(ts->pWhere);
		__expr_delete(ts->pWhere);
		ts->pWhere = new;
	}
	if (ts->pExprList) {
		expr_list_t *new = __expr_list_dup(ts->pExprList);
		__expr_list_delete(ts->pExprList);
		ts->pExprList = new;
	}
	if (ts->pIdList) {
		id_list_t *new = __id_list_dup(ts->pIdList);
		__id_list_delete(ts->pIdList);
		ts->pIdList = new;
	}
}

/*
 * __trigger_select_step --
 *	Turn a SELECT statement (that the 'select' parameter points to) into
 *	a trigger step.  Return a pointer to a trigger_step_t structure.
 *
 *	The parser calls this routine when it finds a SELECT statement in
 *	body of a TRIGGER.
 *
 * PUBLIC: trigger_step_t * __trigger_select_step __P((select_t *));
 */
trigger_step_t *
__trigger_select_step(select)
	select_t *select;
{
	trigger_step_t *ts;
	
	if (__dbsql_calloc(NULL, 1, sizeof(trigger_step_t), &ts) == ENOMEM)
		return 0;

	ts->op = TK_SELECT;
	ts->pSelect = select;
	ts->orconf = OE_Default;
	__persist_trigger_step(ts);

	return ts;
}

/*
 * __trigger_insert_step --
 *	Build a trigger step out of an INSERT statement.  Return a pointer
 *	to the new trigger step.
 *
 *	The parser calls this routine when it sees an INSERT inside the
 *	body of a trigger.
 *
 * PUBLIC: trigger_step_t *__trigger_insert_step __P((token_t *, id_list_t *,
 * PUBLIC:                 expr_list_t *, select_t *, int));
 *
 * tab_name			Name of the table into which we insert
 * column			List of columns in tab_name to insert into
 * elist			The VALUE clause: a list of values to be
 *                              inserted
 * select			A SELECT statement that supplies values
 * orconf			The conflict algorithm (OE_Abort, OE_Replace,
 *                              etc.)
 */
trigger_step_t *
__trigger_insert_step(tab_name, column, elist, select, orconf)
	token_t *tab_name;
	id_list_t *column;
	expr_list_t *elist;
	select_t *select;
	int orconf;
{
	trigger_step_t *ts;

	if (__dbsql_calloc(NULL, 1, sizeof(trigger_step_t), &ts) == ENOMEM)
		return 0;

	DBSQL_ASSERT(elist == 0 || select == 0);
	DBSQL_ASSERT(elist != 0 || select != 0);

	ts->op = TK_INSERT;
	ts->pSelect = select;
	ts->target  = *tab_name;
	ts->pIdList = column;
	ts->pExprList = elist;
	ts->orconf = orconf;
	__persist_trigger_step(ts);

	return ts;
}

/*
 * __trigger_update_step --
 *	Construct a trigger step that implements an UPDATE statement and return
 *	a pointer to that trigger step.  The parser calls this routine when it
 *	sees an UPDATE statement inside the body of a CREATE TRIGGER.
 *
 * PUBLIC: trigger_step_t *__trigger_update_step __P((token_t *, expr_list_t *,
 * PUBLIC:                 expr_t *, int));
 *
 * tab_name			Name of the table to be updated
 * elist			The SET clause: list of column and new values
 * where_clause			The WHERE clause
 * orconf			The conflict algorithm. (OE_Abort, OE_Ignore,
 *                              etc)
 */
trigger_step_t *
__trigger_update_step(tab_name, elist, where_clause, orconf)
	token_t *tab_name;
	expr_list_t *elist;
	expr_t *where_clause;
	int orconf;
{
	trigger_step_t *ts;

	if (__dbsql_calloc(NULL, 1, sizeof(trigger_step_t), &ts) == ENOMEM)
		return 0;

	ts->op = TK_UPDATE;
	ts->target  = *tab_name;
	ts->pExprList = elist;
	ts->pWhere = where_clause;
	ts->orconf = orconf;
	__persist_trigger_step(ts);

	return ts;
}

/*
 * __trigger_delete_step --
 *	Construct a trigger step that implements a DELETE statement and return
 *	a pointer to that trigger step.  The parser calls this routine when it
 *	sees a DELETE statement inside the body of a CREATE TRIGGER.
 *
 * PUBLIC: trigger_step_t *__trigger_delete_step __P((token_t *, expr_t *));
 */
trigger_step_t *
__trigger_delete_step(tab_name, where_clause)
	token_t *tab_name;
	expr_t *where_clause;
{
	trigger_step_t *ts;

	if (__dbsql_calloc(NULL, 1, sizeof(trigger_step_t), &ts) == ENOMEM)
		return 0;

	ts->op = TK_DELETE;
	ts->target  = *tab_name;
	ts->pWhere = where_clause;
	ts->orconf = OE_Default;
	__persist_trigger_step(ts);

	return ts;
}

/*
 * __vdbe_delete_trigger --
 *	Recursively delete a trigger_t structure
 *
 * PUBLIC: void __vdbe_delete_trigger __P((trigger_t *));
 */
void
__vdbe_delete_trigger(trigger)
	trigger_t *trigger;
{
	if (trigger == 0)
		return;
	__vdbe_delete_trigger_step(trigger->step_list);
	__dbsql_free(NULL, trigger->name);
	__dbsql_free(NULL, trigger->table);
	__expr_delete(trigger->pWhen);
	__id_list_delete(trigger->pColumns);
	if (trigger->nameToken.dyn)
		__dbsql_free(NULL, (char*)trigger->nameToken.z);
	__dbsql_free(NULL, trigger);
}

/*
 * __drop_trigger --
 *	This function is called to drop a trigger from the database schema. 
 *
 *	This may be called directly from the parser and therefore identifies
 *	the trigger by name.  The __drop_trigger_ptr() routine does the
 *	same job as this routine except it take a spointer to the trigger
 *	instead of the trigger name.
 *
 *	Note that this function does not delete the trigger entirely. Instead
 *	it removes it from the internal schema and places it in the trigDrop
 *	hash table. This is so that the trigger can be restored into the
 *	database schema if the transaction is rolled back.
 *
 * PUBLIC: void __drop_trigger __P((parser_t *, src_list_t *));
 */
void
__drop_trigger(parser, trig_list)
	parser_t *parser;
	src_list_t *trig_list;
{
	int i;
	trigger_t *trigger;
	const char *db_name;
	const char *trig_name;
	int name_len;
	DBSQL *dbp = parser->db;

	if (parser->rc == ENOMEM)
		goto err;
	DBSQL_ASSERT(trig_list->nSrc == 1);
	db_name = trig_list->a[0].zDatabase;
	trig_name = trig_list->a[0].zName;
	name_len = strlen(trig_name);
	for (i = 0; i < dbp->nDb; i++) {
		int j = (i < 2) ? (i ^ 1) : i;  /* Search TEMP before MAIN. */
		if (db_name &&
		    strcasecmp(dbp->aDb[j].zName, db_name))
			continue;
		trigger = __hash_find(&(dbp->aDb[j].trigHash), trig_name,
				      (name_len + 1));
		if (trigger)
			break;
	}
	if (!trigger) {
		__error_msg(parser, "no such trigger: %S", trig_list, 0);
		goto err;
	}
	__drop_trigger_ptr(parser, trigger, 0);

  err:
	__src_list_delete(trig_list);
}

/*
 * __drop_trigger_ptr --
 *	Drop a trigger given a pointer to that trigger.  If nested is false,
 *	then also generate code to remove the trigger from the DBSQL_MASTER
 *	table.
 *
 * PUBLIC: void __drop_trigger_ptr __P((parser_t *, trigger_t *, int));
 */
void
__drop_trigger_ptr(parser, trigger, nested)
	parser_t *parser;
	trigger_t *trigger;
	int nested;
{
	table_t *table;
	vdbe_t *v;
	DBSQL *dbp = parser->db;

	DBSQL_ASSERT(trigger->iDb < dbp->nDb);
	if (trigger->iDb >= 2) {
		__error_msg(parser, "triggers may not be removed from "
			    "auxiliary database %s",
			    dbp->aDb[trigger->iDb].zName);
		return;
	}
	table = __find_table(dbp, trigger->table,
			     dbp->aDb[trigger->iTabDb].zName);
	DBSQL_ASSERT(table);
	DBSQL_ASSERT(table->iDb == trigger->iDb || trigger->iDb == 1);
#ifndef DBSQL_NO_AUTH
	{
		int code = DBSQL_DROP_TRIGGER;
		const char *db = dbp->aDb[trigger->iDb].zName;
		const char *tab = SCHEMA_TABLE(trigger->iDb);
		if (trigger->iDb)
			code = DBSQL_DROP_TEMP_TRIGGER;
		if (__auth_check(parser, code, trigger->name,
				 table->zName, db) ||
		    __auth_check(parser, DBSQL_DELETE, tab, 0, db)) {
			return;
		}
	}
#endif

	/*
	 * Generate code to destroy the database record of the trigger.
	 */
	if (table != 0 && !nested && (v = __parser_get_vdbe(parser)) != 0) {
		int base;
		static vdbe_op_t drop_trigger[] = {
			{ OP_Rewind,     0, ADDR(9),  0},
			{ OP_String,     0, 0,        0}, /* 1 */
			{ OP_Column,     0, 1,        0},
			{ OP_Ne,         0, ADDR(8),  0},
			{ OP_String,     0, 0,        "trigger"},
			{ OP_Column,     0, 0,        0},
			{ OP_Ne,         0, ADDR(8),  0},
			{ OP_Delete,     0, 0,        0},
			{ OP_Next,       0, ADDR(1),  0}, /* 8 */
		};

		__vdbe_prepare_write(parser, 0, 0);
		__open_master_table(v, trigger->iDb);
		base = __vdbe_add_op_list(v,  ARRAY_SIZE(drop_trigger),
					  drop_trigger);
		__vdbe_change_p3(v, base+1, trigger->name, 0);
		if (trigger->iDb == 0) {
			__change_schema_signature(dbp, v);
		}
		__vdbe_add_op(v, OP_Close, 0, 0);
		__vdbe_conclude_write(parser);
	}

	/*
	 * If this is not an "explain", then delete the trigger structure.
	 */
	if (!parser->explain) {
		const char *name = trigger->name;
		int len = strlen(name);
		if (table->pTrigger == trigger) {
			table->pTrigger = trigger->pNext;
		} else {
			trigger_t *cc = table->pTrigger;
			while(cc) { 
				if (cc->pNext == trigger) {
					cc->pNext = cc->pNext->pNext;
					break;
				}
				cc = cc->pNext;
			}
			DBSQL_ASSERT(cc);
		}
		__hash_insert(&(dbp->aDb[trigger->iDb].trigHash), name,
			      (len + 1), 0);
		__vdbe_delete_trigger(trigger);
	}
}

/*
 * __check_column_overlap --
 *	'elist' is the SET clause of an UPDATE statement.  Each entry
 *	in 'elist' is of the format <id>=<expr>.  If any of the entries
 *	in 'elist' have an <id> which matches an identifier in 'id_list',
 *	then return TRUE.  If 'id_list'==NULL, then it is considered a
 *	wildcard that matches anything.  Likewise if elist==NULL then
 *	it matches anything so always return true.  Return false only
 *	if there is no match.
 *
 * STATIC: static int __check_column_overlap __P((id_list_t *, expr_list_t *));
 */
static int
__check_column_overlap(id_list, elist)
	id_list_t *id_list;
	expr_list_t *elist;
{
	int e;
	if (!id_list || !elist)
		return 1;
	for (e = 0; e < elist->nExpr; e++) {
		if (__id_list_index(id_list, elist->a[e].zName) >= 0)
			return 1;
	}
	return 0; 
}

/*
 * A global variable that is TRUE if we should always set up temp tables for
 * for triggers, even if there are no triggers to code. This is used to test 
 * how much overhead the triggers algorithm is causing.
 *
 * This flag can be set or cleared using the "trigger_overhead_test" pragma.
 * The pragma is not documented since it is not really part of the public
 * interface, just the test procedure.
 */
int always_code_trigger_setup = 0; /* TODO, remove this global, place it
				      in the environment. */

/*
 * __triggers_exist --
 *	Returns true if a trigger matching op, tr_tm and foreach that is NOT
 *	already on the parser_t objects trigger-stack (to prevent recursive
 *	trigger firing) is found in the list specified as trigger.
 *
 * PUBLIC: int __triggers_exist __P((parser_t *, trigger_t *, int, int, int,
 * PUBLIC:     expr_list_t *));
 *
 * parser			Used to check for recursive triggers
 * trigger			A list of triggers associated with a table
 * op				One of TK_DELETE, TK_INSERT, TK_UPDATE
 * tr_tm			One of TK_BEFORE, TK_AFTER
 * foreach			One of TK_ROW or TK_STATEMENT
 * changes			Columns that change in an UPDATE statement
 */
int __triggers_exist(parser, trigger, op, tr_tm, foreach, changes)
	parser_t *parser;
	trigger_t *trigger;
	int op;
	int tr_tm;
	int foreach;
	expr_list_t *changes;
{
	trigger_t * trig_cursor;

	if (always_code_trigger_setup) {
		return 1;
	}
	trig_cursor = trigger;
	while (trig_cursor) {
		if (trig_cursor->op == op && 
		    trig_cursor->tr_tm == tr_tm && 
		    trig_cursor->foreach == foreach &&
		    __check_column_overlap(trig_cursor->pColumns, changes)) {
			trigger_stack_t *ss;
			ss = parser->trigStack;
			while (ss && ss->pTrigger != trigger) {
				ss = ss->pNext;
			}
			if (!ss)
				return 1;
		}
		trig_cursor = trig_cursor->pNext;
	}
	return 0;
}

/*
 * __target_src_list --
 *	Convert the pStep->target token into a src_list_t and return a pointer
 *	to that src_list_t.
 *
 *	This routine adds a specific database name, if needed, to the target
 *	when forming the src_list_t.  This prevents a trigger in one database
 *	from referring to a target in another database.  An exception is when
 *	the trigger is in TEMP in which case it can refer to any other database
 *	it wants.
 *
 * STATIC: static src_list_t *__target_src_list __P((parser_t *,
 * STATIC:                    trigger_step_t *));
 *
 * parser			The parsing context
 * step				The trigger containing the target token
 */
static src_list_t *targetSrcList(parser, step)
	parser_t *parser;
	trigger_step_t *step;
{
	token_t db;           /* Dummy database name token */
	int db_idx;           /* Index of the database to use */
	src_list_t *src;      /* SrcList to be returned */

	db_idx = step->pTrig->iDb;
	if (db_idx == 0 || db_idx >= 2) {
		DBSQL_ASSERT(db_idx < parser->db->nDb);
		db.z = parser->db->aDb[db_idx].zName;
		db.n = strlen(db.z);
		src = __src_list_append(0, &db, &step->target);
	} else {
		src = __src_list_append(0, &step->target, 0);
	}
	return src;
}

/*
 * __code_trigger_prigram --
 *	Generate VDBE code for zero or more statements inside the body of a
 *	trigger.  
 *
 * STATIC: static int __code_trigger_prigram __P(());
 *
 * parser			The parser context
 * steplist			List of statements inside the trigger body
 * orconfin			Conflict algorithm. (OE_Abort, etc)
 */
static int
__code_trigger_program(parser, steplist, orconfin)
	parser_t *parser;
	trigger_step_t *steplist;
	int orconfin;
{
	int orconf;
	trigger_step_t * ts = steplist;

	while (ts) {
		int saveNTab = parser->nTab;
		orconf = (orconfin == OE_Default)?ts->orconf:orconfin;
		parser->trigStack->orconf = orconf;
		switch(ts->op) {
		case TK_SELECT: {
			select_t * ss = __select_dup(ts->pSelect);
			DBSQL_ASSERT(ss);
			DBSQL_ASSERT(ss->pSrc);
			__select(parser, ss, SRT_Discard, 0, 0, 0, 0);
			__select_delete(ss);
			break;
		}
		case TK_UPDATE: {
			src_list_t *src;
			src = targetSrcList(parser, ts);
			__vdbe_add_op(parser->pVdbe, OP_ListPush, 0, 0);
			__update(parser, src,
				 __expr_list_dup(ts->pExprList), 
				 __expr_dup(ts->pWhere), orconf);
			__vdbe_add_op(parser->pVdbe, OP_ListPop, 0, 0);
			break;
		}
		case TK_INSERT: {
			src_list_t *src;
			src = targetSrcList(parser, ts);
			__insert(parser, src,
				 __expr_list_dup(ts->pExprList), 
				 __select_dup(ts->pSelect), 
				 __id_list_dup(ts->pIdList), orconf);
			break;
		}
		case TK_DELETE: {
			src_list_t *src;
			__vdbe_add_op(parser->pVdbe, OP_ListPush, 0, 0);
			src = targetSrcList(parser, ts);
			__delete_from(parser, src, __expr_dup(ts->pWhere));
			__vdbe_add_op(parser->pVdbe, OP_ListPop, 0, 0);
			break;
		}
		default:
			DBSQL_ASSERT(0);
		} 
		parser->nTab = saveNTab;
		ts = ts->pNext;
	}
	return 0;
}

/*
 * __code_row_trigger --
 * This is called to code FOR EACH ROW triggers.
 *
 *	When the code that this function generates is executed, the following 
 *	must be true:
 *
 *	1. No cursors may be open in the main database.  (But newIdx and oldIdx
 *	   can be indices of cursors in temporary tables.  See below.)
 *
 *	2. If the triggers being coded are ON INSERT or ON UPDATE triggers,
 *	   then a temporary vdbe cursor (index newIdx) must be open and
 *	   pointing at a row containing values to be substituted for new.*
 *	   expressions in the trigger program(s).
 *
 *	3. If the triggers being coded are ON DELETE or ON UPDATE triggers,
 *	   then a temporary vdbe cursor (index oldIdx) must be open and
 *	   pointing at a row containing values to be substituted for old.*
 *	   expressions in the trigger program(s).
 *
 * PUBlIC: int __code_row_trigger __P(());
 *
 * parser			Parser context
 * op				One of TK_UPDATE, TK_INSERT, TK_DELETE
 * changes			Changes list for any UPDATE OF triggers
 * tr_tm			One of TK_BEFORE, TK_AFTER
 * table			The table to code triggers from
 * new_idx			The indice of the "new" row to access
 * old_idx			The indice of the "old" row to access
 * orconf			ON CONFLICT policy
 * ignore_jump			Instruction to jump to for RAISE(IGNORE)
 */
int __code_row_trigger(parser, op, changes, tr_tm, table, new_idx, old_idx,
		       orconf, ignore_jump)
	parser_t *parser;
	int op;
	expr_list_t *changes;
	int tr_tm;
	table_t *table;
	int new_idx;
	int old_idx;
	int orconf;
	int ignore_jump;
{
	trigger_t * trigger;
	trigger_stack_t * trig_stack;
	DBSQL *dbp = parser->db;

	DBSQL_ASSERT(op == TK_UPDATE || op == TK_INSERT || op == TK_DELETE);
	DBSQL_ASSERT(tr_tm == TK_BEFORE || tr_tm == TK_AFTER );

	DBSQL_ASSERT(new_idx != -1 || old_idx != -1);

	trigger = table->pTrigger;
	while(trigger) {
		int fire_this = 0;

		/*
		 * Determine whether we should code this trigger.
		 */
		if (trigger->op == op && trigger->tr_tm == tr_tm && 
		    trigger->foreach == TK_ROW) {
			fire_this = 1;
			trig_stack = parser->trigStack;
			while(trig_stack) {
				if (trig_stack->pTrigger == trigger) {
					fire_this = 0;
				}
				trig_stack = trig_stack->pNext;
			}
			if (op == TK_UPDATE && trigger->pColumns &&
			    !__check_column_overlap(trigger->pColumns,
						    changes)) {
				fire_this = 0;
			}
		}

		if (fire_this && __dbsql_calloc(dbp, 1,sizeof(trigger_stack_t),
					     &trig_stack) != ENOMEM) {
			int end_trigger;
			src_list_t dummy_tab_list;
			expr_t * when_expr;
			auth_context_t authctx;

			dummy_tab_list.nSrc = 0;

			/*
			 * Push an entry on to the trigger stack.
			 */
			trig_stack->pTrigger = trigger;
			trig_stack->newIdx = new_idx;
			trig_stack->oldIdx = old_idx;
			trig_stack->pTab = table;
			trig_stack->pNext = parser->trigStack;
			trig_stack->ignoreJump = ignore_jump;
			parser->trigStack = trig_stack;
			__auth_context_push(parser, &authctx, trigger->name);

			/*
			 * Code the WHEN clause.
			 */
			end_trigger = __vdbe_make_label(parser->pVdbe);
			when_expr = __expr_dup(trigger->pWhen);
			if (__expr_resolve_ids(parser, &dummy_tab_list, 0,
					       when_expr)) {
				parser->trigStack = parser->trigStack->pNext;
				__dbsql_free(dbp, trig_stack);
				__expr_delete(when_expr);
				return 1;
			}
			__expr_if_false(parser, when_expr, end_trigger, 1);
			__expr_delete(when_expr);

			__code_trigger_program(parser, trigger->step_list,
					       orconf); 

			/*
			 * Pop the entry off the trigger stack.
			 */
			parser->trigStack = parser->trigStack->pNext;
			__auth_context_pop(&authctx);
			__dbsql_free(dbp, trig_stack);

			__vdbe_resolve_label(parser->pVdbe, end_trigger);
		}
		trigger = trigger->pNext;
	}
	return 0;
}
