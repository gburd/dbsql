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
 * $Id: sql_parser.y 7 2007-02-03 13:34:17Z gburd $
 */

/*
 * This file contains the grammar for SQL92 thus far.  Process this file
 * using the included lemon parser generator to generate C code that runs
 * the parser.  Lemon will also generate a header file containing
 * numeric codes for all of the tokens.
 */
%token_prefix TK_
%token_type {token_t}
%default_type {token_t}
%extra_argument {parser_t *pParse}
%syntax_error {
  if( pParse->zErrMsg==0 ){
    if( TOKEN.z[0] ){
      __set_stringn(&pParse->zErrMsg, 
          "near \"", -1, TOKEN.z, TOKEN.n, "\": syntax error", -1, 0);
    }else{
      __set_string(&pParse->zErrMsg, "incomplete SQL statement", (char*)0);
    }
  }
  pParse->nErr++;
}
%name dbsql_parser
%include {
#include "dbsql_config.h"
#include "dbsql_int.h"
#include "sql_parser.h"

/*
 * An instance of this structure holds information about the
 * LIMIT clause of a SELECT statement.
 */
struct LimitVal {
  int limit;    /* The LIMIT value.  -1 if there is no limit */
  int offset;   /* The OFFSET.  0 if there is none */
};

/*
 * An instance of the following structure describes the event of a
 * TRIGGER.  "a" is the event type, one of TK_UPDATE, TK_INSERT,
 * TK_DELETE, or TK_INSTEAD.  If the event is of the form
 *
 *      UPDATE ON (a,b,c)
 *
 * Then the "b" id_list_t records the list "a,b,c".
 */
struct TrigEvent { int a; id_list_t * b; };

} // end %include

// These are extra tokens used by the lexer but never seen by the
// parser.  We put them in a rule so that the parser generator will
// add them to the sql_parser.h output file.
//
%nonassoc END_OF_FILE ILLEGAL SPACE UNCLOSED_STRING COMMENT FUNCTION
          COLUMN AGG_FUNCTION.

// Input is zero or more commands.
input ::= cmdlist.

// A list of commands is zero or more commands
//
cmdlist ::= ecmd.
cmdlist ::= cmdlist ecmd.
ecmd ::= explain cmdx SEMI.
ecmd ::= SEMI.
cmdx ::= cmd.           { __parse_exec(pParse); }
explain ::= EXPLAIN.    { __parse_begin(pParse, 1); }
explain ::= .           { __parse_begin(pParse, 0); }

///////////////////// Begin and end transactions. ////////////////////////////
//

cmd ::= BEGIN trans_opt onconf(R).  {__dbsql_txn_begin(pParse,R);}
trans_opt ::= .
trans_opt ::= TRANSACTION.
trans_opt ::= TRANSACTION nm.
cmd ::= COMMIT trans_opt.      {__dbsql_txn_commit(pParse);}
cmd ::= END trans_opt.         {__dbsql_txn_commit(pParse);}
cmd ::= ROLLBACK trans_opt.    {__dbsql_txn_abort(pParse);}

///////////////////// The CREATE TABLE statement ////////////////////////////
//
cmd ::= create_table create_table_args.
create_table ::= CREATE(X) temp(T) TABLE nm(Y). {
   __start_table(pParse,&X,&Y,T,0);
}
%type temp {int}
temp(A) ::= TEMP.  {A = 1;}
temp(A) ::= .      {A = 0;}
create_table_args ::= LP columnlist conslist_opt RP(X). {
  __ending_create_table_paren(pParse,&X,0);
}
create_table_args ::= AS select(S). {
  __ending_create_table_paren(pParse,0,S);
  __select_delete(S);
}
columnlist ::= columnlist COMMA column.
columnlist ::= column.

// About the only information used for a column is the name of the
// column.  The type is always just "text".  But the code will accept
// an elaborate typename.  Perhaps someday we'll do something with it.
//
column ::= columnid type carglist. 
columnid ::= nm(X).                {__add_column(pParse,&X);}

// An IDENTIFIER can be a generic identifier, or one of several
// keywords.  Any non-standard keyword can also be an identifier.
//
%type id {token_t}
id(A) ::= ID(X).         {A = X;}

// The following directive causes tokens ABORT, AFTER, ASC, etc. to
// fallback to ID if they will not parse as their original value.
// This obviates the need for the "id" nonterminal.
//
%fallback ID
  ABORT AFTER ASC ATTACH BEFORE BEGIN CASCADE CLUSTER CONFLICT
  COPY DATABASE DEFERRED DELIMITERS DESC DETACH EACH END EXPLAIN FAIL FOR
  GLOB IGNORE IMMEDIATE INITIALLY INSTEAD LIKE MATCH KEY
  OF OFFSET PRAGMA RAISE REPLACE RESTRICT ROW STATEMENT
  TEMP TRIGGER VACUUM VIEW.

// And "ids" is an identifer-or-string.
//
%type ids {token_t}
ids(A) ::= ID(X).        {A = X;}
ids(A) ::= STRING(X).    {A = X;}

// The name of a column or table can be any of the following:
//
%type nm {token_t}
nm(A) ::= ID(X).         {A = X;}
nm(A) ::= STRING(X).     {A = X;}
nm(A) ::= JOIN_KW(X).    {A = X;}

type ::= .
type ::= typename(X).                    {__add_column_type(pParse,&X,&X);}
type ::= typename(X) LP signed RP(Y).    {__add_column_type(pParse,&X,&Y);}
type ::= typename(X) LP signed COMMA signed RP(Y).
                                         {__add_column_type(pParse,&X,&Y);}
%type typename {token_t}
typename(A) ::= ids(X).           {A = X;}
typename(A) ::= typename(X) ids.  {A = X;}
%type signed {int}
signed(A) ::= INTEGER(X).         { A = atoi(X.z); }
signed(A) ::= PLUS INTEGER(X).    { A = atoi(X.z); }
signed(A) ::= MINUS INTEGER(X).   { A = -atoi(X.z); }
carglist ::= carglist carg.
carglist ::= .
carg ::= CONSTRAINT nm ccons.
carg ::= ccons.
carg ::= DEFAULT STRING(X).          {__add_default_value(pParse,&X,0);}
carg ::= DEFAULT ID(X).              {__add_default_value(pParse,&X,0);}
carg ::= DEFAULT INTEGER(X).         {__add_default_value(pParse,&X,0);}
carg ::= DEFAULT PLUS INTEGER(X).    {__add_default_value(pParse,&X,0);}
carg ::= DEFAULT MINUS INTEGER(X).   {__add_default_value(pParse,&X,1);}
carg ::= DEFAULT FLOAT(X).           {__add_default_value(pParse,&X,0);}
carg ::= DEFAULT PLUS FLOAT(X).      {__add_default_value(pParse,&X,0);}
carg ::= DEFAULT MINUS FLOAT(X).     {__add_default_value(pParse,&X,1);}
carg ::= DEFAULT NULL. 

// In addition to the type name, we also care about the primary key and
// UNIQUE constraints.
//
ccons ::= NULL onconf.
ccons ::= NOT NULL onconf(R).               {__add_not_null(pParse, R);}
ccons ::= PRIMARY KEY sortorder onconf(R).  {__add_primary_key(pParse,0,R);}
ccons ::= UNIQUE onconf(R).           {__create_index(pParse,0,0,0,R,0,0);}
ccons ::= CHECK LP expr RP onconf.
ccons ::= REFERENCES nm(T) idxlist_opt(TA) refargs(R).
                                {__create_foreign_key(pParse,0,&T,TA,R);}
ccons ::= defer_subclause(D).   {__defer_foreign_key(pParse,D);}
ccons ::= COLLATE id(C).  {
   __add_collate_type(pParse, __collate_type(C.z, C.n));
}

// The next group of rules parses the arguments to a REFERENCES clause
// that determine if the referential integrity checking is deferred or
// or immediate and which determine what action to take if a ref-integ
// check fails.
//
%type refargs {int}
refargs(A) ::= .                     { A = OE_Restrict * 0x010101; }
refargs(A) ::= refargs(X) refarg(Y). { A = (X & Y.mask) | Y.value; }
%type refarg {struct {int value; int mask;}}
refarg(A) ::= MATCH nm.              { A.value = 0;     A.mask = 0x000000; }
refarg(A) ::= ON DELETE refact(X).   { A.value = X;     A.mask = 0x0000ff; }
refarg(A) ::= ON UPDATE refact(X).   { A.value = X<<8;  A.mask = 0x00ff00; }
refarg(A) ::= ON INSERT refact(X).   { A.value = X<<16; A.mask = 0xff0000; }
%type refact {int}
refact(A) ::= SET NULL.              { A = OE_SetNull; }
refact(A) ::= SET DEFAULT.           { A = OE_SetDflt; }
refact(A) ::= CASCADE.               { A = OE_Cascade; }
refact(A) ::= RESTRICT.              { A = OE_Restrict; }
%type defer_subclause {int}
defer_subclause(A) ::= NOT DEFERRABLE init_deferred_pred_opt(X).  {A = X;}
defer_subclause(A) ::= DEFERRABLE init_deferred_pred_opt(X).      {A = X;}
%type init_deferred_pred_opt {int}
init_deferred_pred_opt(A) ::= .                       {A = 0;}
init_deferred_pred_opt(A) ::= INITIALLY DEFERRED.     {A = 1;}
init_deferred_pred_opt(A) ::= INITIALLY IMMEDIATE.    {A = 0;}

// For the time being, the only constraint we care about is the primary
// key and UNIQUE.  Both create indices.
//
conslist_opt ::= .
conslist_opt ::= COMMA conslist.
conslist ::= conslist COMMA tcons.
conslist ::= conslist tcons.
conslist ::= tcons.
tcons ::= CONSTRAINT nm.
tcons ::= PRIMARY KEY LP idxlist(X) RP onconf(R).
                                             {__add_primary_key(pParse,X,R);}
tcons ::= UNIQUE LP idxlist(X) RP onconf(R).
                                       {__create_index(pParse,0,0,X,R,0,0);}
tcons ::= CHECK expr onconf.
tcons ::= FOREIGN KEY LP idxlist(FA) RP
          REFERENCES nm(T) idxlist_opt(TA) refargs(R) defer_subclause_opt(D). {
    __create_foreign_key(pParse, FA, &T, TA, R);
    __defer_foreign_key(pParse, D);
}
%type defer_subclause_opt {int}
defer_subclause_opt(A) ::= .                    {A = 0;}
defer_subclause_opt(A) ::= defer_subclause(X).  {A = X;}

// The following is a non-standard extension that allows us to declare the
// default behavior when there is a constraint conflict.
//
%type onconf {int}
%type orconf {int}
%type resolvetype {int}
onconf(A) ::= .                              { A = OE_Default; }
onconf(A) ::= ON CONFLICT resolvetype(X).    { A = X; }
orconf(A) ::= .                              { A = OE_Default; }
orconf(A) ::= OR resolvetype(X).             { A = X; }
resolvetype(A) ::= ROLLBACK.                 { A = OE_Rollback; }
resolvetype(A) ::= ABORT.                    { A = OE_Abort; }
resolvetype(A) ::= FAIL.                     { A = OE_Fail; }
resolvetype(A) ::= IGNORE.                   { A = OE_Ignore; }
resolvetype(A) ::= REPLACE.                  { A = OE_Replace; }

////////////////////////// The DROP TABLE /////////////////////////////////////
//
cmd ::= DROP TABLE nm(X).          {__drop_table(pParse,&X,0);}

///////////////////// The CREATE VIEW statement /////////////////////////////
//
cmd ::= CREATE(X) temp(T) VIEW nm(Y) AS select(S). {
  __create_view(pParse, &X, &Y, S, T);
}
cmd ::= DROP VIEW nm(X). {
  __drop_table(pParse, &X, 1);
}

//////////////////////// The SELECT statement /////////////////////////////////
//
cmd ::= select(X).  {
  __select(pParse, X, SRT_Callback, 0, 0, 0, 0);
  __select_delete(X);
}

%type select {select_t*}
%destructor select {__select_delete($$);}
%type oneselect {select_t*}
%destructor oneselect {__select_delete($$);}

select(A) ::= oneselect(X).                      {A = X;}
select(A) ::= select(X) multiselect_op(Y) oneselect(Z).  {
  if( Z ){
    Z->op = Y;
    Z->pPrior = X;
  }
  A = Z;
}
%type multiselect_op {int}
multiselect_op(A) ::= UNION.      {A = TK_UNION;}
multiselect_op(A) ::= UNION ALL.  {A = TK_ALL;}
multiselect_op(A) ::= INTERSECT.  {A = TK_INTERSECT;}
multiselect_op(A) ::= EXCEPT.     {A = TK_EXCEPT;}
oneselect(A) ::= SELECT distinct(D) selcollist(W) from(X) where_opt(Y)
                 groupby_opt(P) having_opt(Q) orderby_opt(Z) limit_opt(L). {
  A = __select_new(W,X,Y,P,Q,Z,D,L.limit,L.offset);
}

// The "distinct" nonterminal is true (1) if the DISTINCT keyword is
// present and false (0) if it is not.
//
%type distinct {int}
distinct(A) ::= DISTINCT.   {A = 1;}
distinct(A) ::= ALL.        {A = 0;}
distinct(A) ::= .           {A = 0;}

// selcollist is a list of expressions that are to become the return
// values of the SELECT statement.  The "*" in statements like
// "SELECT * FROM ..." is encoded as a special expression with an
// opcode of TK_ALL.
//
%type selcollist {expr_list_t*}
%destructor selcollist {__expr_list_delete($$);}
%type sclp {expr_list_t*}
%destructor sclp {__expr_list_delete($$);}
sclp(A) ::= selcollist(X) COMMA.             {A = X;}
sclp(A) ::= .                                {A = 0;}
selcollist(A) ::= sclp(P) expr(X) as(Y).     {
   A = __expr_list_append(P,X,Y.n?&Y:0);
}
selcollist(A) ::= sclp(P) STAR. {
  A = __expr_list_append(P, __expr(TK_ALL, 0, 0, 0), 0);
}
selcollist(A) ::= sclp(P) nm(X) DOT STAR. {
  expr_t *pRight = __expr(TK_ALL, 0, 0, 0);
  expr_t *pLeft = __expr(TK_ID, 0, 0, &X);
  A = __expr_list_append(P, __expr(TK_DOT, pLeft, pRight, 0), 0);
}

// An option "AS <id>" phrase that can follow one of the expressions that
// define the result set, or one of the tables in the FROM clause.
//
%type as {token_t}
as(X) ::= AS nm(Y).    { X = Y; }
as(X) ::= ids(Y).      { X = Y; }
as(X) ::= .            { X.n = 0; }


%type seltablist {src_list_t*}
%destructor seltablist {__src_list_delete($$);}
%type stl_prefix {src_list_t*}
%destructor stl_prefix {__src_list_delete($$);}
%type from {src_list_t*}
%destructor from {__src_list_delete($$);}

// A complete FROM clause.
//
	from(A) ::= .                         {__dbsql_calloc(pParse->db, 1,
							 sizeof(*A), &(A));}
from(A) ::= FROM seltablist(X).               {A = X;}

// "seltablist" is a "select_t Table List" - the content of the FROM clause
// in a SELECT statement.  "stl_prefix" is a prefix of this list.
//
stl_prefix(A) ::= seltablist(X) joinop(Y).    {
   A = X;
   if( A && A->nSrc>0 ) A->a[A->nSrc-1].jointype = Y;
}
stl_prefix(A) ::= .                           {A = 0;}
seltablist(A) ::= stl_prefix(X) nm(Y) dbnm(D) as(Z) on_opt(N) using_opt(U). {
  A = __src_list_append(X,&Y,&D);
  if( Z.n ) __src_list_add_alias(A,&Z);
  if( N ){
    if( A && A->nSrc>1 ){ A->a[A->nSrc-2].pOn = N; }
    else { __expr_delete(N); }
  }
  if( U ){
    if( A && A->nSrc>1 ){ A->a[A->nSrc-2].pUsing = U; }
    else { __id_list_delete(U); }
  }
}
seltablist(A) ::= stl_prefix(X) LP seltablist_paren(S) RP
                  as(Z) on_opt(N) using_opt(U). {
  A = __src_list_append(X,0,0);
  A->a[A->nSrc-1].pSelect = S;
  if( Z.n ) __src_list_add_alias(A,&Z);
  if( N ){
    if( A && A->nSrc>1 ){ A->a[A->nSrc-2].pOn = N; }
    else { __expr_delete(N); }
  }
  if( U ){
    if( A && A->nSrc>1 ){ A->a[A->nSrc-2].pUsing = U; }
    else { __id_list_delete(U); }
  }
}

// A seltablist_paren nonterminal represents anything in a FROM that
// is contained inside parentheses.  This can be either a subquery or
// a grouping of table and subqueries.
//
%type seltablist_paren {select_t*}
%destructor seltablist_paren {__select_delete($$);}
seltablist_paren(A) ::= select(S).      {A = S;}
seltablist_paren(A) ::= seltablist(F).  {
   A = __select_new(0,F,0,0,0,0,0,-1,0);
}

%type dbnm {token_t}
dbnm(A) ::= .          {A.z=0; A.n=0;}
dbnm(A) ::= DOT nm(X). {A = X;}

%type joinop {int}
%type joinop2 {int}
joinop(X) ::= COMMA.                   { X = JT_INNER; }
joinop(X) ::= JOIN.                    { X = JT_INNER; }
joinop(X) ::= JOIN_KW(A) JOIN.         { X = __join_type(pParse,&A,0,0); }
joinop(X) ::= JOIN_KW(A) nm(B) JOIN.   { X = __join_type(pParse,&A,&B,0); }
joinop(X) ::= JOIN_KW(A) nm(B) nm(C) JOIN.
                                       { X = __join_type(pParse,&A,&B,&C); }

%type on_opt {expr_t*}
%destructor on_opt {__expr_delete($$);}
on_opt(N) ::= ON expr(E).   {N = E;}
on_opt(N) ::= .             {N = 0;}

%type using_opt {id_list_t*}
%destructor using_opt {__id_list_delete($$);}
using_opt(U) ::= USING LP idxlist(L) RP.  {U = L;}
using_opt(U) ::= .                        {U = 0;}


%type orderby_opt {expr_list_t*}
%destructor orderby_opt {__expr_list_delete($$);}
%type sortlist {expr_list_t*}
%destructor sortlist {__expr_list_delete($$);}
%type sortitem {expr_t*}
%destructor sortitem {__expr_delete($$);}

orderby_opt(A) ::= .                          {A = 0;}
orderby_opt(A) ::= ORDER BY sortlist(X).      {A = X;}
sortlist(A) ::= sortlist(X) COMMA sortitem(Y) collate(C) sortorder(Z). {
  A = __expr_list_append(X,Y,0);
  if( A ) A->a[A->nExpr-1].sortOrder = C+Z;
}
sortlist(A) ::= sortitem(Y) collate(C) sortorder(Z). {
  A = __expr_list_append(0,Y,0);
  if( A ) A->a[0].sortOrder = C+Z;
}
sortitem(A) ::= expr(X).   {A = X;}

%type sortorder {int}
%type collate {int}

sortorder(A) ::= ASC.           {A = DBSQL_SO_ASC;}
sortorder(A) ::= DESC.          {A = DBSQL_SO_DESC;}
sortorder(A) ::= .              {A = DBSQL_SO_ASC;}
collate(C) ::= .                {C = DBSQL_SO_UNK;}
collate(C) ::= COLLATE id(X).   {C = __collate_type(X.z, X.n);}

%type groupby_opt {expr_list_t*}
%destructor groupby_opt {__expr_list_delete($$);}
groupby_opt(A) ::= .                      {A = 0;}
groupby_opt(A) ::= GROUP BY exprlist(X).  {A = X;}

%type having_opt {expr_t*}
%destructor having_opt {__expr_delete($$);}
having_opt(A) ::= .                {A = 0;}
having_opt(A) ::= HAVING expr(X).  {A = X;}

%type limit_opt {struct LimitVal}
limit_opt(A) ::= .                     {A.limit = -1; A.offset = 0;}
limit_opt(A) ::= LIMIT signed(X).      {A.limit = X; A.offset = 0;}
limit_opt(A) ::= LIMIT signed(X) OFFSET signed(Y). 
                                       {A.limit = X; A.offset = Y;}
limit_opt(A) ::= LIMIT signed(X) COMMA signed(Y). 
                                       {A.limit = Y; A.offset = X;}

/////////////////////////// The DELETE statement /////////////////////////////
//
cmd ::= DELETE FROM nm(X) dbnm(D) where_opt(Y). {
   __delete_from(pParse, __src_list_append(0,&X,&D), Y);
}

%type where_opt {expr_t*}
%destructor where_opt {__expr_delete($$);}

where_opt(A) ::= .                    {A = 0;}
where_opt(A) ::= WHERE expr(X).       {A = X;}

%type setlist {expr_list_t*}
%destructor setlist {__expr_list_delete($$);}

////////////////////////// The UPDATE command ////////////////////////////////
//
cmd ::= UPDATE orconf(R) nm(X) dbnm(D) SET setlist(Y) where_opt(Z).
    {__update(pParse,__src_list_append(0,&X,&D),Y,Z,R);}

setlist(A) ::= setlist(Z) COMMA nm(X) EQ expr(Y).
    {A = __expr_list_append(Z,Y,&X);}
setlist(A) ::= nm(X) EQ expr(Y).   {A = __expr_list_append(0,Y,&X);}

////////////////////////// The INSERT command /////////////////////////////////
//
cmd ::= insert_cmd(R) INTO nm(X) dbnm(D) inscollist_opt(F) 
        VALUES LP itemlist(Y) RP.
            {__insert(pParse, __src_list_append(0,&X,&D), Y, 0, F, R);}
cmd ::= insert_cmd(R) INTO nm(X) dbnm(D) inscollist_opt(F) select(S).
            {__insert(pParse, __src_list_append(0,&X,&D), 0, S, F, R);}

%type insert_cmd {int}
insert_cmd(A) ::= INSERT orconf(R).   {A = R;}
insert_cmd(A) ::= REPLACE.            {A = OE_Replace;}


%type itemlist {expr_list_t*}
%destructor itemlist {__expr_list_delete($$);}

itemlist(A) ::= itemlist(X) COMMA expr(Y).  {A = __expr_list_append(X,Y,0);}
itemlist(A) ::= expr(X).                    {A = __expr_list_append(0,X,0);}

%type inscollist_opt {id_list_t*}
%destructor inscollist_opt {__id_list_delete($$);}
%type inscollist {id_list_t*}
%destructor inscollist {__id_list_delete($$);}

inscollist_opt(A) ::= .                       {A = 0;}
inscollist_opt(A) ::= LP inscollist(X) RP.    {A = X;}
inscollist(A) ::= inscollist(X) COMMA nm(Y).  {A = __id_list_append(X,&Y);}
inscollist(A) ::= nm(Y).                      {A = __id_list_append(0,&Y);}

/////////////////////////// Expression Processing /////////////////////////////
//
%left OR.
%left AND.
%right NOT.
%left EQ NE ISNULL NOTNULL IS LIKE GLOB BETWEEN IN.
%left GT GE LT LE.
%left BITAND BITOR LSHIFT RSHIFT.
%left PLUS MINUS.
%left STAR SLASH REM.
%left CONCAT.
%right UMINUS UPLUS BITNOT.

%type expr {expr_t*}
%destructor expr {__expr_delete($$);}

expr(A) ::= LP(B) expr(X) RP(E). {A = X; __expr_span(A,&B,&E); }
expr(A) ::= NULL(X).             {A = __expr(TK_NULL, 0, 0, &X);}
expr(A) ::= ID(X).               {A = __expr(TK_ID, 0, 0, &X);}
expr(A) ::= JOIN_KW(X).          {A = __expr(TK_ID, 0, 0, &X);}
expr(A) ::= nm(X) DOT nm(Y). {
  expr_t *temp1 = __expr(TK_ID, 0, 0, &X);
  expr_t *temp2 = __expr(TK_ID, 0, 0, &Y);
  A = __expr(TK_DOT, temp1, temp2, 0);
}
expr(A) ::= nm(X) DOT nm(Y) DOT nm(Z). {
  expr_t *temp1 = __expr(TK_ID, 0, 0, &X);
  expr_t *temp2 = __expr(TK_ID, 0, 0, &Y);
  expr_t *temp3 = __expr(TK_ID, 0, 0, &Z);
  expr_t *temp4 = __expr(TK_DOT, temp2, temp3, 0);
  A = __expr(TK_DOT, temp1, temp4, 0);
}
expr(A) ::= INTEGER(X).      {A = __expr(TK_INTEGER, 0, 0, &X);}
expr(A) ::= FLOAT(X).        {A = __expr(TK_FLOAT, 0, 0, &X);}
expr(A) ::= STRING(X).       {A = __expr(TK_STRING, 0, 0, &X);}
expr(A) ::= VARIABLE(X).     {
  A = __expr(TK_VARIABLE, 0, 0, &X);
  if( A ) A->iTable = ++pParse->nVar;
}
expr(A) ::= ID(X) LP exprlist(Y) RP(E). {
  A = __expr_function(Y, &X);
  __expr_span(A,&X,&E);
}
expr(A) ::= ID(X) LP STAR RP(E). {
  A = __expr_function(0, &X);
  __expr_span(A,&X,&E);
}
expr(A) ::= expr(X) AND expr(Y).   {A = __expr(TK_AND, X, Y, 0);}
expr(A) ::= expr(X) OR expr(Y).    {A = __expr(TK_OR, X, Y, 0);}
expr(A) ::= expr(X) LT expr(Y).    {A = __expr(TK_LT, X, Y, 0);}
expr(A) ::= expr(X) GT expr(Y).    {A = __expr(TK_GT, X, Y, 0);}
expr(A) ::= expr(X) LE expr(Y).    {A = __expr(TK_LE, X, Y, 0);}
expr(A) ::= expr(X) GE expr(Y).    {A = __expr(TK_GE, X, Y, 0);}
expr(A) ::= expr(X) NE expr(Y).    {A = __expr(TK_NE, X, Y, 0);}
expr(A) ::= expr(X) EQ expr(Y).    {A = __expr(TK_EQ, X, Y, 0);}
expr(A) ::= expr(X) BITAND expr(Y). {A = __expr(TK_BITAND, X, Y, 0);}
expr(A) ::= expr(X) BITOR expr(Y).  {A = __expr(TK_BITOR, X, Y, 0);}
expr(A) ::= expr(X) LSHIFT expr(Y). {A = __expr(TK_LSHIFT, X, Y, 0);}
expr(A) ::= expr(X) RSHIFT expr(Y). {A = __expr(TK_RSHIFT, X, Y, 0);}
expr(A) ::= expr(X) likeop(OP) expr(Y).  [LIKE]  {
  expr_list_t *pList = __expr_list_append(0, Y, 0);
  pList = __expr_list_append(pList, X, 0);
  A = __expr_function(pList, 0);
  if( A ) A->op = OP;
  __expr_span(A, &X->span, &Y->span);
}
expr(A) ::= expr(X) NOT likeop(OP) expr(Y). [LIKE] {
  expr_list_t *pList = __expr_list_append(0, Y, 0);
  pList = __expr_list_append(pList, X, 0);
  A = __expr_function(pList, 0);
  if( A ) A->op = OP;
  A = __expr(TK_NOT, A, 0, 0);
  __expr_span(A,&X->span,&Y->span);
}
%type likeop {int}
likeop(A) ::= LIKE. {A = TK_LIKE;}
likeop(A) ::= GLOB. {A = TK_GLOB;}
expr(A) ::= expr(X) PLUS expr(Y).  {A = __expr(TK_PLUS, X, Y, 0);}
expr(A) ::= expr(X) MINUS expr(Y). {A = __expr(TK_MINUS, X, Y, 0);}
expr(A) ::= expr(X) STAR expr(Y).  {A = __expr(TK_STAR, X, Y, 0);}
expr(A) ::= expr(X) SLASH expr(Y). {A = __expr(TK_SLASH, X, Y, 0);}
expr(A) ::= expr(X) REM expr(Y).   {A = __expr(TK_REM, X, Y, 0);}
expr(A) ::= expr(X) CONCAT expr(Y). {A = __expr(TK_CONCAT, X, Y, 0);}
expr(A) ::= expr(X) ISNULL(E). {
  A = __expr(TK_ISNULL, X, 0, 0);
  __expr_span(A,&X->span,&E);
}
expr(A) ::= expr(X) IS NULL(E). {
  A = __expr(TK_ISNULL, X, 0, 0);
  __expr_span(A,&X->span,&E);
}
expr(A) ::= expr(X) NOTNULL(E). {
  A = __expr(TK_NOTNULL, X, 0, 0);
  __expr_span(A,&X->span,&E);
}
expr(A) ::= expr(X) NOT NULL(E). {
  A = __expr(TK_NOTNULL, X, 0, 0);
  __expr_span(A,&X->span,&E);
}
expr(A) ::= expr(X) IS NOT NULL(E). {
  A = __expr(TK_NOTNULL, X, 0, 0);
  __expr_span(A,&X->span,&E);
}
expr(A) ::= NOT(B) expr(X). {
  A = __expr(TK_NOT, X, 0, 0);
  __expr_span(A,&B,&X->span);
}
expr(A) ::= BITNOT(B) expr(X). {
  A = __expr(TK_BITNOT, X, 0, 0);
  __expr_span(A,&B,&X->span);
}
expr(A) ::= MINUS(B) expr(X). [UMINUS] {
  A = __expr(TK_UMINUS, X, 0, 0);
  __expr_span(A,&B,&X->span);
}
expr(A) ::= PLUS(B) expr(X). [UPLUS] {
  A = __expr(TK_UPLUS, X, 0, 0);
  __expr_span(A,&B,&X->span);
}
expr(A) ::= LP(B) select(X) RP(E). {
  A = __expr(TK_SELECT, 0, 0, 0);
  if( A ) A->pSelect = X;
  __expr_span(A,&B,&E);
}
expr(A) ::= expr(W) BETWEEN expr(X) AND expr(Y). {
  expr_list_t *pList = __expr_list_append(0, X, 0);
  pList = __expr_list_append(pList, Y, 0);
  A = __expr(TK_BETWEEN, W, 0, 0);
  if( A ) A->pList = pList;
  __expr_span(A,&W->span,&Y->span);
}
expr(A) ::= expr(W) NOT BETWEEN expr(X) AND expr(Y). {
  expr_list_t *pList = __expr_list_append(0, X, 0);
  pList = __expr_list_append(pList, Y, 0);
  A = __expr(TK_BETWEEN, W, 0, 0);
  if( A ) A->pList = pList;
  A = __expr(TK_NOT, A, 0, 0);
  __expr_span(A,&W->span,&Y->span);
}
expr(A) ::= expr(X) IN LP exprlist(Y) RP(E).  {
  A = __expr(TK_IN, X, 0, 0);
  if( A ) A->pList = Y;
  __expr_span(A,&X->span,&E);
}
expr(A) ::= expr(X) IN LP select(Y) RP(E).  {
  A = __expr(TK_IN, X, 0, 0);
  if( A ) A->pSelect = Y;
  __expr_span(A,&X->span,&E);
}
expr(A) ::= expr(X) NOT IN LP exprlist(Y) RP(E).  {
  A = __expr(TK_IN, X, 0, 0);
  if( A ) A->pList = Y;
  A = __expr(TK_NOT, A, 0, 0);
  __expr_span(A,&X->span,&E);
}
expr(A) ::= expr(X) NOT IN LP select(Y) RP(E).  {
  A = __expr(TK_IN, X, 0, 0);
  if( A ) A->pSelect = Y;
  A = __expr(TK_NOT, A, 0, 0);
  __expr_span(A,&X->span,&E);
}
expr(A) ::= expr(X) IN nm(Y) dbnm(D). {
  src_list_t *pSrc = __src_list_append(0, &Y, &D);
  A = __expr(TK_IN, X, 0, 0);
  if( A ) A->pSelect = __select_new(0,pSrc,0,0,0,0,0,-1,0);
  __expr_span(A,&X->span,D.z?&D:&Y);
}
expr(A) ::= expr(X) NOT IN nm(Y) dbnm(D). {
  src_list_t *pSrc = __src_list_append(0, &Y, &D);
  A = __expr(TK_IN, X, 0, 0);
  if( A ) A->pSelect = __select_new(0,pSrc,0,0,0,0,0,-1,0);
  A = __expr(TK_NOT, A, 0, 0);
  __expr_span(A,&X->span,D.z?&D:&Y);
}


/* CASE expressions */
expr(A) ::= CASE(C) case_operand(X) case_exprlist(Y) case_else(Z) END(E). {
  A = __expr(TK_CASE, X, Z, 0);
  if( A ) A->pList = Y;
  __expr_span(A, &C, &E);
}
%type case_exprlist {expr_list_t*}
%destructor case_exprlist {__expr_list_delete($$);}
case_exprlist(A) ::= case_exprlist(X) WHEN expr(Y) THEN expr(Z). {
  A = __expr_list_append(X, Y, 0);
  A = __expr_list_append(A, Z, 0);
}
case_exprlist(A) ::= WHEN expr(Y) THEN expr(Z). {
  A = __expr_list_append(0, Y, 0);
  A = __expr_list_append(A, Z, 0);
}
%type case_else {expr_t*}
case_else(A) ::=  ELSE expr(X).         {A = X;}
case_else(A) ::=  .                     {A = 0;} 
%type case_operand {expr_t*}
case_operand(A) ::= expr(X).            {A = X;} 
case_operand(A) ::= .                   {A = 0;} 

%type exprlist {expr_list_t*}
%destructor exprlist {__expr_list_delete($$);}
%type expritem {expr_t*}
%destructor expritem {__expr_delete($$);}

exprlist(A) ::= exprlist(X) COMMA expritem(Y). 
   {A = __expr_list_append(X,Y,0);}
exprlist(A) ::= expritem(X).            {A = __expr_list_append(0,X,0);}
expritem(A) ::= expr(X).                {A = X;}
expritem(A) ::= .                       {A = 0;}

///////////////////////////// The CREATE INDEX command ///////////////////////
//
cmd ::= CREATE(S) uniqueflag(U) INDEX nm(X)
        ON nm(Y) dbnm(D) LP idxlist(Z) RP(E) onconf(R). {
  src_list_t *pSrc = __src_list_append(0, &Y, &D);
  if( U!=OE_None ) U = R;
  if( U==OE_Default) U = OE_Abort;
  __create_index(pParse, &X, pSrc, Z, U, &S, &E);
}

%type uniqueflag {int}
uniqueflag(A) ::= UNIQUE.  { A = OE_Abort; }
uniqueflag(A) ::= .        { A = OE_None; }

%type idxlist {id_list_t*}
%destructor idxlist {__id_list_delete($$);}
%type idxlist_opt {id_list_t*}
%destructor idxlist_opt {__id_list_delete($$);}
%type idxitem {token_t}

idxlist_opt(A) ::= .                         {A = 0;}
idxlist_opt(A) ::= LP idxlist(X) RP.         {A = X;}
idxlist(A) ::= idxlist(X) COMMA idxitem(Y).  {A = __id_list_append(X,&Y);}
idxlist(A) ::= idxitem(Y).                   {A = __id_list_append(0,&Y);}
idxitem(A) ::= nm(X) sortorder.              {A = X;}

///////////////////////////// The DROP INDEX command /////////////////////////
//

cmd ::= DROP INDEX nm(X) dbnm(Y).   {
  __drop_index(pParse, __src_list_append(0,&X,&Y));
}


///////////////////////////// The COPY command ///////////////////////////////
//
cmd ::= COPY orconf(R) nm(X) dbnm(D) FROM nm(Y) USING DELIMITERS STRING(Z).
    {__copy(pParse,__src_list_append(0,&X,&D),&Y,&Z,R);}
cmd ::= COPY orconf(R) nm(X) dbnm(D) FROM nm(Y).
    {__copy(pParse,__src_list_append(0,&X,&D),&Y,0,R);}

///////////////////////////// The VACUUM command /////////////////////////////
//
cmd ::= VACUUM.                {__vacuum(pParse,0);}
cmd ::= VACUUM nm(X).         {__vacuum(pParse,&X);}

///////////////////////////// The PRAGMA command /////////////////////////////
//
cmd ::= PRAGMA ids(X) EQ nm(Y).         {__pragma(pParse,&X,&Y,0);}
cmd ::= PRAGMA ids(X) EQ ON(Y).          {__pragma(pParse,&X,&Y,0);}
cmd ::= PRAGMA ids(X) EQ plus_num(Y).    {__pragma(pParse,&X,&Y,0);}
cmd ::= PRAGMA ids(X) EQ minus_num(Y).   {__pragma(pParse,&X,&Y,1);}
cmd ::= PRAGMA ids(X) LP nm(Y) RP.      {__pragma(pParse,&X,&Y,0);}
cmd ::= PRAGMA ids(X).                   {__pragma(pParse,&X,&X,0);}
plus_num(A) ::= plus_opt number(X).   {A = X;}
minus_num(A) ::= MINUS number(X).     {A = X;}
number(A) ::= INTEGER(X).  {A = X;}
number(A) ::= FLOAT(X).    {A = X;}
plus_opt ::= PLUS.
plus_opt ::= .

//////////////////////////// The CREATE TRIGGER command /////////////////////

cmd ::= CREATE(A) trigger_decl BEGIN trigger_cmd_list(S) END(Z). {
  token_t all;
  all.z = A.z;
  all.n = (Z.z - A.z) + Z.n;
  __finish_trigger(pParse, S, &all);
}

trigger_decl ::= temp(T) TRIGGER nm(B) trigger_time(C) trigger_event(D)
                 ON nm(E) dbnm(DB) foreach_clause(F) when_clause(G). {
  src_list_t *pTab = __src_list_append(0, &E, &DB);
  __begin_trigger(pParse, &B, C, D.a, D.b, pTab, F, G, T);
}

%type trigger_time  {int}
trigger_time(A) ::= BEFORE.      { A = TK_BEFORE; }
trigger_time(A) ::= AFTER.       { A = TK_AFTER;  }
trigger_time(A) ::= INSTEAD OF.  { A = TK_INSTEAD;}
trigger_time(A) ::= .            { A = TK_BEFORE; }

%type trigger_event {struct TrigEvent}
%destructor trigger_event {__id_list_delete($$.b);}
trigger_event(A) ::= DELETE. { A.a = TK_DELETE; A.b = 0; }
trigger_event(A) ::= INSERT. { A.a = TK_INSERT; A.b = 0; }
trigger_event(A) ::= UPDATE. { A.a = TK_UPDATE; A.b = 0;}
trigger_event(A) ::= UPDATE OF inscollist(X). {A.a = TK_UPDATE; A.b = X; }

%type foreach_clause {int}
foreach_clause(A) ::= .                   { A = TK_ROW; }
foreach_clause(A) ::= FOR EACH ROW.       { A = TK_ROW; }
foreach_clause(A) ::= FOR EACH STATEMENT. { A = TK_STATEMENT; }

%type when_clause {expr_t *}
when_clause(A) ::= .             { A = 0; }
when_clause(A) ::= WHEN expr(X). { A = X; }

%type trigger_cmd_list {trigger_step_t *}
%destructor trigger_cmd_list {__vdbe_delete_trigger_step($$);}
trigger_cmd_list(A) ::= trigger_cmd(X) SEMI trigger_cmd_list(Y). {
  X->pNext = Y;
  A = X;
}
trigger_cmd_list(A) ::= . { A = 0; }

%type trigger_cmd {trigger_step_t *}
%destructor trigger_cmd {__vdbe_delete_trigger_step($$);}
// UPDATE 
trigger_cmd(A) ::= UPDATE orconf(R) nm(X) SET setlist(Y) where_opt(Z).  
               { A = __trigger_update_step(&X, Y, Z, R); }

// INSERT
trigger_cmd(A) ::= INSERT orconf(R) INTO nm(X) inscollist_opt(F) 
  VALUES LP itemlist(Y) RP.  
{A = __trigger_insert_step(&X, F, Y, 0, R);}

trigger_cmd(A) ::= INSERT orconf(R) INTO nm(X) inscollist_opt(F) select(S).
               {A = __trigger_insert_step(&X, F, 0, S, R);}

// DELETE
trigger_cmd(A) ::= DELETE FROM nm(X) where_opt(Y).
               {A = __trigger_delete_step(&X, Y);}

// SELECT
trigger_cmd(A) ::= select(X).  {A = __trigger_select_step(X); }

// The special RAISE expression that may occur in trigger programs
expr(A) ::= RAISE(X) LP IGNORE RP(Y).  {
  A = __expr(TK_RAISE, 0, 0, 0); 
  A->iColumn = OE_Ignore;
  __expr_span(A, &X, &Y);
}
expr(A) ::= RAISE(X) LP ROLLBACK COMMA nm(Z) RP(Y).  {
  A = __expr(TK_RAISE, 0, 0, &Z); 
  A->iColumn = OE_Rollback;
  __expr_span(A, &X, &Y);
}
expr(A) ::= RAISE(X) LP ABORT COMMA nm(Z) RP(Y).  {
  A = __expr(TK_RAISE, 0, 0, &Z); 
  A->iColumn = OE_Abort;
  __expr_span(A, &X, &Y);
}
expr(A) ::= RAISE(X) LP FAIL COMMA nm(Z) RP(Y).  {
  A = __expr(TK_RAISE, 0, 0, &Z); 
  A->iColumn = OE_Fail;
  __expr_span(A, &X, &Y);
}

////////////////////////  DROP TRIGGER statement //////////////////////////////
cmd ::= DROP TRIGGER nm(X) dbnm(D). {
  __drop_trigger(pParse,__src_list_append(0,&X,&D));
}

//////////////////////// ATTACH DATABASE file AS name /////////////////////////
cmd ::= ATTACH database_kw_opt ids(F) AS nm(D). {
  __attach(pParse, &F, &D);
}

database_kw_opt ::= DATABASE.
database_kw_opt ::= .

//////////////////////// DETACH DATABASE name /////////////////////////////////
cmd ::= DETACH database_kw_opt nm(D). {
  __detach(pParse, &D);
}
