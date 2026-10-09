/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains routines used for analyzing expressions and
** for generating VDBE code that evaluates expressions in dbsql.
*/
#include "dbsql_int.h"

/* Forward declarations */
static void __expr_code_between(parser_t*,expr_t*,int,void(*)(parser_t*,expr_t*,int,int),int);
static int __expr_code_vector(parser_t *parser, expr_t *p, int *pi_to_free);

/*
** Return the affinity character for a single column of a table.
*/
char __table_column_affinity(const table_t *tab, int i_col){
  if( i_col<0 || NEVER(i_col>=tab->n_col) ) return DBSQL_AFF_INTEGER;
  return tab->cols[i_col].affinity;
}

/*
** Return the 'affinity' of the expression expr if any.
**
** If expr is a column, a reference to a column via an 'AS' alias,
** or a sub-select with a column as the return value, then the
** affinity of that column is returned. Otherwise, 0x00 is returned,
** indicating no affinity for the expression.
**
** i.e. the WHERE clause expressions in the following statements all
** have an affinity:
**
** CREATE TABLE t1(a);
** SELECT * FROM t1 WHERE a;
** SELECT a AS b FROM t1 WHERE b;
** SELECT * FROM t1 WHERE (select a from t1);
*/
char __expr_affinity(const expr_t *expr){
  int op;
  op = expr->op;
  while( 1 /* exit-by-break */ ){
    if( op==TK_COLUMN || (op==TK_AGG_COLUMN && expr->y.tab!=0) ){
      assert( EXPR_USE_Y_TAB(expr) );
      assert( expr->y.tab!=0 );
      return __table_column_affinity(expr->y.tab, expr->i_column);
    }
    if( op==TK_SELECT ){
      assert( EXPR_USE_X_SELECT(expr) );
      assert( expr->x.p_select!=0 );
      assert( expr->x.p_select->e_list!=0 );
      assert( expr->x.p_select->e_list->a[0].expr!=0 );
      return __expr_affinity(expr->x.p_select->e_list->a[0].expr);
    }
#ifndef DBSQL_OMIT_CAST
    if( op==TK_CAST ){
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      return __affinity_type(expr->u.token, 0);
    }
#endif
    if( op==TK_SELECT_COLUMN ){
      assert( expr->left!=0 && EXPR_USE_X_SELECT(expr->left) );
      assert( expr->i_column < expr->i_table );
      assert( expr->i_column >= 0 );
      assert( expr->i_table==expr->left->x.p_select->e_list->n_expr );
      return __expr_affinity(
          expr->left->x.p_select->e_list->a[expr->i_column].expr
      );
    }
    if( op==TK_VECTOR
     || (op==TK_FUNCTION && expr->aff_expr==DBSQL_AFF_DEFER)
    ){
      assert( EXPR_USE_X_LIST(expr) );
      return __expr_affinity(expr->x.list->a[0].expr);
    }
    if( EXPR_HAS_PROPERTY(expr, EP_Skip|EP_IfNullRow) ){
      assert( expr->op==TK_COLLATE
           || expr->op==TK_IF_NULL_ROW
           || (expr->op==TK_REGISTER && expr->op2==TK_IF_NULL_ROW) );
      expr = expr->left;
      op = expr->op;
      continue;
    }
    if( op!=TK_REGISTER ) break;
    op = expr->op2;
    if( NEVER( op==TK_REGISTER ) ) break;
  }
  return expr->aff_expr;
}

/*
** Make a guess at all the possible datatypes of the result that could
** be returned by an expression.  Return a bitmask indicating the answer:
**
**     0x01         Numeric
**     0x02         Text
**     0x04         Blob
**
** If the expression must return NULL, then 0x00 is returned.
*/
int __expr_data_type(const expr_t *expr){
  while( expr ){
    switch( expr->op ){
      case TK_COLLATE:
      case TK_IF_NULL_ROW:
      case TK_UPLUS:  {
        expr = expr->left;
        break;
      }
      case TK_NULL: {
        expr = 0;
        break;
      }
      case TK_STRING: {
        return 0x02;
      }
      case TK_BLOB: {
        return 0x04;
      }
      case TK_CONCAT: {
        return 0x06;
      }
      case TK_VARIABLE:
      case TK_AGG_FUNCTION:
      case TK_FUNCTION: {
        return 0x07;
      }
      case TK_COLUMN:
      case TK_AGG_COLUMN:
      case TK_SELECT:
      case TK_CAST:
      case TK_SELECT_COLUMN:
      case TK_VECTOR:  {
        int aff = __expr_affinity(expr);
        if( aff>=DBSQL_AFF_NUMERIC ) return 0x05;
        if( aff==DBSQL_AFF_TEXT )    return 0x06;
        return 0x07;
      }
      case TK_CASE: {
        int res = 0;
        int ii;
        expr_list_t *list = expr->x.list;
        assert( EXPR_USE_X_LIST(expr) && list!=0 );
        assert( list->n_expr > 0);
        for(ii=1; ii<list->n_expr; ii+=2){
          res |= __expr_data_type(list->a[ii].expr);
        }
        if( list->n_expr % 2 ){
          res |= __expr_data_type(list->a[list->n_expr-1].expr);
        }
        return res;
      }
      default: {
        return 0x01;
      }
    } /* End of switch(op) */
  } /* End of while(expr) */
  return 0x00;
}

/*
** Set the collating sequence for expression expr to be the collating
** sequence named by token.   Return a pointer to a new expr_t node that
** implements the COLLATE operator.
**
** If a memory allocation error occurs, that fact is recorded in parser->db
** and the expr parameter is returned unchanged.
*/
expr_t *__expr_add_collate_token(
  const parser_t *parser,     /* Parsing context */
  expr_t *expr,             /* Add the "COLLATE" clause to this expression */
  const Token *coll_name,  /* Name of collating sequence */
  int dequote              /* True to dequote coll_name */
){
  if( coll_name->n>0 ){
    expr_t *p_new = __expr_alloc(parser->db, TK_COLLATE, coll_name, dequote);
    if( p_new ){
      p_new->left = expr;
      p_new->flags |= EP_Collate|EP_Skip;
      expr = p_new;
    }
  }
  return expr;
}
expr_t *__expr_add_collate_string(
  const parser_t *parser,  /* Parsing context */
  expr_t *expr,          /* Add the "COLLATE" clause to this expression */
  const char *c        /* The collating sequence name */
){
  Token s;
  assert( c!=0 );
  __token_init(&s, (char*)c);
  return __expr_add_collate_token(parser, expr, &s, 0);
}

/*
** Skip over any TK_COLLATE operators.
*/
expr_t *__expr_skip_collate(expr_t *expr){
  while( expr && EXPR_HAS_PROPERTY(expr, EP_Skip) ){
    assert( expr->op==TK_COLLATE );
    expr = expr->left;
  }  
  return expr;
}

/*
** Skip over any TK_COLLATE operators and/or any unlikely()
** or likelihood() or likely() functions at the root of an
** expression.
*/
expr_t *__expr_skip_collate_and_likely(expr_t *expr){
  while( expr && EXPR_HAS_PROPERTY(expr, EP_Skip|EP_Unlikely) ){
    if( EXPR_HAS_PROPERTY(expr, EP_Unlikely) ){
      assert( EXPR_USE_X_LIST(expr) );
      assert( expr->x.list->n_expr>0 );
      assert( expr->op==TK_FUNCTION );
      expr = expr->x.list->a[0].expr;
    }else if( expr->op==TK_COLLATE ){
      expr = expr->left;
    }else{
      break;
    }
  }  
  return expr;
}

/*
** Return the collation sequence for the expression expr. If
** there is no defined collating sequence, return NULL.
**
** See also: __expr_nn_coll_seq()
**
** The __expr_nn_coll_seq() works the same exact that it returns the
** default collation if expr has no defined collation.
**
** The collating sequence might be determined by a COLLATE operator
** or by the presence of a column with a defined collating sequence.
** COLLATE operators take first precedence.  Left operands take
** precedence over right operands.
*/
coll_seq_t *__expr_coll_seq(parser_t *parser, const expr_t *expr){
  DBSQL *db = parser->db;
  coll_seq_t *coll = 0;
  const expr_t *p = expr;
  while( p ){
    int op = p->op;
    if( op==TK_REGISTER ) op = p->op2;
    if( (op==TK_AGG_COLUMN && p->y.tab!=0)
     || op==TK_COLUMN || op==TK_TRIGGER
    ){
      int j;
      assert( EXPR_USE_Y_TAB(p) );
      assert( p->y.tab!=0 );
      if( (j = p->i_column)>=0 ){
        const char *z_coll = __column_coll(&p->y.tab->cols[j]);
        coll = __find_coll_seq(db, ENC(db), z_coll, 0);
      }
      break;
    }
    if( op==TK_CAST || op==TK_UPLUS ){
      p = p->left;
      continue;
    }
    if( op==TK_VECTOR
     || (op==TK_FUNCTION && p->aff_expr==DBSQL_AFF_DEFER)
    ){
      assert( EXPR_USE_X_LIST(p) );
      p = p->x.list->a[0].expr;
      continue;
    }
    if( op==TK_COLLATE ){
      assert( !EXPR_HAS_PROPERTY(p, EP_IntValue) );
      coll = __get_coll_seq(parser, ENC(db), 0, p->u.token);
      break;
    }
    if( p->flags & EP_Collate ){
      if( p->left && (p->left->flags & EP_Collate)!=0 ){
        p = p->left;
      }else{
        expr_t *next  = p->right;
        /* The expr_t.x union is never used at the same time as expr_t.right */
        assert( !EXPR_USE_X_LIST(p) || p->x.list==0 || p->right==0 );
        if( EXPR_USE_X_LIST(p) && p->x.list!=0 && !db->malloc_failed ){
          int i;
          for(i=0; i<p->x.list->n_expr; i++){
            if( EXPR_HAS_PROPERTY(p->x.list->a[i].expr, EP_Collate) ){
              next = p->x.list->a[i].expr;
              break;
            }
          }
        }
        p = next;
      }
    }else{
      break;
    }
  }
  if( __check_coll_seq(parser, coll) ){
    coll = 0;
  }
  return coll;
}

/*
** Return the collation sequence for the expression expr. If
** there is no defined collating sequence, return a pointer to the
** default collation sequence.
**
** See also: __expr_coll_seq()
**
** The __expr_coll_seq() routine works the same except that it
** returns NULL if there is no defined collation.
*/
coll_seq_t *__expr_nn_coll_seq(parser_t *parser, const expr_t *expr){
  coll_seq_t *p = __expr_coll_seq(parser, expr);
  if( p==0 ) p = parser->db->dflt_coll;
  assert( p!=0 );
  return p;
}

/*
** Return TRUE if the two expressions have equivalent collating sequences.
*/
int __expr_coll_seq_match(parser_t *parser, const expr_t *e1, const expr_t *e2){
  coll_seq_t *coll1 = __expr_nn_coll_seq(parser, e1);
  coll_seq_t *coll2 = __expr_nn_coll_seq(parser, e2);
  return __str_icmp(coll1->name, coll2->name)==0;
}

/*
** expr is an operand of a comparison operator.  aff2 is the
** type affinity of the other operand.  This routine returns the
** type affinity that should be used for the comparison operator.
*/
char __compare_affinity(const expr_t *expr, char aff2){
  char aff1 = __expr_affinity(expr);
  if( aff1>DBSQL_AFF_NONE && aff2>DBSQL_AFF_NONE ){
    /* Both sides of the comparison are columns. If one has numeric
    ** affinity, use that. Otherwise use no affinity.
    */
    if( __is_numeric_affinity(aff1) || __is_numeric_affinity(aff2) ){
      return DBSQL_AFF_NUMERIC;
    }else{
      return DBSQL_AFF_BLOB;
    }
  }else{
    /* One side is a column, the other is not. Use the columns affinity. */
    assert( aff1<=DBSQL_AFF_NONE || aff2<=DBSQL_AFF_NONE );
    return (aff1<=DBSQL_AFF_NONE ? aff2 : aff1) | DBSQL_AFF_NONE;
  }
}

/*
** expr is a comparison operator.  Return the type affinity that should
** be applied to both operands prior to doing the comparison.
*/
static char __comparison_affinity(const expr_t *expr){
  char aff;
  assert( expr->op==TK_EQ || expr->op==TK_IN || expr->op==TK_LT ||
          expr->op==TK_GT || expr->op==TK_GE || expr->op==TK_LE ||
          expr->op==TK_NE || expr->op==TK_IS || expr->op==TK_ISNOT );
  assert( expr->left );
  aff = __expr_affinity(expr->left);
  if( expr->right ){
    aff = __compare_affinity(expr->right, aff);
  }else if( EXPR_USE_X_SELECT(expr) ){
    aff = __compare_affinity(expr->x.p_select->e_list->a[0].expr, aff);
  }else if( aff==0 ){
    aff = DBSQL_AFF_BLOB;
  }
  return aff;
}

/*
** expr is a comparison expression, eg. '=', '<', IN(...) etc.
** idx_affinity is the affinity of an indexed column. Return true
** if the index with affinity idx_affinity may be used to implement
** the comparison in expr.
*/
int __index_affinity_ok(const expr_t *expr, char idx_affinity){
  char aff = __comparison_affinity(expr);
  if( aff<DBSQL_AFF_TEXT ){
    return 1;
  }
  if( aff==DBSQL_AFF_TEXT ){
    return idx_affinity==DBSQL_AFF_TEXT;
  }
  return __is_numeric_affinity(idx_affinity);
}

/*
** Return the P5 value that should be used for a binary comparison
** opcode (OP_Eq, OP_Ge etc.) used to compare expr1 and expr2.
*/
static u_int8_t __binary_compare_p5(
  const expr_t *expr1,   /* Left operand */
  const expr_t *expr2,   /* Right operand */
  int jump_if_null        /* Extra flags added to P5 */
){
  u_int8_t aff = (char)__expr_affinity(expr2);
  aff = (u_int8_t)__compare_affinity(expr1, aff) | (u_int8_t)jump_if_null;
  return aff;
}

/*
** Return a pointer to the collation sequence that should be used by
** a binary comparison operator comparing left and right.
**
** If the left hand expression has a collating sequence type, then it is
** used. Otherwise the collation sequence for the right hand expression
** is used, or the default (BINARY) if neither expression has a collating
** type.
**
** Argument right (but not left) may be a null pointer. In this case,
** it is not considered.
*/
coll_seq_t *__binary_compare_coll_seq(
  parser_t *parser,
  const expr_t *left,
  const expr_t *right
){
  coll_seq_t *coll;
  assert( left );
  if( left->flags & EP_Collate ){
    coll = __expr_coll_seq(parser, left);
  }else if( right && (right->flags & EP_Collate)!=0 ){
    coll = __expr_coll_seq(parser, right);
  }else{
    coll = __expr_coll_seq(parser, left);
    if( !coll ){
      coll = __expr_coll_seq(parser, right);
    }
  }
  return coll;
}

/* Expression p is a comparison operator.  Return a collation sequence
** appropriate for the comparison operator.
**
** This is normally just a wrapper around __binary_compare_coll_seq().
** However, if the OP_Commuted flag is set, then the order of the operands
** is reversed in the __binary_compare_coll_seq() call so that the
** correct collating sequence is found.
*/
coll_seq_t *__expr_compare_coll_seq(parser_t *parser, const expr_t *p){
  if( EXPR_HAS_PROPERTY(p, EP_Commuted) ){
    return __binary_compare_coll_seq(parser, p->right, p->left);
  }else{
    return __binary_compare_coll_seq(parser, p->left, p->right);
  }
}

/*
** Generate code for a comparison operator.
*/
static int __code_compare(
  parser_t *parser,    /* The parsing (and code generating) context */
  expr_t *left,      /* The left operand */
  expr_t *right,     /* The right operand */
  int opcode,       /* The comparison opcode */
  int in1, int in2, /* Register holding operands */
  int dest,         /* Jump here if true.  */
  int jump_if_null,   /* If true, jump if either operand is NULL */
  int is_commuted    /* The comparison has been commuted */
){
  int p5;
  int addr;
  coll_seq_t *p4;

  if( parser->n_err ) return 0;
  if( is_commuted ){
    p4 = __binary_compare_coll_seq(parser, right, left);
  }else{
    p4 = __binary_compare_coll_seq(parser, left, right);
  }
  p5 = __binary_compare_p5(left, right, jump_if_null);
  addr = __vdbe_add_op4(parser->vdbe, opcode, in2, dest, in1,
                           (void*)p4, P4_COLLSEQ);
  __vdbe_change_p5(parser->vdbe, (u_int16_t)p5);
  return addr;
}

/*
** Return true if expression expr is a vector, or false otherwise.
**
** A vector is defined as any expression that results in two or more
** columns of result.  Every TK_VECTOR node is an vector because the
** parser will not generate a TK_VECTOR with fewer than two entries.
** But a TK_SELECT might be either a vector or a scalar. It is only
** considered a vector if it has two or more result columns.
*/
int __expr_is_vector(const expr_t *expr){
  return __expr_vector_size(expr)>1;
}

/*
** If the expression passed as the only argument is of type TK_VECTOR
** return the number of expressions in the vector. Or, if the expression
** is a sub-select, return the number of columns in the sub-select. For
** any other type of expression, return 1.
*/
int __expr_vector_size(const expr_t *expr){
  u_int8_t op = expr->op;
  if( op==TK_REGISTER ) op = expr->op2;
  if( op==TK_VECTOR ){
    assert( EXPR_USE_X_LIST(expr) );
    return expr->x.list->n_expr;
  }else if( op==TK_SELECT ){
    assert( EXPR_USE_X_SELECT(expr) );
    return expr->x.p_select->e_list->n_expr;
  }else{
    return 1;
  }
}

/*
** Return a pointer to a subexpression of vector that is the i-th
** column of the vector (numbered starting with 0).  The caller must
** ensure that i is within range.
**
** If vector is really a scalar (and "scalar" here includes subqueries
** that return a single column!) then return vector unmodified.
**
** vector retains ownership of the returned subexpression.
**
** If the vector is a (SELECT ...) then the expression returned is
** just the expression for the i-th term of the result set, and may
** not be ready for evaluation because the table cursor has not yet
** been positioned.
*/
expr_t *__vector_field_subexpr(expr_t *vector, int i){
  assert( i<__expr_vector_size(vector) || vector->op==TK_ERROR );
  if( __expr_is_vector(vector) ){
    assert( vector->op2==0 || vector->op==TK_REGISTER );
    if( vector->op==TK_SELECT || vector->op2==TK_SELECT ){
      assert( EXPR_USE_X_SELECT(vector) );
      return vector->x.p_select->e_list->a[i].expr;
    }else{
      assert( EXPR_USE_X_LIST(vector) );
      return vector->x.list->a[i].expr;
    }
  }
  return vector;
}

/*
** Compute and return a new expr_t object which when passed to
** __expr_code() will generate all necessary code to compute
** the i_field-th column of the vector expression vector.
**
** It is ok for vector to be a scalar (as long as i_field==0). 
** In that case, this routine works like __expr_dup().
**
** The caller owns the returned expr_t object and is responsible for
** ensuring that the returned value eventually gets freed.
**
** The caller retains ownership of vector.  If vector is a TK_SELECT,
** then the returned object will reference vector and so vector must remain
** valid for the life of the returned object.  If vector is a TK_VECTOR
** or a scalar expression, then it can be deleted as soon as this routine
** returns.
**
** A trick to cause a TK_SELECT vector to be deleted together with
** the returned expr_t object is to attach the vector to the right field
** of the returned TK_SELECT_COLUMN expr_t object.
*/
expr_t *__expr_for_vector_field(
  parser_t *parser,       /* Parsing context */
  expr_t *vector,       /* The vector.  List of expressions or a sub-SELECT */
  int i_field,          /* Which column of the vector to return */
  int n_field           /* Total number of columns in the vector */
){
  expr_t *ret;
  if( vector->op==TK_SELECT ){
    assert( EXPR_USE_X_SELECT(vector) );
    /* The TK_SELECT_COLUMN expr_t node:
    **
    ** left:           vector containing TK_SELECT.  Not deleted.
    ** right:          not used.  But recursively deleted.
    ** i_column:         index_t of a column in vector
    ** i_table:          0 or the number of columns on the LHS of an assignment
    ** left->i_table:   First in an array of register holding result, or 0
    **                  if the result is not yet computed.
    **
    ** __expr_delete() specifically skips the recursive delete of
    ** left on TK_SELECT_COLUMN nodes.  But right is followed, so vector
    ** can be attached to right to cause this node to take ownership of
    ** vector.  Typically there will be multiple TK_SELECT_COLUMN nodes
    ** with the same left pointer to the vector, but only one of them
    ** will own the vector.
    */
    ret = __p_expr(parser, TK_SELECT_COLUMN, 0, 0);
    if( ret ){
      EXPR_SET_PROPERTY(ret, EP_FullSize);
      ret->i_table = n_field;
      ret->i_column = i_field;
      ret->left = vector;
    }
  }else{
    if( vector->op==TK_VECTOR ){
      expr_t **pp_vector;
      assert( EXPR_USE_X_LIST(vector) );
      pp_vector = &vector->x.list->a[i_field].expr;
      vector = *pp_vector;
      if( IN_RENAME_OBJECT ){
        /* This must be a vector UPDATE inside a trigger */
        *pp_vector = 0;
        return vector;
      }
    }
    ret = __expr_dup(parser->db, vector, 0);
  }
  return ret;
}

/*
** If expression expr is of type TK_SELECT, generate code to evaluate
** it. Return the register in which the result is stored (or, if the
** sub-select returns more than one column, the first in an array
** of registers in which the result is stored).
**
** If expr is not a TK_SELECT expression, return 0.
*/
static int __expr_code_subselect(parser_t *parser, expr_t *expr){
  int reg = 0;
#ifndef DBSQL_OMIT_SUBQUERY
  if( expr->op==TK_SELECT ){
    reg = __code_subselect(parser, expr);
  }
#endif
  return reg;
}

/*
** Argument vector points to a vector expression - either a TK_VECTOR
** or TK_SELECT that returns more than one column. This function returns
** the register number of a register that contains the value of
** element i_field of the vector.
**
** If vector is a TK_SELECT expression, then code for it must have
** already been generated using the __expr_code_subselect() routine. In this
** case parameter reg_select should be the first in an array of registers
** containing the results of the sub-select.
**
** If vector is of type TK_VECTOR, then code for the requested field
** is generated. In this case (*reg_free) may be set to the number of
** a temporary register to be freed by the caller before returning.
**
** Before returning, output parameter (*expr) is set to point to the
** expr_t object corresponding to element i_elem of the vector.
*/
static int __expr_vector_register(
  parser_t *parser,                  /* parser_t context */
  expr_t *vector,                  /* Vector to extract element from */
  int i_field,                     /* Field to extract from vector */
  int reg_select,                  /* First in array of registers */
  expr_t **expr,                  /* OUT: Expression element */
  int *reg_free                   /* OUT: Temp register to free */
){
  u_int8_t op = vector->op;
  assert( op==TK_VECTOR || op==TK_REGISTER || op==TK_SELECT || op==TK_ERROR );
  if( op==TK_REGISTER ){
    *expr = __vector_field_subexpr(vector, i_field);
    return vector->i_table+i_field;
  }
  if( op==TK_SELECT ){
    assert( EXPR_USE_X_SELECT(vector) );
    *expr = vector->x.p_select->e_list->a[i_field].expr;
     return reg_select+i_field;
  }
  if( op==TK_VECTOR ){
    assert( EXPR_USE_X_LIST(vector) );
    *expr = vector->x.list->a[i_field].expr;
    return __expr_code_temp(parser, *expr, reg_free);
  }
  return 0;
}

/*
** Expression expr is a comparison between two vector values. Compute
** the result of the comparison (1, 0, or NULL) and write that
** result into register dest.
**
** The caller must satisfy the following preconditions:
**
**    if expr->op==TK_IS:      op==TK_EQ and p5==DBSQL_NULLEQ
**    if expr->op==TK_ISNOT:   op==TK_NE and p5==DBSQL_NULLEQ
**    otherwise:                op==expr->op and p5==0
*/
static void __code_vector_compare(
  parser_t *parser,        /* Code generator context */
  expr_t *expr,          /* The comparison operation */
  int dest,             /* Write results into this register */
  u_int8_t op,                /* Comparison operator */
  u_int8_t p5                 /* DBSQL_NULLEQ or zero */
){
  vdbe_t *v = parser->vdbe;
  expr_t *left = expr->left;
  expr_t *right = expr->right;
  int n_left = __expr_vector_size(left);
  int i;
  int reg_left = 0;
  int reg_right = 0;
  u_int8_t opx = op;
  int addr_cmp = 0;
  int addr_done = __vdbe_make_label(parser);
  int is_commuted = EXPR_HAS_PROPERTY(expr,EP_Commuted);

  assert( !EXPR_HAS_VVA_PROPERTY(expr,EP_Immutable) );
  if( parser->n_err ) return;
  if( n_left!=__expr_vector_size(right) ){
    __error_msg(parser, "row value misused");
    return;
  }
  assert( expr->op==TK_EQ || expr->op==TK_NE
       || expr->op==TK_IS || expr->op==TK_ISNOT
       || expr->op==TK_LT || expr->op==TK_GT
       || expr->op==TK_LE || expr->op==TK_GE
  );
  assert( expr->op==op || (expr->op==TK_IS && op==TK_EQ)
            || (expr->op==TK_ISNOT && op==TK_NE) );
  assert( p5==0 || expr->op!=op );
  assert( p5==DBSQL_NULLEQ || expr->op==op );

  if( op==TK_LE ) opx = TK_LT;
  if( op==TK_GE ) opx = TK_GT;
  if( op==TK_NE ) opx = TK_EQ;

  reg_left = __expr_code_subselect(parser, left);
  reg_right = __expr_code_subselect(parser, right);

  __vdbe_add_op2(v, OP_Integer, 1, dest);
  for(i=0; 1 /*Loop exits by "break"*/; i++){
    int reg_free1 = 0, reg_free2 = 0;
    expr_t *l = 0, *r = 0;
    int r1, r2;
    assert( i>=0 && i<n_left );
    if( addr_cmp ) __vdbe_jump_here(v, addr_cmp);
    r1 = __expr_vector_register(parser, left, i, reg_left, &l, &reg_free1);
    r2 = __expr_vector_register(parser, right, i, reg_right, &r, &reg_free2);
    addr_cmp = __vdbe_current_addr(v);
    __code_compare(parser, l, r, opx, r1, r2, addr_done, p5, is_commuted);
    testcase(op==OP_Lt); VDBE_COVERAGE_IF(v,op==OP_Lt);
    testcase(op==OP_Le); VDBE_COVERAGE_IF(v,op==OP_Le);
    testcase(op==OP_Gt); VDBE_COVERAGE_IF(v,op==OP_Gt);
    testcase(op==OP_Ge); VDBE_COVERAGE_IF(v,op==OP_Ge);
    testcase(op==OP_Eq); VDBE_COVERAGE_IF(v,op==OP_Eq);
    testcase(op==OP_Ne); VDBE_COVERAGE_IF(v,op==OP_Ne);
    __release_temp_reg(parser, reg_free1);
    __release_temp_reg(parser, reg_free2);
    if( (opx==TK_LT || opx==TK_GT) && i<n_left-1 ){
      addr_cmp = __vdbe_add_op0(v, OP_ElseEq);
      testcase(opx==TK_LT); VDBE_COVERAGE_IF(v,opx==TK_LT);
      testcase(opx==TK_GT); VDBE_COVERAGE_IF(v,opx==TK_GT);
    }
    if( p5==DBSQL_NULLEQ ){
      __vdbe_add_op2(v, OP_Integer, 0, dest);
    }else{
      __vdbe_add_op3(v, OP_ZeroOrNull, r1, dest, r2);
    }
    if( i==n_left-1 ){
      break;
    }
    if( opx==TK_EQ ){
      __vdbe_add_op2(v, OP_NotNull, dest, addr_done); VDBE_COVERAGE(v);
    }else{
      assert( op==TK_LT || op==TK_GT || op==TK_LE || op==TK_GE );
      __vdbe_add_op2(v, OP_Goto, 0, addr_done);
      if( i==n_left-2 ) opx = op;
    }
  }
  __vdbe_jump_here(v, addr_cmp);
  __vdbe_resolve_label(v, addr_done);
  if( op==TK_NE ){
    __vdbe_add_op2(v, OP_Not, dest, dest);
  }
}

#if DBSQL_MAX_EXPR_DEPTH>0
/*
** Check that argument n_height is less than or equal to the maximum
** expression depth allowed. If it is not, leave an error message in
** parser.
*/
int __expr_check_height(parser_t *parser, int n_height){
  int rc = DBSQL_OK;
  int mx_height = parser->db->limits[DBSQL_LIMIT_EXPR_DEPTH];
  if( n_height>mx_height ){
    __error_msg(parser,
       "Expression tree is too large (maximum depth %d)", mx_height
    );
    rc = DBSQL_ERROR;
  }
  return rc;
}

/* The following three functions, __height_of_expr(), __height_of_expr_list()
** and __height_of_select(), are used to determine the maximum height
** of any expression tree referenced by the structure passed as the
** first argument.
**
** If this maximum height is greater than the current value pointed
** to by pn_height, the second parameter, then set *pn_height to that
** value.
*/
static void __height_of_expr(const expr_t *p, int *pn_height){
  if( p ){
    if( p->n_height>*pn_height ){
      *pn_height = p->n_height;
    }
  }
}
static void __height_of_expr_list(const expr_list_t *p, int *pn_height){
  if( p ){
    int i;
    for(i=0; i<p->n_expr; i++){
      __height_of_expr(p->a[i].expr, pn_height);
    }
  }
}
static void __height_of_select(const select_t *p_select, int *pn_height){
  const select_t *p;
  for(p=p_select; p; p=p->prior){
    __height_of_expr(p->where, pn_height);
    __height_of_expr(p->having, pn_height);
    __height_of_expr(p->limit, pn_height);
    __height_of_expr_list(p->e_list, pn_height);
    __height_of_expr_list(p->group_by, pn_height);
    __height_of_expr_list(p->order_by, pn_height);
  }
}

/*
** Set the expr_t.n_height variable in the structure passed as an
** argument. An expression with no children, expr_t.list or
** expr_t.p_select member has a height of 1. Any other expression
** has a height equal to the maximum height of any other
** referenced expr_t plus one.
**
** Also propagate EP_Propagate flags up from expr_t.x.list to expr_t.flags,
** if appropriate.
*/
static void EXPR_SET_HEIGHT(expr_t *p){
  int n_height = p->left ? p->left->n_height : 0;
  if( NEVER(p->right) && p->right->n_height>n_height ){
    n_height = p->right->n_height;
  }
  if( EXPR_USE_X_SELECT(p) ){
    __height_of_select(p->x.p_select, &n_height);
  }else if( p->x.list ){
    __height_of_expr_list(p->x.list, &n_height);
    p->flags |= EP_Propagate & __expr_list_flags(p->x.list);
  }
  p->n_height = n_height + 1;
}

/*
** Set the expr_t.n_height variable using the EXPR_SET_HEIGHT() function. If
** the height is greater than the maximum allowed expression depth,
** leave an error in parser.
**
** Also propagate all EP_Propagate flags from the expr_t.x.list into
** expr_t.flags.
*/
void __expr_set_height_and_flags(parser_t *parser, expr_t *p){
  if( parser->n_err ) return;
  EXPR_SET_HEIGHT(p);
  __expr_check_height(parser, p->n_height);
}

/*
** Return the maximum height of any expression tree referenced
** by the select statement passed as an argument.
*/
int __select_expr_height(const select_t *p){
  int n_height = 0;
  __height_of_select(p, &n_height);
  return n_height;
}
#else /* ABOVE:  Height enforcement enabled.  BELOW: Height enforcement off */
/*
** Propagate all EP_Propagate flags from the expr_t.x.list into
** expr_t.flags.
*/
void __expr_set_height_and_flags(parser_t *parser, expr_t *p){
  if( parser->n_err ) return;
  if( p && EXPR_USE_X_LIST(p) && p->x.list ){
    p->flags |= EP_Propagate & __expr_list_flags(p->x.list);
  }
}
#define EXPR_SET_HEIGHT(y)
#endif /* DBSQL_MAX_EXPR_DEPTH>0 */

/*
** Set the error offset for an expr_t node, if possible.
*/
void __expr_set_error_offset(expr_t *expr, int i_ofst){
  if( expr==0 ) return;
  if( NEVER(EXPR_USE_W_JOIN(expr)) ) return;
  expr->w.i_ofst = i_ofst;
}

/*
** This routine is the core allocator for expr_t nodes.
**
** Construct a new expression node and return a pointer to it.  Memory
** for this node and for the token argument is a single allocation
** obtained from __db_malloc().  The calling function
** is responsible for making sure the node eventually gets freed.
**
** If dequote is true, then the token (if it exists) is dequoted.
** If dequote is false, no dequoting is performed.  The de_quote
** parameter is ignored if token is NULL or if the token does not
** appear to be quoted.  If the quotes were of the form "..." (double-quotes)
** then the EP_DblQuoted flag is set on the expression node.
**
** Special case (tag-20240227-a):  If op==TK_INTEGER and token points to
** a string that can be translated into a 32-bit integer, then the token is
** not stored in u.token.  Instead, the integer values is written
** into u.i_value and the EP_IntValue flag is set. No extra storage
** is allocated to hold the integer text and the dequote flag is ignored.
** See also tag-20240227-b.
*/
expr_t *__expr_alloc(
  DBSQL *db,            /* Handle for __db_malloc_raw_nn() */
  int op,                 /* Expression opcode */
  const Token *token,    /* Token argument.  Might be NULL */
  int dequote             /* True to dequote */
){
  expr_t *p_new;
  int n_extra = token ? token->n+1 : 0;

  assert( db!=0 );
  p_new = __db_malloc_raw_nn(db, sizeof(expr_t)+n_extra);
  if( p_new ){
    memset(p_new, 0, sizeof(expr_t));
    p_new->op = (u_int8_t)op;
    p_new->i_agg = -1;
    if( n_extra ){
      assert( token!=0 );
      p_new->u.token = (char*)&p_new[1];
      assert( token->z!=0 || token->n==0 );
      if( token->n ) memcpy(p_new->u.token, token->z, token->n);
      p_new->u.token[token->n] = 0;
      if( dequote && __isquote(p_new->u.token[0]) ){
        __dequote_expr(p_new);
      }
    }
#if DBSQL_MAX_EXPR_DEPTH>0
    p_new->n_height = 1;
#endif 
  }
  return p_new;
}

/*
** Allocate a new expression node from a zero-terminated token that has
** already been dequoted.
*/
expr_t *__expr(
  DBSQL *db,            /* Handle for __db_malloc_zero() (may be null) */
  int op,                 /* Expression opcode */
  const char *token      /* Token argument.  Might be NULL */
){
  Token x;
  x.z = token;
  x.n = __strlen30(token);
  return __expr_alloc(db, op, &x, 0);
}

/*
** Allocate an expression for a 32-bit signed integer literal.
*/
expr_t *__expr_int32(DBSQL *db, int i_val){
  expr_t *p_new = __db_malloc_raw_nn(db, sizeof(expr_t));
  if( p_new ){
    memset(p_new, 0, sizeof(expr_t));
    p_new->op = TK_INTEGER;
    p_new->i_agg = -1;
    p_new->flags = EP_IntValue|EP_Leaf|(i_val?EP_IsTrue:EP_IsFalse);
    p_new->u.i_value = i_val;
#if DBSQL_MAX_EXPR_DEPTH>0
    p_new->n_height = 1;
#endif 
  }
  return p_new;
}

/*
** Attach subtrees left and right to the expr_t node root.
**
** If root==NULL that means that a memory allocation error has occurred.
** In that case, delete the subtrees left and right.
*/
void __expr_attach_subtrees(
  DBSQL *db,
  expr_t *root,
  expr_t *left,
  expr_t *right
){
  if( root==0 ){
    assert( db->malloc_failed );
    __expr_delete(db, left);
    __expr_delete(db, right);
  }else{
    assert( EXPR_USE_X_LIST(root) );
    assert( root->x.p_select==0 );
    if( right ){
      root->right = right;
      root->flags |= EP_Propagate & right->flags;
#if DBSQL_MAX_EXPR_DEPTH>0
      root->n_height = right->n_height+1;
    }else{
      root->n_height = 1;
#endif
    }
    if( left ){
      root->left = left;
      root->flags |= EP_Propagate & left->flags;
#if DBSQL_MAX_EXPR_DEPTH>0
      if( left->n_height>=root->n_height ){
        root->n_height = left->n_height+1;
      }
#endif
    }
  }
}

/*
** Allocate an expr_t node which joins as many as two subtrees.
**
** One or both of the subtrees can be NULL.  Return a pointer to the new
** expr_t node.  Or, if an OOM error occurs, set parser->db->malloc_failed,
** free the subtrees and return NULL.
*/
expr_t *__p_expr(
  parser_t *parser,          /* Parsing context */
  int op,                 /* Expression opcode */
  expr_t *left,            /* Left operand */
  expr_t *right            /* Right operand */
){
  expr_t *p;
  p = __db_malloc_raw_nn(parser->db, sizeof(expr_t));
  if( p ){
    memset(p, 0, sizeof(expr_t));
    p->op = op & 0xff;
    p->i_agg = -1;
    __expr_attach_subtrees(parser->db, p, left, right);
    __expr_check_height(parser, p->n_height);
  }else{
    __expr_delete(parser->db, left);
    __expr_delete(parser->db, right);
  }
  return p;
}

/*
** Add p_select to the expr_t.x.p_select field.  Or, if expr is NULL (due
** do a memory allocation failure) then delete the p_select object.
*/
void __p_expr_add_select(parser_t *parser, expr_t *expr, select_t *p_select){
  if( expr ){
    expr->x.p_select = p_select;
    EXPR_SET_PROPERTY(expr, EP_xIsSelect|EP_Subquery);
    __expr_set_height_and_flags(parser, expr);
  }else{
    assert( parser->db->malloc_failed );
    __select_delete(parser->db, p_select);
  }
}

/*
** Expression list e_list is a list of vector values. This function
** converts the contents of e_list to a VALUES(...) select_t statement
** returning 1 row for each element of the list. For example, the
** expression list:
**
**   ( (1,2), (3,4) (5,6) )
**
** is translated to the equivalent of:
**
**   VALUES(1,2), (3,4), (5,6)
**
** Each of the vector values in e_list must contain exactly n_elem terms.
** If a list element that is not a vector or does not contain n_elem terms,
** an error message is left in parser.
**
** This is used as part of processing IN(...) expressions with a list
** of vectors on the RHS. e.g. "... IN ((1,2), (3,4), (5,6))".
*/
select_t *__expr_list_to_values(parser_t *parser, int n_elem, expr_list_t *e_list){
  int ii;
  select_t *ret = 0;
  assert( n_elem>1 );
  for(ii=0; ii<e_list->n_expr; ii++){
    select_t *sel;
    expr_t *expr = e_list->a[ii].expr;
    int n_expr_elem;
    if( expr->op==TK_VECTOR ){
      assert( EXPR_USE_X_LIST(expr) );
      n_expr_elem = expr->x.list->n_expr;
    }else{
      n_expr_elem = 1;
    }
    if( n_expr_elem!=n_elem ){
      __error_msg(parser, "IN(...) element has %d term%s - expected %d",
          n_expr_elem, n_expr_elem>1?"s":"", n_elem
      );
      break;
    }
    assert( EXPR_USE_X_LIST(expr) );
    sel = __select_new(parser, expr->x.list, 0, 0, 0, 0, 0, SF_Values,0);
    expr->x.list = 0;
    if( sel ){
      if( ret ){
        sel->op = TK_ALL;
        sel->prior = ret;
      }
      ret = sel;
    }
  }

  if( ret && ret->prior ){
    ret->sel_flags |= SF_MultiValue;
  }
  __expr_list_delete(parser->db, e_list);
  return ret;
}

/*
** Join two expressions using an AND operator.  If either expression is
** NULL, then just return the other expression.
**
** If one side or the other of the AND is known to be false, and neither side
** is part of an ON clause, then instead of returning an AND expression,
** just return a constant expression with a value of false.
*/
expr_t *__expr_and(parser_t *parser, expr_t *left, expr_t *right){
  DBSQL *db = parser->db;
  if( left==0  ){
    return right;
  }else if( right==0 ){
    return left;
  }else{
    u_int32_t f = left->flags | right->flags;
    if( (f&(EP_OuterON|EP_InnerON|EP_IsFalse|EP_HasFunc))==EP_IsFalse
     && !IN_RENAME_OBJECT
    ){
      __expr_deferred_delete(parser, left);
      __expr_deferred_delete(parser, right);
      return __expr_int32(db, 0);
    }else{
      return __p_expr(parser, TK_AND, left, right);
    }
  }
}

/*
** Construct a new expression node for a function with multiple
** arguments.
*/
expr_t *__expr_function(
  parser_t *parser,        /* Parsing context */
  expr_list_t *list,      /* Argument list */
  const Token *token,  /* Name of the function */
  int distinct         /* SF_Distinct or SF_ALL or 0 */
){
  expr_t *p_new;
  DBSQL *db = parser->db;
  assert( token );
  p_new = __expr_alloc(db, TK_FUNCTION, token, 1);
  if( p_new==0 ){
    __expr_list_delete(db, list); /* Avoid memory leak when malloc fails */
    return 0;
  }
  assert( !EXPR_HAS_PROPERTY(p_new, EP_InnerON|EP_OuterON) );
  p_new->w.i_ofst = (int)(token->z - parser->tail);
  if( list
   && list->n_expr > parser->db->limits[DBSQL_LIMIT_FUNCTION_ARG]
   && !parser->nested
  ){
    __error_msg(parser, "too many arguments on function %T", token);
  }
  p_new->x.list = list;
  EXPR_SET_PROPERTY(p_new, EP_HasFunc);
  assert( EXPR_USE_X_LIST(p_new) );
  __expr_set_height_and_flags(parser, p_new);
  if( distinct==SF_Distinct ) EXPR_SET_PROPERTY(p_new, EP_Distinct);
  return p_new;
}

/*
** Report an error when attempting to use an ORDER BY clause within
** the arguments of a non-aggregate function.
*/
void __expr_order_by_aggregate_error(parser_t *parser, expr_t *p){
  __error_msg(parser,
     "ORDER BY may not be used with non-aggregate %#T()", p
  );
}

/*
** Attach an ORDER BY clause to a function call.
**
**     functionname( arguments ORDER BY sortlist )
**     \_____________________/          \______/
**             expr                    order_by
**
** The ORDER BY clause is inserted into a new expr_t node of type TK_ORDER
** and added to the expr_t.left field of the parent TK_FUNCTION node.
*/
void __expr_add_function_order_by(
  parser_t *parser,        /* Parsing context */
  expr_t *expr,          /* The function call to which ORDER BY is to be added */
  expr_list_t *order_by    /* The ORDER BY clause to add */
){
  expr_t *ob;
  DBSQL *db = parser->db;
  if( NEVER(order_by==0) ){
    assert( db->malloc_failed );
    return;
  }
  if( expr==0 ){
    assert( db->malloc_failed );
    __expr_list_delete(db, order_by);
    return;
  }
  assert( expr->op==TK_FUNCTION );
  assert( expr->left==0 );
  assert( EXPR_USE_X_LIST(expr) );
  if( expr->x.list==0 || NEVER(expr->x.list->n_expr==0) ){
    /* Ignore ORDER BY on zero-argument aggregates */
    __parser_add_cleanup(parser, __expr_list_delete_generic, order_by);
    return;
  }
  if( IS_WINDOW_FUNC(expr) ){
    __expr_order_by_aggregate_error(parser, expr);
    __expr_list_delete(db, order_by);
    return;
  }
  if( order_by->n_expr>db->limits[DBSQL_LIMIT_COLUMN] ){
    __error_msg(parser, "too many terms in ORDER BY clause");
    __expr_list_delete(db, order_by);
    return;
  }

  ob = __expr_alloc(db, TK_ORDER, 0, 0);
  if( ob==0 ){
    __expr_list_delete(db, order_by);
    return;
  }
  ob->x.list = order_by;
  assert( EXPR_USE_X_LIST(ob) );
  expr->left = ob;
  EXPR_SET_PROPERTY(ob, EP_FullSize);
}

/*
** Check to see if a function is usable according to current access
** rules:
**
**    DBSQL_FUNC_DIRECT    -     Only usable from top-level SQL
**
**    DBSQL_FUNC_UNSAFE    -     Usable if TRUSTED_SCHEMA or from
**                                top-level SQL
**
** If the function is not usable, create an error.
*/
void __expr_function_usable(
  parser_t *parser,         /* Parsing and code generating context */
  const expr_t *expr,     /* The function invocation */
  const func_def_t *def    /* The function being invoked */
){
  assert( !IN_RENAME_OBJECT );
  assert( (def->func_flags & (DBSQL_FUNC_DIRECT|DBSQL_FUNC_UNSAFE))!=0 );
  if( EXPR_HAS_PROPERTY(expr, EP_FromDDL) 
   || parser->prep_flags & DBSQL_PREPARE_FROM_DDL
  ){
    if( (def->func_flags & DBSQL_FUNC_DIRECT)!=0
     || (parser->db->flags & DBSQL_TrustedSchema)==0
    ){
      /* Functions prohibited in triggers and views if:
      **     (1) tagged with DBSQL_DIRECTONLY
      **     (2) not tagged with DBSQL_INNOCUOUS (which means it
      **         is tagged with DBSQL_FUNC_UNSAFE) and
      **         DBSQL_DBCONFIG_TRUSTED_SCHEMA is off (meaning
      **         that the schema is possibly tainted).
      */
      __error_msg(parser, "unsafe use of %#T()", expr);
    }
  }
}

/*
** Assign a variable number to an expression that encodes a wildcard
** in the original SQL statement. 
**
** Wildcards consisting of a single "?" are assigned the next sequential
** variable number.
**
** Wildcards of the form "?nnn" are assigned the number "nnn".  We make
** sure "nnn" is not too big to avoid a denial of service attack when
** the SQL statement comes from an external source.
**
** Wildcards of the form ":aaa", "@aaa", or "$aaa" are assigned the same number
** as the previous instance of the same wildcard.  Or if this is the first
** instance of the wildcard, the next sequential variable number is
** assigned.
*/
void __expr_assign_var_number(parser_t *parser, expr_t *expr, u_int32_t n){
  DBSQL *db = parser->db;
  const char *z;
  yn_var_t x;

  if( expr==0 ) return;
  assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue|EP_Reduced|EP_TokenOnly) );
  z = expr->u.token;
  assert( z!=0 );
  assert( z[0]!=0 );
  assert( n==(u_int32_t)__strlen30(z) );
  if( z[1]==0 ){
    /* Wildcard of the form "?".  Assign the next variable number */
    assert( z[0]=='?' );
    x = (yn_var_t)(++parser->n_var);
  }else{
    int do_add = 0;
    if( z[0]=='?' ){
      /* Wildcard of the form "?nnn".  Convert "nnn" to an integer and
      ** use it as the variable number */
      int64_t i;
      int ok;
      if( n==2 ){ /*OPTIMIZATION-IF-TRUE*/
        i = z[1]-'0';  /* The common case of ?N for a single digit N */
        ok = 1;
      }else{
        ok = 0==__atoi64(&z[1], &i, n-1, DBSQL_UTF8);
      }
      testcase( i==0 );
      testcase( i==1 );
      testcase( i==db->limits[DBSQL_LIMIT_VARIABLE_NUMBER]-1 );
      testcase( i==db->limits[DBSQL_LIMIT_VARIABLE_NUMBER] );
      if( ok==0 || i<1 || i>db->limits[DBSQL_LIMIT_VARIABLE_NUMBER] ){
        __error_msg(parser, "variable number must be between ?1 and ?%d",
            db->limits[DBSQL_LIMIT_VARIABLE_NUMBER]);
        __record_error_offset_of_expr(parser->db, expr);
        return;
      }
      x = (yn_var_t)i;
      if( x>parser->n_var ){
        parser->n_var = (int)x;
        do_add = 1;
      }else if( __v_list_num_to_name(parser->v_list, x)==0 ){
        do_add = 1;
      }
    }else{
      /* Wildcards like ":aaa", "$aaa" or "@aaa".  Reuse the same variable
      ** number as the prior appearance of the same name, or if the name
      ** has never appeared before, reuse the same variable number
      */
      x = (yn_var_t)__v_list_name_to_num(parser->v_list, z, n);
      if( x==0 ){
        x = (yn_var_t)(++parser->n_var);
        do_add = 1;
      }
    }
    if( do_add ){
      parser->v_list = __v_list_add(db, parser->v_list, z, n, x);
    }
  }
  expr->i_column = x;
  if( x>db->limits[DBSQL_LIMIT_VARIABLE_NUMBER] ){
    __error_msg(parser, "too many SQL variables");
    __record_error_offset_of_expr(parser->db, expr);
  }
}

/*
** Recursively delete an expression tree.
*/
static DBSQL_NOINLINE void __expr_delete_nn(DBSQL *db, expr_t *p){
  assert( p!=0 );
  assert( db!=0 );
expr_delete_restart:
  assert( !EXPR_USE_U_VALUE(p) || p->u.i_value>=0 );
  assert( !EXPR_USE_Y_WIN(p) || !EXPR_USE_Y_SUB(p) );
  assert( !EXPR_USE_Y_WIN(p) || p->y.win!=0 || db->malloc_failed );
  assert( p->op!=TK_FUNCTION || !EXPR_USE_Y_SUB(p) );
#ifdef DBSQL_DEBUG
  if( EXPR_HAS_PROPERTY(p, EP_Leaf) && !EXPR_HAS_PROPERTY(p, EP_TokenOnly) ){
    assert( p->left==0 );
    assert( p->right==0 );
    assert( !EXPR_USE_X_SELECT(p) || p->x.p_select==0 );
    assert( !EXPR_USE_X_LIST(p) || p->x.list==0 );
  }
#endif
  if( !EXPR_HAS_PROPERTY(p, (EP_TokenOnly|EP_Leaf)) ){
    /* The expr_t.x union is never used at the same time as expr_t.right */
    assert( (EXPR_USE_X_LIST(p) && p->x.list==0) || p->right==0 );
    if( p->right ){
      assert( !EXPR_HAS_PROPERTY(p, EP_WinFunc) );
      __expr_delete_nn(db, p->right);
    }else if( EXPR_USE_X_SELECT(p) ){
      assert( !EXPR_HAS_PROPERTY(p, EP_WinFunc) );
      __select_delete(db, p->x.p_select);
    }else{
      __expr_list_delete(db, p->x.list);
#ifndef DBSQL_OMIT_WINDOWFUNC
      if( EXPR_HAS_PROPERTY(p, EP_WinFunc) ){
        __window_delete(db, p->y.win);
      }
#endif
    }
    if( p->left && p->op!=TK_SELECT_COLUMN ){
      expr_t *left = p->left;
      if( !EXPR_HAS_PROPERTY(p, EP_Static)
       && !EXPR_HAS_PROPERTY(left, EP_Static)
      ){
        /* Avoid unnecessary recursion on unary operators */
        __db_nn_free_nn(db, p);
        p = left;
        goto expr_delete_restart;
      }else{
        __expr_delete_nn(db, left);
      }
    }
  }
  if( !EXPR_HAS_PROPERTY(p, EP_Static) ){
    __db_nn_free_nn(db, p);
  }
}
void __expr_delete(DBSQL *db, expr_t *p){
  if( p ) __expr_delete_nn(db, p);
}
void __expr_delete_generic(DBSQL *db, void *p){
  if( ALWAYS(p) ) __expr_delete_nn(db, (expr_t*)p);
}

/*
** Clear both elements of an on_or_using_t object
*/
void __clear_on_or_using(DBSQL *db, on_or_using_t *p){
  if( p==0 ){
    /* Nothing to clear */
  }else if( p->on ){
    __expr_delete_nn(db, p->on);
  }else if( p->p_using ){
    __id_list_delete(db, p->p_using);
  }
}

/*
** Arrange to cause expr to be deleted when the parser is deleted.
** This is similar to __expr_delete() except that the delete is
** deferred until the parser is deleted.
**
** The expr might be deleted immediately on an OOM error.
**
** Return 0 if the delete was successfully deferred.  Return non-zero
** if the delete happened immediately because of an OOM.
*/
int __expr_deferred_delete(parser_t *parser, expr_t *expr){
  return 0==__parser_add_cleanup(parser, __expr_delete_generic, expr);
}

/* Invoke __rename_expr_unmap() and __expr_delete() on the
** expression.
*/
void __expr_unmap_and_delete(parser_t *parser, expr_t *p){
  if( p ){
    if( IN_RENAME_OBJECT ){
      __rename_expr_unmap(parser, p);
    }
    __expr_delete_nn(parser->db, p);
  }
}

/*
** Return the number of bytes allocated for the expression structure
** passed as the first argument. This is always one of EXPR_FULLSIZE,
** EXPR_REDUCEDSIZE or EXPR_TOKENONLYSIZE.
*/
static int __expr_struct_size(const expr_t *p){
  if( EXPR_HAS_PROPERTY(p, EP_TokenOnly) ) return EXPR_TOKENONLYSIZE;
  if( EXPR_HAS_PROPERTY(p, EP_Reduced) ) return EXPR_REDUCEDSIZE;
  return EXPR_FULLSIZE;
}

/*
** The duped_expr*Size() routines each return the number of bytes required
** to store a copy of an expression or expression tree.  They differ in
** how much of the tree is measured.
**
**     __duped_expr_struct_size()     Size of only the expr_t structure
**     __duped_expr_node_size()       Size of expr_t + space for token
**     __duped_expr_size()           expr_t + token + subtree components
**
***************************************************************************
**
** The __duped_expr_struct_size() function returns two values OR-ed together: 
** (1) the space required for a copy of the expr_t structure only and
** (2) the EP_xxx flags that indicate what the structure size should be.
** The return values is always one of:
**
**      EXPR_FULLSIZE
**      EXPR_REDUCEDSIZE   | EP_Reduced
**      EXPR_TOKENONLYSIZE | EP_TokenOnly
**
** The size of the structure can be found by masking the return value
** of this routine with 0xfff.  The flags can be found by masking the
** return value with EP_Reduced|EP_TokenOnly.
**
** Note that with flags==EXPRDUP_REDUCE, this routines works on full-size
** (unreduced) expr_t objects as they or originally constructed by the parser.
** During expression analysis, extra information is computed and moved into
** later parts of the expr_t object and that extra information might get chopped
** off if the expression is reduced.  Note also that it does not work to
** make an EXPRDUP_REDUCE copy of a reduced expression.  It is only legal
** to reduce a pristine expression tree from the parser.  The implementation
** of __duped_expr_struct_size() contain multiple assert() statements that attempt
** to enforce this constraint.
*/
static int __duped_expr_struct_size(const expr_t *p, int flags){
  int n_size;
  assert( flags==EXPRDUP_REDUCE || flags==0 ); /* Only one flag value allowed */
  assert( EXPR_FULLSIZE<=0xfff );
  assert( (0xfff & (EP_Reduced|EP_TokenOnly))==0 );
  if( 0==flags || EXPR_HAS_PROPERTY(p, EP_FullSize) ){
    n_size = EXPR_FULLSIZE;
  }else{
    assert( !EXPR_HAS_PROPERTY(p, EP_TokenOnly|EP_Reduced) );
    assert( !EXPR_HAS_PROPERTY(p, EP_OuterON) );
    assert( !EXPR_HAS_VVA_PROPERTY(p, EP_NoReduce) );
    if( p->left || p->x.list ){
      n_size = EXPR_REDUCEDSIZE | EP_Reduced;
    }else{
      assert( p->right==0 );
      n_size = EXPR_TOKENONLYSIZE | EP_TokenOnly;
    }
  }
  return n_size;
}

/*
** This function returns the space in bytes required to store the copy
** of the expr_t structure and a copy of the expr_t.u.token string (if that
** string is defined.)
*/
static int __duped_expr_node_size(const expr_t *p, int flags){
  int n_byte = __duped_expr_struct_size(p, flags) & 0xfff;
  if( !EXPR_HAS_PROPERTY(p, EP_IntValue) && p->u.token ){
    n_byte += __strlen30_nn(p->u.token)+1;
  }
  return ROUND8(n_byte);
}

/*
** Return the number of bytes required to create a duplicate of the
** expression passed as the first argument.
**
** The value returned includes space to create a copy of the expr_t struct
** itself and the buffer referred to by expr_t.u.token, if any.
**
** The return value includes space to duplicate all expr_t nodes in the
** tree formed by expr_t.left and expr_t.right, but not any other
** substructure such as expr_t.x.list, expr_t.x.p_select, and expr_t.y.win.
*/
static int __duped_expr_size(const expr_t *p){
  int n_byte;
  assert( p!=0 );
  n_byte = __duped_expr_node_size(p, EXPRDUP_REDUCE);
  if( p->left ) n_byte += __duped_expr_size(p->left);
  if( p->right ) n_byte += __duped_expr_size(p->right);
  assert( n_byte==ROUND8(n_byte) );
  return n_byte;
}

/*
** An edup_buf_t is a memory allocation used to stored multiple expr_t objects
** together with their expr_t.token content.  This is used to help implement
** compression while doing __expr_dup().  The top-level expr_t does the
** allocation for itself and many of its decendents, then passes an instance
** of the structure down into __expr_dup_local() so that they decendents can have
** access to that memory.
*/
typedef struct edup_buf_t edup_buf_t;
struct edup_buf_t {
  u_int8_t *alloc;          /* Memory space available for storage */
#ifdef DBSQL_DEBUG
  u_int8_t *end;            /* First byte past the end of memory */
#endif
};

/*
** This function is similar to __expr_dup(), except that if edup_buf
** is not NULL then it points to memory that can be used to store a copy
** of the input expr_t p together with its p->u.token (if any).  edup_buf
** is updated with the new buffer tail prior to returning.
*/
static expr_t *__expr_dup_local(
  DBSQL *db,          /* Database connection (for memory allocation) */
  const expr_t *p,        /* expr_t tree to be duplicated */
  int dup_flags,         /* EXPRDUP_REDUCE for compression.  0 if not */
  edup_buf_t *edup_buf     /* Preallocated storage space, or NULL */
){
  expr_t *p_new;           /* Value to return */
  edup_buf_t s_edup_buf;     /* Memory space from which to build expr_t object */
  u_int32_t static_flag;       /* EP_Static if space not obtained from malloc */
  int n_token = -1;       /* Space needed for p->u.token.  -1 means unknown */

  assert( db!=0 );
  assert( p );
  assert( dup_flags==0 || dup_flags==EXPRDUP_REDUCE );
  assert( edup_buf==0 || dup_flags==EXPRDUP_REDUCE );

  /* Figure out where to write the new expr_t structure. */
  if( edup_buf ){
    s_edup_buf.alloc = edup_buf->alloc;
#ifdef DBSQL_DEBUG
    s_edup_buf.end = edup_buf->end;
#endif
    static_flag = EP_Static;
    assert( s_edup_buf.alloc!=0 );
    assert( dup_flags==EXPRDUP_REDUCE );
  }else{
    int n_alloc;
    if( dup_flags ){
      n_alloc = __duped_expr_size(p);
    }else if( !EXPR_HAS_PROPERTY(p, EP_IntValue) && p->u.token ){
      n_token = __strlen30_nn(p->u.token)+1;
      n_alloc = ROUND8(EXPR_FULLSIZE + n_token);
    }else{
      n_token = 0;
      n_alloc = ROUND8(EXPR_FULLSIZE);
    }
    assert( n_alloc==ROUND8(n_alloc) );
    s_edup_buf.alloc = __db_malloc_raw_nn(db, n_alloc);
#ifdef DBSQL_DEBUG
    s_edup_buf.end = s_edup_buf.alloc ? s_edup_buf.alloc+n_alloc : 0;
#endif
    
    static_flag = 0;
  }
  p_new = (expr_t *)s_edup_buf.alloc;
  assert( EIGHT_BYTE_ALIGNMENT(p_new) );

  if( p_new ){
    /* Set n_new_size to the size allocated for the structure pointed to
    ** by p_new. This is either EXPR_FULLSIZE, EXPR_REDUCEDSIZE or
    ** EXPR_TOKENONLYSIZE. n_token is set to the number of bytes consumed
    ** by the copy of the p->u.token string (if any).
    */
    const unsigned n_struct_size = __duped_expr_struct_size(p, dup_flags);
    int n_new_size = n_struct_size & 0xfff;
    if( n_token<0 ){
      if( !EXPR_HAS_PROPERTY(p, EP_IntValue) && p->u.token ){
        n_token = __strlen30(p->u.token) + 1;
      }else{
        n_token = 0;
      }
    }
    if( dup_flags ){
      assert( (int)(s_edup_buf.end - s_edup_buf.alloc) >= n_new_size+n_token );
      assert( EXPR_HAS_PROPERTY(p, EP_Reduced)==0 );
      memcpy(s_edup_buf.alloc, p, n_new_size);
    }else{
      u_int32_t n_size = (u_int32_t)__expr_struct_size(p);
      assert( (int)(s_edup_buf.end - s_edup_buf.alloc) >=
                                                   (int)EXPR_FULLSIZE+n_token );
      memcpy(s_edup_buf.alloc, p, n_size);
      if( n_size<EXPR_FULLSIZE ){
        memset(&s_edup_buf.alloc[n_size], 0, EXPR_FULLSIZE-n_size);
      }
      n_new_size = EXPR_FULLSIZE;
    }

    /* Set the EP_Reduced, EP_TokenOnly, and EP_Static flags appropriately. */
    p_new->flags &= ~(EP_Reduced|EP_TokenOnly|EP_Static);
    p_new->flags |= n_struct_size & (EP_Reduced|EP_TokenOnly);
    p_new->flags |= static_flag;
    EXPR_CLEAR_VVA_PROPERTIES(p_new);
    if( dup_flags ){
      EXPR_SET_VVA_PROPERTY(p_new, EP_Immutable);
    }

    /* Copy the p->u.token string, if any. */
    assert( n_token>=0 );
    if( n_token>0 ){
      char *token = p_new->u.token = (char*)&s_edup_buf.alloc[n_new_size];
      memcpy(token, p->u.token, n_token);
      n_new_size += n_token;
    }
    s_edup_buf.alloc += ROUND8(n_new_size);

    if( ((p->flags|p_new->flags)&(EP_TokenOnly|EP_Leaf))==0 ){

      /* Fill in the p_new->x.p_select or p_new->x.list member. */
      if( EXPR_USE_X_SELECT(p) ){
        p_new->x.p_select = __select_dup(db, p->x.p_select, dup_flags);
      }else{
        p_new->x.list = __expr_list_dup(db, p->x.list,
                           p->op!=TK_ORDER ? dup_flags : 0);
      }

#ifndef DBSQL_OMIT_WINDOWFUNC
      if( EXPR_HAS_PROPERTY(p, EP_WinFunc) ){
        p_new->y.win = __window_dup(db, p_new, p->y.win);
        assert( EXPR_HAS_PROPERTY(p_new, EP_WinFunc) );
      }
#endif /* DBSQL_OMIT_WINDOWFUNC */

      /* Fill in p_new->left and p_new->right. */
      if( dup_flags ){
        if( p->op==TK_SELECT_COLUMN ){
          p_new->left = p->left;
          assert( p->right==0 
               || p->right==p->left
               || EXPR_HAS_PROPERTY(p->left, EP_Subquery) );
        }else{
          p_new->left = p->left ?
                      __expr_dup_local(db, p->left, EXPRDUP_REDUCE, &s_edup_buf) : 0;
        }
        p_new->right = p->right ?
                       __expr_dup_local(db, p->right, EXPRDUP_REDUCE, &s_edup_buf) : 0;
      }else{
        if( p->op==TK_SELECT_COLUMN ){
          p_new->left = p->left;
          assert( p->right==0 
               || p->right==p->left
               || EXPR_HAS_PROPERTY(p->left, EP_Subquery) );
        }else{
          p_new->left = __expr_dup(db, p->left, 0);
        }
        p_new->right = __expr_dup(db, p->right, 0);
      }
    }
  }
  if( edup_buf ) memcpy(edup_buf, &s_edup_buf, sizeof(s_edup_buf));
  assert( s_edup_buf.alloc <= s_edup_buf.end );
  return p_new;
}

/*
** Create and return a deep copy of the object passed as the second
** argument. If an OOM condition is encountered, NULL is returned
** and the db->malloc_failed flag set.
*/
#ifndef DBSQL_OMIT_CTE
with_t *__with_dup(DBSQL *db, with_t *p){
  with_t *ret = 0;
  if( p ){
    int64_t n_byte = SZ_WITH(p->n_cte);
    ret = __db_malloc_zero(db, n_byte);
    if( ret ){
      int i;
      ret->n_cte = p->n_cte;
      for(i=0; i<p->n_cte; i++){
        ret->a[i].p_select = __select_dup(db, p->a[i].p_select, 0);
        ret->a[i].cols = __expr_list_dup(db, p->a[i].cols, 0);
        ret->a[i].name = __db_str_dup(db, p->a[i].name);
        ret->a[i].m10d = p->a[i].m10d;
      }
    }
  }
  return ret;
}
#else
# define __with_dup(x,y) 0
#endif

#ifndef DBSQL_OMIT_WINDOWFUNC
/*
** The __gather_select_windows() procedure and its helper routine
** __gather_select_windows_callback() are used to scan all the expressions
** an a newly duplicated SELECT statement and gather all of the window_t
** objects found there, assembling them onto the linked list at select_t->win.
*/
static int __gather_select_windows_callback(walker_t *walker, expr_t *expr){
  if( expr->op==TK_FUNCTION && EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
    select_t *p_select = walker->u.p_select;
    window_t *win = expr->y.win;
    assert( win );
    assert( IS_WINDOW_FUNC(expr) );
    assert( win->pp_this==0 );
    __window_link(p_select, win);
  }
  return WRC_Continue;
}
static int __gather_select_windows_select_callback(walker_t *walker, select_t *p){
  return p==walker->u.p_select ? WRC_Continue : WRC_Prune;
}
static void __gather_select_windows(select_t *p){
  walker_t w;
  w.expr_callback = __gather_select_windows_callback;
  w.select_callback = __gather_select_windows_select_callback;
  w.select_callback2 = 0;
  w.parser = 0;
  w.u.p_select = p;
  __walk_select(&w, p);
}
#endif


/*
** The following group of routines make deep copies of expressions,
** expression lists, ID lists, and select statements.  The copies can
** be deleted (by being passed to their respective ...Delete() routines)
** without effecting the originals.
**
** The expression list, ID, and source lists return by __expr_list_dup(),
** __id_list_dup(), and __src_list_dup() can not be further expanded
** by subsequent calls to sqlite*__list_append() routines.
**
** Any tables that the src_list_t might point to are not duplicated.
**
** The flags parameter contains a combination of the EXPRDUP_XXX flags.
** If the EXPRDUP_REDUCE flag is set, then the structure returned is a
** truncated version of the usual expr_t structure that will be stored as
** part of the in-memory representation of the database schema.
*/
expr_t *__expr_dup(DBSQL *db, const expr_t *p, int flags){
  assert( flags==0 || flags==EXPRDUP_REDUCE );
  return p ? __expr_dup_local(db, p, flags, 0) : 0;
}
expr_list_t *__expr_list_dup(DBSQL *db, const expr_list_t *p, int flags){
  expr_list_t *p_new;
  struct expr_list_item_t *item;
  const struct expr_list_item_t *old_item;
  int i;
  expr_t *prior_select_col_old = 0;
  expr_t *prior_select_col_new = 0;
  assert( db!=0 );
  if( p==0 ) return 0;
  p_new = __db_malloc_raw_nn(db, __db_malloc_size(db, p));
  if( p_new==0 ) return 0;
  p_new->n_expr = p->n_expr;
  p_new->n_alloc = p->n_alloc;
  item = p_new->a;
  old_item = p->a;
  for(i=0; i<p->n_expr; i++, item++, old_item++){
    expr_t *old_expr = old_item->expr;
    expr_t *new_expr;
    item->expr = __expr_dup(db, old_expr, flags);
    if( old_expr
     && old_expr->op==TK_SELECT_COLUMN
     && (new_expr = item->expr)!=0
    ){
      if( new_expr->right ){
        prior_select_col_old = old_expr->right;
        prior_select_col_new = new_expr->right;
        new_expr->left = new_expr->right;
      }else{
        if( old_expr->left!=prior_select_col_old ){
          prior_select_col_old = old_expr->left;
          prior_select_col_new = __expr_dup(db, prior_select_col_old, flags);
          new_expr->right = prior_select_col_new;
        }
        new_expr->left = prior_select_col_new;
      }
    }
    item->e_name = __db_str_dup(db, old_item->e_name);
    item->fg = old_item->fg;
    item->u = old_item->u;
  }
  return p_new;
}

/*
** If cursors, triggers, views and subqueries are all omitted from
** the build, then none of the following routines, except for
** __select_dup(), can be called. __select_dup() is sometimes
** called with a NULL argument.
*/
#if !defined(DBSQL_OMIT_VIEW) || !defined(DBSQL_OMIT_TRIGGER) \
 || !defined(DBSQL_OMIT_SUBQUERY)
src_list_t *__src_list_dup(DBSQL *db, const src_list_t *p, int flags){
  src_list_t *p_new;
  int i;
  assert( db!=0 );
  if( p==0 ) return 0;
  p_new = __db_malloc_raw_nn(db, SZ_SRCLIST(p->n_src) );
  if( p_new==0 ) return 0;
  p_new->n_src = p_new->n_alloc = p->n_src;
  for(i=0; i<p->n_src; i++){
    src_item_t *new_item = &p_new->a[i];
    const src_item_t *old_item = &p->a[i];
    table_t *tab;
    new_item->fg = old_item->fg;
    if( old_item->fg.is_subquery ){
      subquery_t *new_subq = __db_malloc_raw(db, sizeof(subquery_t));
      if( new_subq==0 ){
        assert( db->malloc_failed );
        new_item->fg.is_subquery = 0;
      }else{
        memcpy(new_subq, old_item->u4.subq, sizeof(*new_subq));
        new_subq->p_select = __select_dup(db, new_subq->p_select, flags);
        if( new_subq->p_select==0 ){
          __dbsql_db_free(db, new_subq);
          new_subq = 0;
          new_item->fg.is_subquery = 0;
        }
      }
      new_item->u4.subq = new_subq;
    }else if( old_item->fg.fixed_schema ){
      new_item->u4.schema = old_item->u4.schema;
    }else{
      new_item->u4.database = __db_str_dup(db, old_item->u4.database);
    }
    new_item->name = __db_str_dup(db, old_item->name);
    new_item->alias = __db_str_dup(db, old_item->alias);
    new_item->i_cursor = old_item->i_cursor;
    if( new_item->fg.is_indexed_by ){
      new_item->u1.indexed_by = __db_str_dup(db, old_item->u1.indexed_by);
    }else if( new_item->fg.is_tab_func ){
      new_item->u1.func_arg =
          __expr_list_dup(db, old_item->u1.func_arg, flags);
    }else{
      new_item->u1.n_row = old_item->u1.n_row;
    }
    new_item->u2 = old_item->u2;
    if( new_item->fg.is_cte ){
      new_item->u2.cte_use->n_use++;
    }
    tab = new_item->s_tab = old_item->s_tab;
    if( tab ){
      tab->n_tab_ref++;
    }
    if( old_item->fg.is_using ){
      assert( new_item->fg.is_using );
      new_item->u3.p_using = __id_list_dup(db, old_item->u3.p_using);
    }else{
      new_item->u3.on = __expr_dup(db, old_item->u3.on, flags);
    }
    new_item->col_used = old_item->col_used;
  }
  return p_new;
}
id_list_t *__id_list_dup(DBSQL *db, const id_list_t *p){
  id_list_t *p_new;
  int i;
  assert( db!=0 );
  if( p==0 ) return 0;
  p_new = __db_malloc_raw_nn(db, SZ_IDLIST(p->n_id));
  if( p_new==0 ) return 0;
  p_new->n_id = p->n_id;
  for(i=0; i<p->n_id; i++){
    struct id_list_item_t *new_item = &p_new->a[i];
    const struct id_list_item_t *old_item = &p->a[i];
    new_item->name = __db_str_dup(db, old_item->name);
  }
  return p_new;
}
select_t *__select_dup(DBSQL *db, const select_t *dup, int flags){
  select_t *ret = 0;
  select_t *next = 0;
  select_t **pp = &ret;
  const select_t *p;

  assert( db!=0 );
  for(p=dup; p; p=p->prior){
    select_t *p_new = __db_malloc_raw_nn(db, sizeof(*p) );
    if( p_new==0 ) break;
    p_new->e_list = __expr_list_dup(db, p->e_list, flags);
    p_new->src = __src_list_dup(db, p->src, flags);
    p_new->where = __expr_dup(db, p->where, flags);
    p_new->group_by = __expr_list_dup(db, p->group_by, flags);
    p_new->having = __expr_dup(db, p->having, flags);
    p_new->order_by = __expr_list_dup(db, p->order_by, flags);
    p_new->op = p->op;
    p_new->next = next;
    p_new->prior = 0;
    p_new->limit = __expr_dup(db, p->limit, flags);
    p_new->i_limit = 0;
    p_new->i_offset = 0;
    p_new->sel_flags = p->sel_flags;
    p_new->n_select_row = p->n_select_row;
    p_new->with = __with_dup(db, p->with);
#ifndef DBSQL_OMIT_WINDOWFUNC
    p_new->win = 0;
    p_new->win_defn = __window_list_dup(db, p->win_defn);
    if( p->win && db->malloc_failed==0 ) __gather_select_windows(p_new);
#endif
    p_new->sel_id = p->sel_id;
    if( db->malloc_failed ){
      /* Any prior OOM might have left the select_t object incomplete.
      ** Delete the whole thing rather than allow an incomplete select_t
      ** to be used by the code generator. */
      p_new->next = 0;
      __select_delete(db, p_new);
      break;
    }
    *pp = p_new;
    pp = &p_new->prior;
    next = p_new;
  }
  return ret;
}
#else
select_t *__select_dup(DBSQL *db, const select_t *p, int flags){
  assert( p==0 );
  return 0;
}
#endif


/*
** Add a new element to the end of an expression list.  If list is
** initially NULL, then create a new expression list.
**
** The list argument must be either NULL or a pointer to an expr_list_t
** obtained from a prior call to __expr_list_append().
**
** If a memory allocation error occurs, the entire list is freed and
** NULL is returned.  If non-NULL is returned, then it is guaranteed
** that the new entry was successfully appended.
*/
static const struct expr_list_item_t __zero_item = {0};
DBSQL_NOINLINE expr_list_t *__expr_list_append_new(
  DBSQL *db,            /* Database handle.  Used for memory allocation */
  expr_t *expr             /* Expression to be appended. Might be NULL */
){
  struct expr_list_item_t *item;
  expr_list_t *list;

  list = __db_malloc_raw_nn(db, SZ_EXPRLIST(4));
  if( list==0 ){
    __expr_delete(db, expr);
    return 0;
  }
  list->n_alloc = 4;
  list->n_expr = 1;
  item = &list->a[0];
  *item = __zero_item;
  item->expr = expr;
  return list;
}
DBSQL_NOINLINE expr_list_t *__expr_list_append_grow(
  DBSQL *db,            /* Database handle.  Used for memory allocation */
  expr_list_t *list,        /* List to which to append. Might be NULL */
  expr_t *expr             /* Expression to be appended. Might be NULL */
){
  struct expr_list_item_t *item;
  expr_list_t *p_new;
  list->n_alloc *= 2;
  p_new = __db_realloc(db, list, SZ_EXPRLIST(list->n_alloc));
  if( p_new==0 ){
    __expr_list_delete(db, list);
    __expr_delete(db, expr);
    return 0;
  }else{
    list = p_new;
  }
  item = &list->a[list->n_expr++];
  *item = __zero_item;
  item->expr = expr;
  return list;
}
expr_list_t *__expr_list_append(
  parser_t *parser,          /* Parsing context */
  expr_list_t *list,        /* List to which to append. Might be NULL */
  expr_t *expr             /* Expression to be appended. Might be NULL */
){
  struct expr_list_item_t *item;
  if( list==0 ){
    return __expr_list_append_new(parser->db,expr);
  }
  if( list->n_alloc<list->n_expr+1 ){
    return __expr_list_append_grow(parser->db,list,expr);
  }
  item = &list->a[list->n_expr++];
  *item = __zero_item;
  item->expr = expr;
  return list;
}

/*
** columns and expr form a vector assignment which is part of the SET
** clause of an UPDATE statement.  Like this:
**
**        (a,b,c) = (expr1,expr2,expr3)
** Or:    (a,b,c) = (SELECT x,y,z FROM ....)
**
** For each term of the vector assignment, append new entries to the
** expression list list.  In the case of a subquery on the RHS, append
** TK_SELECT_COLUMN expressions.
*/
expr_list_t *__expr_list_append_vector(
  parser_t *parser,         /* Parsing context */
  expr_list_t *list,       /* List to which to append. Might be NULL */
  id_list_t *columns,      /* List of names of LHS of the assignment */
  expr_t *expr            /* Vector expression to be appended. Might be NULL */
){
  DBSQL *db = parser->db;
  int n;
  int i;
  int i_first = list ? list->n_expr : 0;
  /* columns can only be NULL due to an OOM but an OOM will cause an
  ** exit prior to this routine being invoked */
  if( NEVER(columns==0) ) goto vector_append_error;
  if( expr==0 ) goto vector_append_error;

  /* If the RHS is a vector, then we can immediately check to see that
  ** the size of the RHS and LHS match.  But if the RHS is a SELECT,
  ** wildcards ("*") in the result set of the SELECT must be expanded before
  ** we can do the size check, so defer the size check until code generation.
  */
  if( expr->op!=TK_SELECT && columns->n_id!=(n=__expr_vector_size(expr)) ){
    __error_msg(parser, "%d columns assigned %d values",
                    columns->n_id, n);
    goto vector_append_error;
  }

  for(i=0; i<columns->n_id; i++){
    expr_t *sub_expr = __expr_for_vector_field(parser, expr, i, columns->n_id);
    assert( sub_expr!=0 || db->malloc_failed );
    if( sub_expr==0 ) continue;
    list = __expr_list_append(parser, list, sub_expr);
    if( list ){
      assert( list->n_expr==i_first+i+1 );
      list->a[list->n_expr-1].e_name = columns->a[i].name;
      columns->a[i].name = 0;
    }
  }

  if( !db->malloc_failed && expr->op==TK_SELECT && ALWAYS(list!=0) ){
    expr_t *first = list->a[i_first].expr;
    assert( first!=0 );
    assert( first->op==TK_SELECT_COLUMN );
    
    /* Store the SELECT statement in right so it will be deleted when
    ** __expr_list_delete() is called */
    first->right = expr;
    expr = 0;

    /* Remember the size of the LHS in i_table so that we can check that
    ** the RHS and LHS sizes match during code generation. */
    first->i_table = columns->n_id;
  }

vector_append_error:
  __expr_unmap_and_delete(parser, expr);
  __id_list_delete(db, columns);
  return list;
}

/*
** Set the sort order for the last element on the given expr_list_t.
*/
void __expr_list_set_sort_order(expr_list_t *p, int i_sort_order, int nulls){
  struct expr_list_item_t *item;
  if( p==0 ) return;
  assert( p->n_expr>0 );

  assert( DBSQL_SO_UNDEFINED<0 && DBSQL_SO_ASC==0 && DBSQL_SO_DESC>0 );
  assert( i_sort_order==DBSQL_SO_UNDEFINED
       || i_sort_order==DBSQL_SO_ASC
       || i_sort_order==DBSQL_SO_DESC
  );
  assert( nulls==DBSQL_SO_UNDEFINED
       || nulls==DBSQL_SO_ASC
       || nulls==DBSQL_SO_DESC
  );

  item = &p->a[p->n_expr-1];
  assert( item->fg.nulls==0 );
  if( i_sort_order==DBSQL_SO_UNDEFINED ){
    i_sort_order = DBSQL_SO_ASC;
  }
  item->fg.sort_flags = (u_int8_t)i_sort_order;

  if( nulls!=DBSQL_SO_UNDEFINED ){
    item->fg.nulls = 1;
    if( i_sort_order!=nulls ){
      item->fg.sort_flags |= KEYINFO_ORDER_BIGNULL;
    }
  }
}

/*
** Set the expr_list_t.a[].e_name element of the most recently added item
** on the expression list.
**
** list might be NULL following an OOM error.  But name should never be
** NULL.  If a memory allocation fails, the parser->db->malloc_failed flag
** is set.
*/
void __expr_list_set_name(
  parser_t *parser,          /* Parsing context */
  expr_list_t *list,        /* List to which to add the span. */
  const Token *name,     /* Name to be added */
  int dequote             /* True to cause the name to be dequoted */
){
  assert( list!=0 || parser->db->malloc_failed!=0 );
  assert( parser->parse_mode!=PARSE_MODE_UNMAP || dequote==0 );
  if( list ){
    struct expr_list_item_t *item;
    assert( list->n_expr>0 );
    item = &list->a[list->n_expr-1];
    assert( item->e_name==0 );
    assert( item->fg.e_name==ENAME_NAME );
    item->e_name = __db_str_n_dup(parser->db, name->z, name->n);
    if( dequote ){
      /* If dequote==0, then name->z does not point to part of a DDL
      ** statement handled by the parser. And so no token need be added
      ** to the token-map.  */
      __dequote(item->e_name);
      if( IN_RENAME_OBJECT ){
        __rename_token_map(parser, (const void*)item->e_name, name);
      }
    }
  }
}

/*
** Set the expr_list_t.a[].span element of the most recently added item
** on the expression list.
**
** list might be NULL following an OOM error.  But span should never be
** NULL.  If a memory allocation fails, the parser->db->malloc_failed flag
** is set.
*/
void __expr_list_set_span(
  parser_t *parser,          /* Parsing context */
  expr_list_t *list,        /* List to which to add the span. */
  const char *start,     /* Start of the span */
  const char *end        /* End of the span */
){
  DBSQL *db = parser->db;
  assert( list!=0 || db->malloc_failed!=0 );
  if( list ){
    struct expr_list_item_t *item = &list->a[list->n_expr-1];
    assert( list->n_expr>0 );
    if( item->e_name==0 ){
      item->e_name = __db_span_dup(db, start, end);
      item->fg.e_name = ENAME_SPAN;
    }
  }
}

/*
** If the expression list e_list contains more than i_limit elements,
** leave an error message in parser.
*/
void __expr_list_check_length(
  parser_t *parser,
  expr_list_t *e_list,
  const char *object
){
  int mx = parser->db->limits[DBSQL_LIMIT_COLUMN];
  testcase( e_list && e_list->n_expr==mx );
  testcase( e_list && e_list->n_expr==mx+1 );
  if( e_list && e_list->n_expr>mx ){
    __error_msg(parser, "too many columns in %s", object);
  }
}

/*
** Delete an entire expression list.
*/
static DBSQL_NOINLINE void __expr_list_delete_nn(DBSQL *db, expr_list_t *list){
  int i = list->n_expr;
  struct expr_list_item_t *item =  list->a;
  assert( list->n_expr>0 );
  assert( db!=0 );
  do{
    __expr_delete(db, item->expr);
    if( item->e_name ) __db_nn_free_nn(db, item->e_name);
    item++;
  }while( --i>0 );
  __db_nn_free_nn(db, list);
}
void __expr_list_delete(DBSQL *db, expr_list_t *list){
  if( list ) __expr_list_delete_nn(db, list);
}
void __expr_list_delete_generic(DBSQL *db, void *list){
  if( ALWAYS(list) ) __expr_list_delete_nn(db, (expr_list_t*)list);
}

/*
** Return the bitwise-OR of all expr_t.flags fields in the given
** expr_list_t.
*/
u_int32_t __expr_list_flags(const expr_list_t *list){
  int i;
  u_int32_t m = 0;
  assert( list!=0 );
  for(i=0; i<list->n_expr; i++){
     expr_t *expr = list->a[i].expr;
     assert( expr!=0 );
     m |= expr->flags;
  }
  return m;
}

/*
** This is a SELECT-node callback for the expression walker that
** always "fails".  By "fail" in this case, we mean set
** walker->code to zero and abort.
**
** This callback is used by multiple expression walkers.
*/
int __select_walk_fail(walker_t *walker, select_t *__not_used){
  UNUSED_PARAMETER(__not_used);
  walker->code = 0;
  return WRC_Abort;
}

/*
** Check the input string to see if it is "true" or "false" (in any case).
**
**       If the string is....           Return
**         "true"                         EP_IsTrue
**         "false"                        EP_IsFalse
**         anything else                  0
*/
u_int32_t __is_true_or_false(const char *in){
  if( __str_icmp(in, "true")==0  ) return EP_IsTrue;
  if( __str_icmp(in, "false")==0 ) return EP_IsFalse;
  return 0;
}


/*
** If the input expression is an ID with the name "true" or "false"
** then convert it into an TK_TRUEFALSE term.  Return non-zero if
** the conversion happened, and zero if the expression is unaltered.
*/
int __expr_id_to_true_false(expr_t *expr){
  u_int32_t v;
  assert( expr->op==TK_ID || expr->op==TK_STRING );
  if( !EXPR_HAS_PROPERTY(expr, EP_Quoted|EP_IntValue)
   && (v = __is_true_or_false(expr->u.token))!=0
  ){
    expr->op = TK_TRUEFALSE;
    EXPR_SET_PROPERTY(expr, v);
    return 1;
  }
  return 0;
}

/*
** The argument must be a TK_TRUEFALSE expr_t node.  Return 1 if it is TRUE
** and 0 if it is FALSE.
*/
int __expr_truth_value(const expr_t *expr){
  expr = __expr_skip_collate_and_likely((expr_t*)expr);
  assert( expr->op==TK_TRUEFALSE );
  assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
  assert( __str_icmp(expr->u.token,"true")==0
       || __str_icmp(expr->u.token,"false")==0 );
  return expr->u.token[4]==0;
}

/*
** If expr is an AND or OR expression, try to simplify it by eliminating
** terms that are always true or false.  Return the simplified expression.
** Or return the original expression if no simplification is possible.
**
** Examples:
**
**     (x<10) AND true                =>   (x<10)
**     (x<10) AND false               =>   false
**     (x<10) AND (y=22 OR false)     =>   (x<10) AND (y=22)
**     (x<10) AND (y=22 OR true)      =>   (x<10)
**     (y=22) OR true                 =>   true
*/
expr_t *__expr_simplified_and_or(expr_t *expr){
  assert( expr!=0 );
  if( expr->op==TK_AND || expr->op==TK_OR ){
    expr_t *right = __expr_simplified_and_or(expr->right);
    expr_t *left = __expr_simplified_and_or(expr->left);
    if( EXPR_ALWAYS_TRUE(left) || EXPR_ALWAYS_FALSE(right) ){
      expr = expr->op==TK_AND ? right : left;
    }else if( EXPR_ALWAYS_TRUE(right) || EXPR_ALWAYS_FALSE(left) ){
      expr = expr->op==TK_AND ? left : right;
    }
  }
  return expr;
}

/*
** Return true if it might be advantageous to compute the right operand
** of expression expr first, before the left operand.
**
** Normally the left operand is computed before the right operand.  But if
** the left operand contains a subquery and the right does not, then it
** might be more efficient to compute the right operand first.
*/
static int __expr_eval_rhs_first(expr_t *expr){
  if( EXPR_HAS_PROPERTY(expr->left, EP_Subquery)
   && !EXPR_HAS_PROPERTY(expr->right, EP_Subquery)
  ){
    return 1;
  }else{
    return 0;
  }
}

/*
** Compute the two operands of a binary operator.
**
** If either operand contains a subquery, then the code strives to
** compute the operand containing the subquery second.  If the other
** operand evalutes to NULL, then a jump is made.  The address of the
** __is_null operand that does this jump is returned.  The caller can use
** this to optimize the computation so as to avoid doing the potentially
** expensive subquery.
**
** If no optimization opportunities exist, return 0.
*/
static int __expr_compute_operands(
  parser_t *parser,     /* Parsing context */
  expr_t *expr,       /* The comparison expression */
  int *p_r1,          /* OUT: Register holding the left operand */
  int *p_r2,          /* OUT: Register holding the right operand */
  int *free1,       /* OUT: Temp register to free if not zero */
  int *free2        /* OUT: Another temp register to free if not zero */
){
  int addr_is_null;
  int r1, r2;
  vdbe_t *v = parser->vdbe;

  assert( v!=0 );
  /*
  ** If the left operand contains a (possibly expensive) subquery and the
  ** right operand does not and the right operation might be NULL,
  ** then compute the right operand first and do an __is_null jump if the
  ** right operand evalutes to NULL.
  */
  if( __expr_eval_rhs_first(expr) && __expr_can_be_null(expr->right) ){
    r2 = __expr_code_temp(parser, expr->right, free2);
    addr_is_null = __vdbe_add_op1(v, OP_IsNull, r2);
    VDBE_COMMENT((v, "skip left operand"));
    VDBE_COVERAGE(v);
  }else{
    r2 = 0; /* Silence a false-positive uninit-var warning in MSVC */
    addr_is_null = 0;
  }
  r1 = __expr_code_temp(parser, expr->left, free1);
  if( addr_is_null==0 ){
    /*
    ** If the right operand contains a subquery and the left operand does not
    ** and the left operand might be NULL, then do an __is_null check
    ** check on the left operand before computing the right operand.
    */
    if( EXPR_HAS_PROPERTY(expr->right, EP_Subquery)
     && __expr_can_be_null(expr->left)
    ){
      addr_is_null = __vdbe_add_op1(v, OP_IsNull, r1);
      VDBE_COMMENT((v, "skip right operand"));
      VDBE_COVERAGE(v);
    }
    r2 = __expr_code_temp(parser, expr->right, free2);
  }
  *p_r1 = r1;
  *p_r2 = r2;
  return addr_is_null;
}

/*
** expr is a TK_FUNCTION node.  Try to determine whether or not the
** function is a constant function.  A function is constant if all of
** the following are true:
**
**    (1)  It is a scalar function (not an aggregate or window function)
**    (2)  It has either the DBSQL_FUNC_CONSTANT or DBSQL_FUNC_SLOCHNG
**         property.
**    (3)  All of its arguments are constants
**
** This routine sets walker->code to 0 if expr is not a constant.
** It makes no changes to walker->code if expr is constant.  In
** every case, it returns WRC_Abort.
**
** Called as a service subroutine from __expr_node_is_constant().
*/
static DBSQL_NOINLINE int __expr_node_is_constant_function(
  walker_t *walker,
  expr_t *expr
){
  int n;             /* Number of arguments */
  expr_list_t *list;   /* List of arguments */
  func_def_t *def;     /* The function */
  DBSQL *db;       /* The database */

  assert( expr->op==TK_FUNCTION );
  if( EXPR_HAS_PROPERTY(expr, EP_TokenOnly)
   || (list = expr->x.list)==0
  ){;
    n = 0;
  }else{
    n = list->n_expr;
    __walk_expr_list(walker, list);
    if( walker->code==0 ) return WRC_Abort;
  }
  db = walker->parser->db;
  def = __find_function(db, expr->u.token, n, ENC(db), 0);
  if( def==0
   || def->finalize!=0
   || (def->func_flags & (DBSQL_FUNC_CONSTANT|DBSQL_FUNC_SLOCHNG))==0
   || EXPR_HAS_PROPERTY(expr, EP_WinFunc)
  ){
    walker->code = 0;
    return WRC_Abort;
  }
  return WRC_Prune;
}


/*
** These routines are walker_t callbacks used to check expressions to
** see if they are "constant" for some definition of constant.  The
** walker_t.code value determines the type of "constant" we are looking
** for.
**
** These callback routines are used to implement the following:
**
**     __expr_is_constant()                  walker->code==1
**     __expr_is_constant_not_join()           walker->code==2
**     __expr_is_table_constant()             walker->code==3
**     __expr_is_constant_or_function()        walker->code==4 or 5
**
** In all cases, the callbacks set walker_t.code=0 and abort if the expression
** is found to not be a constant.
**
** The __expr_is_constant_or_function() is used for evaluating DEFAULT
** expressions in a CREATE TABLE statement.  The walker_t.code value is 5
** when parsing an existing schema out of the dbsql_schema table and 4
** when processing a new CREATE TABLE statement.  A bound parameter raises
** an error for new statements, but is silently converted
** to NULL for existing schemas.  This allows dbsql_schema tables that
** contain a bound parameter because they were generated by older versions
** of dbsql to be parsed by newer versions of dbsql without raising a
** malformed schema error.
*/
static int __expr_node_is_constant(walker_t *walker, expr_t *expr){
  assert( walker->code>0 );

  /* If walker->code is 2 then any term of the expression that comes from
  ** the ON or USING clauses of an outer join disqualifies the expression
  ** from being considered constant. */
  if( walker->code==2 && EXPR_HAS_PROPERTY(expr, EP_OuterON) ){
    walker->code = 0;
    return WRC_Abort;
  }

  switch( expr->op ){
    /* Consider functions to be constant if all their arguments are constant
    ** and either walker->code==4 or 5 or the function has the
    ** DBSQL_FUNC_CONST flag. */
    case TK_FUNCTION:
      if( (walker->code>=4 || EXPR_HAS_PROPERTY(expr,EP_ConstFunc))
       && !EXPR_HAS_PROPERTY(expr, EP_WinFunc)
      ){
        if( walker->code==5 ) EXPR_SET_PROPERTY(expr, EP_FromDDL);
        return WRC_Continue;
      }else if( walker->parser ){
        return __expr_node_is_constant_function(walker, expr);
      }else{
        walker->code = 0;
        return WRC_Abort;
      }
    case TK_ID:
      /* Convert "true" or "false" in a DEFAULT clause into the
      ** appropriate TK_TRUEFALSE operator */
      if( __expr_id_to_true_false(expr) ){
        return WRC_Prune;
      }
      /* no break */ deliberate_fall_through
    case TK_COLUMN:
    case TK_AGG_FUNCTION:
    case TK_AGG_COLUMN:
      testcase( expr->op==TK_ID );
      testcase( expr->op==TK_COLUMN );
      testcase( expr->op==TK_AGG_FUNCTION );
      testcase( expr->op==TK_AGG_COLUMN );
      if( EXPR_HAS_PROPERTY(expr, EP_FixedCol) && walker->code!=2 ){
        return WRC_Continue;
      }
      if( walker->code==3 && expr->i_table==walker->u.i_cur ){
        return WRC_Continue;
      }
      /* no break */ deliberate_fall_through
    case TK_IF_NULL_ROW:
    case TK_REGISTER:
    case TK_DOT:
    case TK_RAISE:
      testcase( expr->op==TK_REGISTER );
      testcase( expr->op==TK_IF_NULL_ROW );
      testcase( expr->op==TK_DOT );
      testcase( expr->op==TK_RAISE );
      walker->code = 0;
      return WRC_Abort;
    case TK_VARIABLE:
      if( walker->code==5 ){
        /* Silently convert bound parameters that appear inside of CREATE
        ** statements into a NULL when parsing the CREATE statement text out
        ** of the dbsql_schema table */
        expr->op = TK_NULL;
      }else if( walker->code==4 ){
        /* A bound parameter in a CREATE statement that originates from
        ** dbsql_prepare() causes an error */
        walker->code = 0;
        return WRC_Abort;
      }
      /* no break */ deliberate_fall_through
    default:
      testcase( expr->op==TK_SELECT ); /* __select_walk_fail() disallows */
      testcase( expr->op==TK_EXISTS ); /* __select_walk_fail() disallows */
      return WRC_Continue;
  }
}
static int __expr_is_const(parser_t *parser, expr_t *p, int init_flag){
  walker_t w;
  w.code = init_flag;
  w.parser = parser;
  w.expr_callback = __expr_node_is_constant;
  w.select_callback = __select_walk_fail;
#ifdef DBSQL_DEBUG
  w.select_callback2 = __select_walk_assert2;
#endif
  __walk_expr(&w, p);
  return w.code;
}

/*
** Walk an expression tree.  Return non-zero if the expression is constant
** or return zero if the expression involves variables or function calls.
**
** For the purposes of this function, a double-quoted string (ex: "abc")
** is considered a variable but a single-quoted string (ex: 'abc') is
** a constant.
**
** The parser parameter may be NULL.  But if it is NULL, there is no way
** to determine if function calls are constant or not, and hence all
** function calls will be considered to be non-constant.  If parser is
** not NULL, then a function call might be constant, depending on the
** function and on its parameters.
*/
int __expr_is_constant(parser_t *parser, expr_t *p){
  return __expr_is_const(parser, p, 1);
}

/*
** Walk an expression tree.  Return non-zero if
**
**   (1) the expression is constant, and
**   (2) the expression does originate in the ON or USING clause
**       of a LEFT JOIN, and
**   (3) the expression does not contain any EP_FixedCol TK_COLUMN
**       operands created by the constant propagation optimization.
**
** When this routine returns true, it indicates that the expression
** can be added to the parser->const_expr list and evaluated once when
** the prepared statement starts up.  See __expr_code_run_just_once().
*/
static int __expr_is_constant_not_join(parser_t *parser, expr_t *p){
  return __expr_is_const(parser, p, 2);
}

/*
** This routine examines sub-SELECT statements as an expression is being
** walked as part of __expr_is_table_constant().  Sub-SELECTs are considered
** constant as long as they are uncorrelated - meaning that they do not
** contain any terms from outer contexts.
*/
static int __expr_select_walk_table_constant(walker_t *walker, select_t *p_select){
  assert( p_select!=0 );
  assert( walker->code==3 || walker->code==0 );
  if( (p_select->sel_flags & SF_Correlated)!=0 ){
    walker->code = 0;
    return WRC_Abort;
  }
  return WRC_Prune;
}

/*
** Walk an expression tree.  Return non-zero if the expression is constant
** for any single row of the table with cursor i_cur.  In other words, the
** expression must not refer to any non-deterministic function nor any
** table other than i_cur.
**
** Consider uncorrelated subqueries to be constants if the allow_subq
** parameter is true.
*/
static int __expr_is_table_constant(expr_t *p, int i_cur, int allow_subq){
  walker_t w;
  w.code = 3;
  w.parser = 0;
  w.expr_callback = __expr_node_is_constant;
  if( allow_subq ){
    w.select_callback = __expr_select_walk_table_constant;
  }else{
    w.select_callback = __select_walk_fail;
#ifdef DBSQL_DEBUG
    w.select_callback2 = __select_walk_assert2;
#endif
  }
  w.u.i_cur = i_cur;
  __walk_expr(&w, p);
  return w.code;
}

/*
** Check expr to see if it is an constraint on the single data source
** src = &src_list->a[i_src].  In other words, check to see if expr
** constrains src but does not depend on any other tables or data
** sources anywhere else in the query.  Return true (non-zero) if expr
** is a constraint on src only.
**
** This is an optimization.  False negatives will perhaps cause slower
** queries, but false positives will yield incorrect answers.  So when in
** doubt, return 0.
**
** To be an single-source constraint, the following must be true:
**
**   (1)  expr cannot refer to any table other than src->i_cursor.
**
**   (2a) expr cannot use subqueries unless the allow_subq parameter is
**        true and the subquery is non-correlated
**
**   (2b) expr cannot use non-deterministic functions.
**
**   (3)  src cannot be part of the left operand for a RIGHT JOIN.
**        (Is there some way to relax this constraint?)
**
**   (4)  If src is the right operand of a LEFT JOIN, then...
**         (4a)  expr must come from an ON clause..
**         (4b)  and specifically the ON clause associated with the LEFT JOIN.
**
**   (5)  If src is the right operand of a LEFT JOIN or the left
**        operand of a RIGHT JOIN, then expr must be from the WHERE
**        clause, not an ON clause.
**
**   (6) Either:
**
**       (6a) expr does not originate in an ON or USING clause, or
**
**       (6b) The ON or USING clause from which expr is derived is
**            not to the left of a RIGHT JOIN (or FULL JOIN).
**
**       Without this restriction, accepting expr as a single-table
**       constraint might move the the ON/USING filter expression
**       from the left side of a RIGHT JOIN over to the right side,
**       which leads to incorrect answers.  See also restriction (9)
**       on push-down.
*/
int __expr_is_single_table_constraint(
  expr_t *expr,                 /* The constraint */
  const src_list_t *src_list,     /* Complete FROM clause */
  int i_src,                    /* Which element of src_list to use */
  int allow_subq               /* Allow non-correlated subqueries */
){
  const src_item_t *src = &src_list->a[i_src];
  if( src->fg.jointype & JT_LTORJ ){
    return 0;  /* rule (3) */
  }
  if( src->fg.jointype & JT_LEFT ){
    if( !EXPR_HAS_PROPERTY(expr, EP_OuterON) ) return 0;   /* rule (4a) */
    if( expr->w.i_join!=src->i_cursor ) return 0;         /* rule (4b) */
  }else{
    if( EXPR_HAS_PROPERTY(expr, EP_OuterON) ) return 0;    /* rule (5) */
  }
  if( EXPR_HAS_PROPERTY(expr, EP_OuterON|EP_InnerON)  /* (6a) */
   && (src_list->a[0].fg.jointype & JT_LTORJ)!=0     /* Fast pre-test of (6b) */
  ){
    int jj;
    for(jj=0; jj<i_src; jj++){
      if( expr->w.i_join==src_list->a[jj].i_cursor ){
        if( (src_list->a[jj].fg.jointype & JT_LTORJ)!=0 ){
          return 0;  /* restriction (6) */
        }
        break;
      }
    }
  }
  /* Rules (1), (2a), and (2b) handled by the following: */
  return __expr_is_table_constant(expr, src->i_cursor, allow_subq);
}


/*
** __walk_expr() callback used by __expr_is_constant_or_group_by().
*/
static int __expr_node_is_constant_or_group_by(walker_t *walker, expr_t *expr){
  expr_list_t *group_by = walker->u.group_by;
  int i;

  /* Check if expr is identical to any GROUP BY term. If so, consider
  ** it constant.  */
  for(i=0; i<group_by->n_expr; i++){
    expr_t *p = group_by->a[i].expr;
    if( __expr_compare(0, expr, p, -1)<2 ){
      coll_seq_t *coll = __expr_nn_coll_seq(walker->parser, p);
      if( __is_binary(coll) ){
        return WRC_Prune;
      }
    }
  }

  /* Check if expr is a sub-select. If so, consider it variable. */
  if( EXPR_USE_X_SELECT(expr) ){
    walker->code = 0;
    return WRC_Abort;
  }

  return __expr_node_is_constant(walker, expr);
}

/*
** Walk the expression tree passed as the first argument. Return non-zero
** if the expression consists entirely of constants or copies of terms
** in group_by that sort with the BINARY collation sequence.
**
** This routine is used to determine if a term of the HAVING clause can
** be promoted into the WHERE clause.  In order for such a promotion to work,
** the value of the HAVING clause term must be the same for all members of
** a "group".  The requirement that the GROUP BY term must be BINARY
** assumes that no other collating sequence will have a finer-grained
** grouping than binary.  In other words (A=B COLLATE binary) implies
** A=B in every other collating sequence.  The requirement that the
** GROUP BY be BINARY is stricter than necessary.  It would also work
** to promote HAVING clauses that use the same alternative collating
** sequence as the GROUP BY term, but that is much harder to check,
** alternative collating sequences are uncommon, and this is only an
** optimization, so we take the easy way out and simply require the
** GROUP BY to use the BINARY collating sequence.
*/
int __expr_is_constant_or_group_by(parser_t *parser, expr_t *p, expr_list_t *group_by){
  walker_t w;
  w.code = 1;
  w.expr_callback = __expr_node_is_constant_or_group_by;
  w.select_callback = 0;
  w.u.group_by = group_by;
  w.parser = parser;
  __walk_expr(&w, p);
  return w.code;
}

/*
** Walk an expression tree for the DEFAULT field of a column definition
** in a CREATE TABLE statement.  Return non-zero if the expression is
** acceptable for use as a DEFAULT.  That is to say, return non-zero if
** the expression is constant or a function call with constant arguments.
** Return and 0 if there are any variables.
**
** is_init is true when parsing from dbsql_schema.  is_init is false when
** processing a new CREATE TABLE statement.  When is_init is true, parameters
** (such as ? or $abc) in the expression are converted into NULL.  When
** is_init is false, parameters raise an error.  Parameters should not be
** allowed in a CREATE TABLE statement, but some legacy versions of dbsql
** allowed it, so we need to support it when reading dbsql_schema for
** backwards compatibility.
**
** If is_init is true, set EP_FromDDL on every TK_FUNCTION node.
**
** For the purposes of this function, a double-quoted string (ex: "abc")
** is considered a variable but a single-quoted string (ex: 'abc') is
** a constant.
*/
int __expr_is_constant_or_function(expr_t *p, u_int8_t is_init){
  assert( is_init==0 || is_init==1 );
  return __expr_is_const(0, p, 4+is_init);
}

#ifdef DBSQL_ENABLE_CURSOR_HINTS
/*
** Walk an expression tree.  Return 1 if the expression contains a
** subquery of some kind.  Return 0 if there are no subqueries.
*/
int __expr_contains_subquery(expr_t *p){
  walker_t w;
  w.code = 1;
  w.expr_callback = __expr_walk_noop;
  w.select_callback = __select_walk_fail;
#ifdef DBSQL_DEBUG
  w.select_callback2 = __select_walk_assert2;
#endif
  __walk_expr(&w, p);
  return w.code==0;
}
#endif

/*
** If the expression p codes a constant integer that is small enough
** to fit in a 32-bit integer, return 1 and put the value of the integer
** in *value.  If the expression is not an integer or if it is too big
** to fit in a signed 32-bit integer, return 0 and leave *value unchanged.
**
** If the parser pointer is provided, then allow the expression p to be
** a parameter (TK_VARIABLE) that is bound to an integer.
** But if parser is NULL, then p must be a pure integer literal.
*/
int __expr_is_integer(const expr_t *p, int *value, parser_t *parser){
  int rc = 0;
  if( NEVER(p==0) ) return 0;  /* Used to only happen following on OOM */

  /* If an expression is an integer literal that fits in a signed 32-bit
  ** integer, then the EP_IntValue flag will have already been set */
  assert( p->op!=TK_INTEGER || (p->flags & EP_IntValue)!=0
           || __get_int32(p->u.token, &rc)==0 );

  if( p->flags & EP_IntValue ){
    *value = p->u.i_value;
    return 1;
  }
  switch( p->op ){
    case TK_UPLUS: {
      rc = __expr_is_integer(p->left, value, 0);
      break;
    }
    case TK_UMINUS: {
      int v = 0;
      if( __expr_is_integer(p->left, &v, 0) ){
        assert( ((unsigned int)v)!=0x80000000 );
        *value = -v;
        rc = 1;
      }
      break;
    }
    case TK_VARIABLE: {
      dbsql_value_t *val;
      if( parser==0 ) break;
      if( NEVER(parser->vdbe==0) ) break;
      if( (parser->db->flags & DBSQL_EnableQPSG)!=0 ) break;
      __vdbe_set_varmask(parser->vdbe, p->i_column);
      val = __vdbe_get_bound_value(parser->reprepare, p->i_column,
                                      DBSQL_AFF_BLOB);
      if( val ){
        if( dbsql_value_type(val)==DBSQL_INTEGER ){
          int64_t vv = dbsql_value_int64(val);
          if( vv == (vv & 0x7fffffff) ){ /* non-negative numbers only */
            *value = (int)vv;
            rc = 1;
          }
        }
        __value_free(val);
      }
      break;
    }
    default: break;
  }
  return rc;
}

/*
** Return FALSE if there is no chance that the expression can be NULL.
**
** If the expression might be NULL or if the expression is too complex
** to tell return TRUE. 
**
** This routine is used as an optimization, to skip OP_IsNull opcodes
** when we know that a value cannot be NULL.  Hence, a false positive
** (returning TRUE when in fact the expression can never be NULL) might
** be a small performance hit but is otherwise harmless.  On the other
** hand, a false negative (returning FALSE when the result could be NULL)
** will likely result in an incorrect answer.  So when in doubt, return
** TRUE.
*/
int __expr_can_be_null(const expr_t *p){
  u_int8_t op;
  assert( p!=0 );
  while( p->op==TK_UPLUS || p->op==TK_UMINUS ){
    p = p->left;
    assert( p!=0 );
  }
  op = p->op;
  if( op==TK_REGISTER ) op = p->op2;
  switch( op ){
    case TK_INTEGER:
    case TK_STRING:
    case TK_FLOAT:
    case TK_BLOB:
      return 0;
    case TK_COLUMN:
      assert( EXPR_USE_Y_TAB(p) );
      return EXPR_HAS_PROPERTY(p, EP_CanBeNull)
          || NEVER(p->y.tab==0) /* Reference to column of index on expr */
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
          || (p->i_column==XN_ROWID && IsView(p->y.tab))
#endif
          || (p->i_column>=0
              && p->y.tab->cols!=0 /* Possible due to prior error */
              && ALWAYS(p->i_column<p->y.tab->n_col)
              && p->y.tab->cols[p->i_column].not_null==0);
    default:
      return 1;
  }
}

/*
** Return TRUE if the given expression is a constant which would be
** unchanged by OP_Affinity with the affinity given in the second
** argument.
**
** This routine is used to determine if the OP_Affinity operation
** can be omitted.  When in doubt return FALSE.  A false negative
** is harmless.  A false positive, however, can result in the wrong
** answer.
*/
int __expr_needs_no_affinity_change(const expr_t *p, char aff){
  u_int8_t op;
  int unary_minus = 0;
  if( aff==DBSQL_AFF_BLOB ) return 1;
  while( p->op==TK_UPLUS || p->op==TK_UMINUS ){
    if( p->op==TK_UMINUS ) unary_minus = 1;
    p = p->left;
  }
  op = p->op;
  if( op==TK_REGISTER ) op = p->op2;
  switch( op ){
    case TK_INTEGER: {
      return aff>=DBSQL_AFF_NUMERIC;
    }
    case TK_FLOAT: {
      return aff>=DBSQL_AFF_NUMERIC;
    }
    case TK_STRING: {
      return !unary_minus && aff==DBSQL_AFF_TEXT;
    }
    case TK_BLOB: {
      return !unary_minus;
    }
    case TK_COLUMN: {
      assert( p->i_table>=0 );  /* p cannot be part of a CHECK constraint */
      return aff>=DBSQL_AFF_NUMERIC && p->i_column<0;
    }
    default: {
      return 0;
    }
  }
}

/*
** Return TRUE if the given string is a row-id column name.
*/
int __is_rowid(const char *z){
  if( __str_icmp(z, "_ROWID_")==0 ) return 1;
  if( __str_icmp(z, "ROWID")==0 ) return 1;
  if( __str_icmp(z, "OID")==0 ) return 1;
  return 0;
}

/*
** Return a pointer to a buffer containing a usable rowid alias for table
** tab. An alias is usable if there is not an explicit user-defined column 
** of the same name.
*/
const char *__rowid_alias(table_t *tab){
  const char *opts[] = {"_ROWID_", "ROWID", "OID"};
  int ii;
  assert( VISIBLE_ROWID(tab) );
  for(ii=0; ii<ARRAY_SIZE(opts); ii++){
    if( __column_index(tab, opts[ii])<0 ) return opts[ii];
  }
  return 0;
}

/*
** x is the RHS of an IN operator.  If x is a SELECT statement
** that can be simplified to a direct table access, then return
** a pointer to the SELECT statement.  If x is not a SELECT statement,
** or if the SELECT statement needs to be materialized into a transient
** table, then return NULL.
*/
#ifndef DBSQL_OMIT_SUBQUERY
static select_t *__is_candidate_for_in_opt(const expr_t *x){
  select_t *p;
  src_list_t *src;
  expr_list_t *e_list;
  table_t *tab;
  int i;
  if( !EXPR_USE_X_SELECT(x) ) return 0;                 /* Not a subquery */
  if( EXPR_HAS_PROPERTY(x, EP_VarSelect)  ) return 0;  /* Correlated subq */
  p = x->x.p_select;
  if( p->prior ) return 0;              /* Not a compound SELECT */
  if( p->sel_flags & (SF_Distinct|SF_Aggregate) ){
    testcase( (p->sel_flags & (SF_Distinct|SF_Aggregate))==SF_Distinct );
    testcase( (p->sel_flags & (SF_Distinct|SF_Aggregate))==SF_Aggregate );
    return 0; /* No DISTINCT keyword and no aggregate functions */
  }
  assert( p->group_by==0 );              /* Has no GROUP BY clause */
  if( p->limit ) return 0;              /* Has no LIMIT clause */
  if( p->where ) return 0;              /* Has no WHERE clause */
  src = p->src;
  assert( src!=0 );
  if( src->n_src!=1 ) return 0;          /* Single term in FROM clause */
  if( src->a[0].fg.is_subquery) return 0;/* FROM is not a subquery or view */
  tab = src->a[0].s_tab;
  assert( tab!=0 );
  assert( !IsView(tab)  );              /* FROM clause is not a view */
  if( IS_VIRTUAL(tab) ) return 0;        /* FROM clause not a virtual table */
  e_list = p->e_list;
  assert( e_list!=0 );
  /* All SELECT results must be columns. */
  for(i=0; i<e_list->n_expr; i++){
    expr_t *res = e_list->a[i].expr;
    if( res->op!=TK_COLUMN ) return 0;
    assert( res->i_table==src->a[0].i_cursor );  /* Not a correlated subquery */
  }
  return p;
}
#endif /* DBSQL_OMIT_SUBQUERY */

#ifndef DBSQL_OMIT_SUBQUERY
/*
** Generate code that checks the left-most column of index table i_cur to see if
** it contains any NULL entries.  Cause the register at reg_has_null to be set
** to a non-NULL value if i_cur contains no NULLs.  Cause register reg_has_null
** to be set to NULL if i_cur contains one or more NULL values.
*/
static void __set_has_null_flag(vdbe_t *v, int i_cur, int reg_has_null){
  int addr1;
  __vdbe_add_op2(v, OP_Integer, 0, reg_has_null);
  addr1 = __vdbe_add_op1(v, OP_Rewind, i_cur); VDBE_COVERAGE(v);
  __vdbe_add_op3(v, OP_Column, i_cur, 0, reg_has_null);
  __vdbe_change_p5(v, OPFLAG_TYPEOFARG);
  VDBE_COMMENT((v, "first_entry_in(%d)", i_cur));
  __vdbe_jump_here(v, addr1);
}
#endif


#ifndef DBSQL_OMIT_SUBQUERY
/*
** The argument is an IN operator with a list (not a subquery) on the
** right-hand side.  Return TRUE if that list is constant.
*/
static int __in_rhs_is_constant(parser_t *parser, expr_t *in){
  expr_t *lhs;
  int res;
  assert( !EXPR_HAS_PROPERTY(in, EP_xIsSelect) );
  lhs = in->left;
  in->left = 0;
  res = __expr_is_constant(parser, in);
  in->left = lhs;
  return res;
}
#endif

/*
** This function is used by the implementation of the IN (...) operator.
** The x parameter is the expression on the RHS of the IN operator, which
** might be either a list of expressions or a subquery.
**
** The job of this routine is to find or create a b-tree object that can
** be used either to test for membership in the RHS set or to iterate through
** all members of the RHS set, skipping duplicates.
**
** A cursor is opened on the b-tree object that is the RHS of the IN operator
** and the *pi_tab parameter is set to the index of that cursor.
**
** The returned value of this function indicates the b-tree type, as follows:
**
**   IN_INDEX_ROWID      - The cursor was opened on a database table.
**   IN_INDEX_INDEX_ASC  - The cursor was opened on an ascending index.
**   IN_INDEX_INDEX_DESC - The cursor was opened on a descending index.
**   IN_INDEX_EPH        - The cursor was opened on a specially created and
**                         populated ephemeral table.
**   IN_INDEX_NOOP       - No cursor was allocated.  The IN operator must be
**                         implemented as a sequence of comparisons.
**
** An existing b-tree might be used if the RHS expression x is a simple
** subquery such as:
**
**     SELECT <column1>, <column2>... FROM <table>
**
** If the RHS of the IN operator is a list or a more complex subquery, then
** an ephemeral table might need to be generated from the RHS and then
** x->i_table made to point to the ephemeral table instead of an
** existing table.  In this case, the creation and initialization of the
** ephemeral table might be put inside of a subroutine, the EP_Subrtn flag
** will be set on x and the x->y.sub fields will be set to show where
** the subroutine is coded.
**
** The in_flags parameter must contain, at a minimum, one of the bits
** IN_INDEX_MEMBERSHIP or IN_INDEX_LOOP but not both.  If in_flags contains
** IN_INDEX_MEMBERSHIP, then the generated table will be used for a fast
** membership test.  When the IN_INDEX_LOOP bit is set, the IN index will
** be used to loop over all values of the RHS of the IN operator.
**
** When IN_INDEX_LOOP is used (and the b-tree will be used to iterate
** through the set members) then the b-tree must not contain duplicates.
** An ephemeral table will be created unless the selected columns are guaranteed
** to be unique - either because it is an INTEGER PRIMARY KEY or due to
** a UNIQUE constraint or index.
**
** When IN_INDEX_MEMBERSHIP is used (and the b-tree will be used
** for fast set membership tests) then an ephemeral table must
** be used unless <columns> is a single INTEGER PRIMARY KEY column or an
** index can be found with the specified <columns> as its left-most.
**
** If the IN_INDEX_NOOP_OK and IN_INDEX_MEMBERSHIP are both set and
** if the RHS of the IN operator is a list (not a subquery) then this
** routine might decide that creating an ephemeral b-tree for membership
** testing is too expensive and return IN_INDEX_NOOP.  In that case, the
** calling routine should implement the IN operator using a sequence
** of Eq or Ne comparison operations.
**
** When the b-tree is being used for membership tests, the calling function
** might need to know whether or not the RHS side of the IN operator
** contains a NULL.  If pr_rhs_has_null is not a NULL pointer and
** if there is any chance that the (...) might contain a NULL value at
** runtime, then a register is allocated and the register number written
** to *pr_rhs_has_null. If there is no chance that the (...) contains a
** NULL value, then *pr_rhs_has_null is left unchanged.
**
** If a register is allocated and its location stored in *pr_rhs_has_null, then
** the value in that register will be NULL if the b-tree contains one or more
** NULL values, and it will be some non-NULL value if the b-tree contains no
** NULL values.
**
** If the maps parameter is not NULL, it must point to an array containing
** one element for each column returned by the SELECT statement on the RHS
** of the IN(...) operator. The i'th entry of the array is populated with the
** offset of the index column that matches the i'th column returned by the
** SELECT. For example, if the expression and selected index are:
**
**   (?,?,?) IN (SELECT a, b, c FROM t1)
**   CREATE INDEX i1 ON t1(b, c, a);
**
** then maps[] is populated with {2, 0, 1}.
*/
#ifndef DBSQL_OMIT_SUBQUERY
int __find_in_index(
  parser_t *parser,             /* Parsing context */
  expr_t *x,                  /* The IN expression */
  u_int32_t in_flags,               /* IN_INDEX_LOOP, _MEMBERSHIP, and/or _NOOP_OK */
  int *pr_rhs_has_null,         /* Register holding NULL status.  See notes */
  int *maps,                /* Mapping from index_t fields to RHS fields */
  int *pi_tab                 /* OUT: index to use */
){
  select_t *p;                            /* SELECT to the right of IN operator */
  int type = 0;                        /* Type of RHS table. IN_INDEX_* */
  int i_tab;                             /* Cursor of the RHS table */
  int must_be_unique;                     /* True if RHS must be unique */
  vdbe_t *v = __get_vdbe(parser);     /* Virtual machine being coded */

  assert( x->op==TK_IN );
  must_be_unique = (in_flags & IN_INDEX_LOOP)!=0;
  i_tab = parser->n_tab++;

  /* If the RHS of this IN(...) operator is a SELECT, and if it matters
  ** whether or not the SELECT result contains NULL values, check whether
  ** or not NULL is actually possible (it may not be, for example, due
  ** to NOT NULL constraints in the schema). If no NULL values are possible,
  ** set pr_rhs_has_null to 0 before continuing.  */
  if( pr_rhs_has_null && EXPR_USE_X_SELECT(x) ){
    int i;
    expr_list_t *e_list = x->x.p_select->e_list;
    for(i=0; i<e_list->n_expr; i++){
      if( __expr_can_be_null(e_list->a[i].expr) ) break;
    }
    if( i==e_list->n_expr ){
      pr_rhs_has_null = 0;
    }
  }

  /* Check to see if an existing table or index can be used to
  ** satisfy the query.  This is preferable to generating a new
  ** ephemeral table.  */
  if( parser->n_err==0 && (p = __is_candidate_for_in_opt(x))!=0 ){
    DBSQL *db = parser->db;              /* Database connection */
    table_t *tab;                           /* table_t <table>. */
    int i_db;                               /* Database idx for tab */
    expr_list_t *e_list = p->e_list;
    int n_expr = e_list->n_expr;

    assert( p->e_list!=0 );             /* Because of __is_candidate_for_in_opt(p) */
    assert( p->e_list->a[0].expr!=0 ); /* Because of __is_candidate_for_in_opt(p) */
    assert( p->src!=0 );               /* Because of __is_candidate_for_in_opt(p) */
    tab = p->src->a[0].s_tab;

    /* Code an OP_Transaction and OP_TableLock for <table>. */
    i_db = __schema_to_index(db, tab->schema);
    assert( i_db>=0 && i_db<DBSQL_MAX_DB );
    __code_verify_schema(parser, i_db);
    __table_lock(parser, i_db, tab->tnum, 0, tab->name);

    assert(v);  /* __get_vdbe() has always been previously called */
    if( n_expr==1 && e_list->a[0].expr->i_column<0 ){
      /* The "x IN (SELECT rowid FROM table)" case */
      int i_addr = __vdbe_add_op0(v, OP_Once);
      VDBE_COVERAGE(v);

      __open_table(parser, i_tab, i_db, tab, OP_OpenRead);
      type = IN_INDEX_ROWID;
      EXPLAIN_QUERY_PLAN((parser, 0,
            "USING ROWID SEARCH ON TABLE %s FOR IN-OPERATOR",tab->name));
      __vdbe_jump_here(v, i_addr);
    }else{
      index_t *idx;                         /* Iterator variable */
      int affinity_ok = 1;
      int i;

      /* Check that the affinity that will be used to perform each
      ** comparison is the same as the affinity of each column in table
      ** on the RHS of the IN operator.  If it not, it is not possible to
      ** use any index of the RHS table.  */
      for(i=0; i<n_expr && affinity_ok; i++){
        expr_t *lhs = __vector_field_subexpr(x->left, i);
        int i_col = e_list->a[i].expr->i_column;
        char idxaff = __table_column_affinity(tab,i_col); /* RHS table */
        char cmpaff = __compare_affinity(lhs, idxaff);
        testcase( cmpaff==DBSQL_AFF_BLOB );
        testcase( cmpaff==DBSQL_AFF_TEXT );
        switch( cmpaff ){
          case DBSQL_AFF_BLOB:
            break;
          case DBSQL_AFF_TEXT:
            /* __compare_affinity() only returns TEXT if one side or the
            ** other has no affinity and the other side is TEXT.  Hence,
            ** the only way for cmpaff to be TEXT is for idxaff to be TEXT
            ** and for the term on the LHS of the IN to have no affinity. */
            assert( idxaff==DBSQL_AFF_TEXT );
            break;
          default:
            affinity_ok = __is_numeric_affinity(idxaff);
        }
      }

      if( affinity_ok ){
        /* Search for an existing index that will work for this IN operator */
        for(idx=tab->p_index; idx && type==0; idx=idx->next){
          Bitmask col_used;      /* Columns of the index used */
          Bitmask m_col;         /* Mask for the current column */
          if( idx->n_column<n_expr ) continue;
          if( idx->part_idx_where!=0 ) continue;
          /* Maximum n_column is BMS-2, not BMS-1, so that we can compute
          ** BITMASK(n_expr) without overflowing */
          testcase( idx->n_column==BMS-2 );
          testcase( idx->n_column==BMS-1 );
          if( idx->n_column>=BMS-1 ) continue;
          if( must_be_unique ){
            if( idx->n_key_col>n_expr
             ||(idx->n_column>n_expr && !IS_UNIQUE_INDEX(idx))
            ){
              continue;  /* This index is not unique over the IN RHS columns */
            }
          }
 
          col_used = 0;   /* Columns of index used so far */
          for(i=0; i<n_expr; i++){
            expr_t *lhs = __vector_field_subexpr(x->left, i);
            expr_t *rhs = e_list->a[i].expr;
            coll_seq_t *req = __binary_compare_coll_seq(parser, lhs, rhs);
            int j;
 
            for(j=0; j<n_expr; j++){
              if( idx->columns[j]!=rhs->i_column ) continue;
              assert( idx->colls[j] );
              if( req!=0 && __str_icmp(req->name, idx->colls[j])!=0 ){
                continue;
              }
              break;
            }
            if( j==n_expr ) break;
            m_col = MASKBIT(j);
            if( m_col & col_used ) break; /* Each column used only once */
            col_used |= m_col;
            if( maps ) maps[i] = j;
          }
 
          assert( n_expr>0 && n_expr<BMS );
          assert( i==n_expr || col_used!=(MASKBIT(n_expr)-1) );
          if( col_used==(MASKBIT(n_expr)-1) ){
            /* If we reach this point, that means the index idx is usable */
            int i_addr = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
            EXPLAIN_QUERY_PLAN((parser, 0,
                              "USING INDEX %s FOR IN-OPERATOR",idx->name));
            __vdbe_add_op3(v, OP_OpenRead, i_tab, idx->tnum, i_db);
            __vdbe_set_p4_key_info(parser, idx);
            VDBE_COMMENT((v, "%s", idx->name));
            assert( IN_INDEX_INDEX_DESC == IN_INDEX_INDEX_ASC+1 );
            type = IN_INDEX_INDEX_ASC + idx->sort_orders[0];
 
            if( pr_rhs_has_null ){
#ifdef DBSQL_ENABLE_COLUMN_USED_MASK
              int64_t mask = (1<<n_expr)-1;
              __vdbe_add_op4_dup8(v, OP_ColumnsUsed,
                  i_tab, 0, 0, (u_int8_t*)&mask, P4_INT64);
#endif
              *pr_rhs_has_null = ++parser->n_mem;
              if( n_expr==1 ){
                __set_has_null_flag(v, i_tab, *pr_rhs_has_null);
              }
            }
            __vdbe_jump_here(v, i_addr);
          }
        } /* End loop over indexes */
      } /* End if( affinity_ok ) */
    } /* End if not an rowid index */
  } /* End attempt to optimize using an index */

  /* If no preexisting index is available for the IN clause
  ** and IN_INDEX_NOOP is an allowed reply
  ** and the RHS of the IN operator is a list, not a subquery
  ** and the RHS is not constant or has two or fewer terms,
  ** then it is not worth creating an ephemeral table to evaluate
  ** the IN operator so return IN_INDEX_NOOP.
  */
  if( type==0
   && (in_flags & IN_INDEX_NOOP_OK)
   && EXPR_USE_X_LIST(x)
   && (!__in_rhs_is_constant(parser,x) || x->x.list->n_expr<=2)
  ){
    parser->n_tab--;  /* Back out the allocation of the unused cursor */
    i_tab = -1;       /* Cursor is not allocated */
    type = IN_INDEX_NOOP;
  }

  if( type==0 ){
    /* Could not find an existing table or index to use as the RHS b-tree.
    ** We will have to generate an ephemeral table to do the job.
    */
    u_int32_t saved_n_query_loop = parser->n_query_loop;
    int r_may_have_null = 0;
    int bloom_ok = (in_flags & IN_INDEX_MEMBERSHIP)!=0;
    type = IN_INDEX_EPH;
    if( in_flags & IN_INDEX_LOOP ){
      parser->n_query_loop = 0;
    }else if( pr_rhs_has_null ){
      *pr_rhs_has_null = r_may_have_null = ++parser->n_mem;
    }
    assert( x->op==TK_IN );
    if( !bloom_ok
     && EXPR_USE_X_SELECT(x)
     && (x->x.p_select->sel_flags & SF_ClonedRhsIn)!=0
    ){
      bloom_ok = 1;
    }
    __code_rhs_of_in(parser, x, i_tab, bloom_ok);
    if( r_may_have_null ){
      __set_has_null_flag(v, i_tab, r_may_have_null);
    }
    parser->n_query_loop = saved_n_query_loop;
  }

  if( maps && type!=IN_INDEX_INDEX_ASC && type!=IN_INDEX_INDEX_DESC ){
    int i, n;
    n = __expr_vector_size(x->left);
    for(i=0; i<n; i++) maps[i] = i;
  }
  *pi_tab = i_tab;
  return type;
}
#endif

#ifndef DBSQL_OMIT_SUBQUERY
/*
** Argument expr is an (?, ?...) IN(...) expression. This
** function allocates and returns a nul-terminated string containing
** the affinities to be used for each column of the comparison.
**
** It is the responsibility of the caller to ensure that the returned
** string is eventually freed using __dbsql_db_free().
*/
static char *__expr_in_affinity(parser_t *parser, const expr_t *expr){
  expr_t *left = expr->left;
  int n_val = __expr_vector_size(left);
  select_t *p_select = EXPR_USE_X_SELECT(expr) ? expr->x.p_select : 0;
  char *ret;

  assert( expr->op==TK_IN );
  ret = __db_malloc_raw(parser->db, 1+(int64_t)n_val);
  if( ret ){
    int i;
    for(i=0; i<n_val; i++){
      expr_t *p_a = __vector_field_subexpr(left, i);
      char a = __expr_affinity(p_a);
      if( p_select ){
        ret[i] = __compare_affinity(p_select->e_list->a[i].expr, a);
      }else{
        ret[i] = a;
      }
    }
    ret[n_val] = '\0';
  }
  return ret;
}
#endif

#ifndef DBSQL_OMIT_SUBQUERY
/*
** Load the parser_t object passed as the first argument with an error
** message of the form:
**
**   "sub-select returns N columns - expected M"
*/  
void __subselect_error(parser_t *parser, int n_actual, int n_expect){
  if( parser->n_err==0 ){
    const char *fmt = "sub-select returns %d columns - expected %d";
    __error_msg(parser, fmt, n_actual, n_expect);
  }
}
#endif

/*
** Expression expr is a vector that has been used in a context where
** it is not permitted. If expr is a sub-select vector, this routine
** loads the parser_t object with a message of the form:
**
**   "sub-select returns N columns - expected 1"
**
** Or, if it is a regular scalar vector:
**
**   "row value misused"
*/  
void __vector_error_msg(parser_t *parser, expr_t *expr){
#ifndef DBSQL_OMIT_SUBQUERY
  if( EXPR_USE_X_SELECT(expr) ){
    __subselect_error(parser, expr->x.p_select->e_list->n_expr, 1);
  }else
#endif
  {
    __error_msg(parser, "row value misused");
  }
}

#ifndef DBSQL_OMIT_SUBQUERY
/*
** Scan all previously generated bytecode looking for an OP_BeginSubrtn
** that is compatible with expr.  If found, add the y.sub values
** to expr and return true.  If not found, return false.
*/
static int __find_compatible_in_rhs_subrtn(
  parser_t *parser,          /* Parsing context */
  expr_t *expr,            /* IN operator with RHS that we want to reuse */
  subrtn_sig_t *new_sig      /* Signature for the IN operator */
){
  vdbe_op_t *op, *end;
  subrtn_sig_t *sig;
  vdbe_t *v;

  if( new_sig==0 ) return 0;
  if( (parser->m_subrtn_sig & (1<<(new_sig->sel_id&7)))==0 ) return 0;
  assert( expr->op==TK_IN );
  assert( !EXPR_USE_Y_SUB(expr) );
  assert( EXPR_USE_X_SELECT(expr) );
  assert( expr->x.p_select!=0 );
  assert( (expr->x.p_select->sel_flags & SF_All)==0 );
  v = parser->vdbe;
  assert( v!=0 );
  op = __vdbe_get_op(v, 1);
  end = __vdbe_get_last_op(v);
  for(; op<end; op++){
    if( op->p4type!=P4_SUBRTNSIG ) continue;
    assert( op->opcode==OP_BeginSubrtn );
    sig = op->p4.subrtn_sig;
    assert( sig!=0 );
    if( !sig->complete ) continue;
    if( new_sig->sel_id!=sig->sel_id ) continue;
    if( strcmp(new_sig->aff,sig->aff)!=0 ) continue;
    expr->y.sub.i_addr = sig->i_addr;
    expr->y.sub.reg_return = sig->reg_return;
    expr->i_table = sig->i_table;
    EXPR_SET_PROPERTY(expr, EP_Subrtn);
    return 1;
  }
  return 0;
}
#endif /* DBSQL_OMIT_SUBQUERY */

#ifndef DBSQL_OMIT_SUBQUERY
/*
** Generate code that will construct an ephemeral table containing all terms
** in the RHS of an IN operator.  The IN operator can be in either of two
** forms:
**
**     x IN (4,5,11)              -- IN operator with list on right-hand side
**     x IN (SELECT a FROM b)     -- IN operator with subquery on the right
**
** The expr parameter is the IN operator.  The cursor number for the
** constructed ephemeral table is returned.  The first time the ephemeral
** table is computed, the cursor number is also stored in expr->i_table,
** however the cursor number returned might not be the same, as it might
** have been duplicated using OP_OpenDup.
**
** If the LHS expression ("x" in the examples) is a column value, or
** the SELECT statement returns a column value, then the affinity of that
** column is used to build the index keys. If both 'x' and the
** SELECT... statement are columns, then numeric affinity is used
** if either column has NUMERIC or INTEGER affinity. If neither
** 'x' nor the SELECT... statement are columns, then numeric affinity
** is used.
*/
void __code_rhs_of_in(
  parser_t *parser,          /* Parsing context */
  expr_t *expr,            /* The IN operator */
  int i_tab,               /* Use this cursor number */
  int allow_bloom          /* True to allow the use of a Bloom filter */
){
  int addr_once = 0;           /* Address of the OP_Once instruction at top */
  int addr;                   /* Address of OP_OpenEphemeral instruction */
  expr_t *left;                /* the LHS of the IN operator */
  key_info_t *key_info = 0;      /* Key information */
  int n_val;                   /* Size of vector left */
  vdbe_t *v;                    /* The prepared statement under construction */
  subrtn_sig_t *sig = 0;        /* Signature for this subroutine */

  v = parser->vdbe;
  assert( v!=0 );

  /* The evaluation of the IN must be repeated every time it
  ** is encountered if any of the following is true:
  **
  **    *  The right-hand side is a correlated subquery
  **    *  The right-hand side is an expression list containing variables
  **    *  We are inside a trigger
  **
  ** If all of the above are false, then we can compute the RHS just once
  ** and reuse it many names.
  */
  if( !EXPR_HAS_PROPERTY(expr, EP_VarSelect) && parser->i_self_tab==0 ){
    /* Reuse of the RHS is allowed
    **
    ** Compute a signature for the RHS of the IN operator to facility
    ** finding and reusing prior instances of the same IN operator.
    */
    assert( !EXPR_USE_X_SELECT(expr) || expr->x.p_select!=0 );
    if( EXPR_USE_X_SELECT(expr) && (expr->x.p_select->sel_flags & SF_All)==0 ){
      sig = __db_malloc_raw_nn(parser->db, sizeof(sig[0]));
      if( sig ){
        sig->sel_id = expr->x.p_select->sel_id;
        sig->aff = __expr_in_affinity(parser, expr);
      }
    }

    /* Check to see if there is a prior materialization of the RHS of
    ** this IN operator.  If there is, then make use of that prior
    ** materialization rather than recomputing it.
    */
    if( EXPR_HAS_PROPERTY(expr, EP_Subrtn) 
     || __find_compatible_in_rhs_subrtn(parser, expr, sig)
    ){
      addr_once = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
      if( EXPR_USE_X_SELECT(expr) ){
        EXPLAIN_QUERY_PLAN((parser, 0, "REUSE LIST SUBQUERY %d",
              expr->x.p_select->sel_id));
      }
      assert( EXPR_USE_Y_SUB(expr) );
      __vdbe_add_op2(v, OP_Gosub, expr->y.sub.reg_return,
                        expr->y.sub.i_addr);
      assert( i_tab!=expr->i_table );
      __vdbe_add_op2(v, OP_OpenDup, i_tab, expr->i_table);
      __vdbe_jump_here(v, addr_once);
      if( sig ){
        __dbsql_db_free(parser->db, sig->aff);
        __dbsql_db_free(parser->db, sig);
      }
      return;
    }

    /* Begin coding the subroutine */
    assert( !EXPR_USE_Y_WIN(expr) );
    EXPR_SET_PROPERTY(expr, EP_Subrtn);
    assert( !EXPR_HAS_PROPERTY(expr, EP_TokenOnly|EP_Reduced) );
    expr->y.sub.reg_return = ++parser->n_mem;
    expr->y.sub.i_addr =
      __vdbe_add_op2(v, OP_BeginSubrtn, 0, expr->y.sub.reg_return) + 1;
    if( sig ){
      sig->complete = 0;
      sig->i_addr = expr->y.sub.i_addr;
      sig->reg_return = expr->y.sub.reg_return;
      sig->i_table = i_tab;
      parser->m_subrtn_sig = 1 << (sig->sel_id&7);
      __vdbe_change_p4(v, -1, (const char*)sig, P4_SUBRTNSIG);
    }
    addr_once = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
  }

  /* Check to see if this is a vector IN operator */
  left = expr->left;
  n_val = __expr_vector_size(left);

  /* Construct the ephemeral table that will contain the content of
  ** RHS of the IN operator.
  */
  expr->i_table = i_tab;
  addr = __vdbe_add_op2(v, OP_OpenEphemeral, expr->i_table, n_val);
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
  if( EXPR_USE_X_SELECT(expr) ){
    VDBE_COMMENT((v, "Result of SELECT %u", expr->x.p_select->sel_id));
  }else{
    VDBE_COMMENT((v, "RHS of IN operator"));
  }
#endif
  key_info = __key_info_alloc(parser->db, n_val, 1);

  if( EXPR_USE_X_SELECT(expr) ){
    /* Case 1:     expr IN (SELECT ...)
    **
    ** Generate code to write the results of the select into the temporary
    ** table allocated and opened above.
    */
    select_t *p_select = expr->x.p_select;
    expr_list_t *e_list = p_select->e_list;

    EXPLAIN_QUERY_PLAN((parser, 1, "%sLIST SUBQUERY %d",
        addr_once?"":"CORRELATED ", p_select->sel_id
    ));
    /* If the LHS and RHS of the IN operator do not match, that
    ** error will have been caught long before we reach this point. */
    if( ALWAYS(e_list->n_expr==n_val) ){
      select_t *copy;
      select_dest_t dest;
      int i;
      int rc;
      int addr_bloom = 0;
      __select_dest_init(&dest, SRT_Set, i_tab);
      dest.aff_sdst = __expr_in_affinity(parser, expr);
      p_select->i_limit = 0;
      if( addr_once
       && allow_bloom
       && OPTIMIZATION_ENABLED(parser->db, DBSQL_BloomFilter)
      ){
        int reg_bloom = ++parser->n_mem;
        addr_bloom = __vdbe_add_op2(v, OP_Blob, 10000, reg_bloom);
        VDBE_COMMENT((v, "Bloom filter"));
        dest.i_sd_parm2 = reg_bloom;
      }
      testcase( p_select->sel_flags & SF_Distinct );
      testcase( key_info==0 ); /* Caused by OOM in __key_info_alloc() */
      copy = __select_dup(parser->db, p_select, 0);
      rc = parser->db->malloc_failed ? 1 :__select(parser, copy, &dest);
      __select_delete(parser->db, copy);
      __dbsql_db_free(parser->db, dest.aff_sdst);
      if( addr_bloom ){
        /* Remember that location of the Bloom filter in the P3 operand
        ** of the OP_Once that began this subroutine. tag-202407032019 */
        __vdbe_get_op(v, addr_once)->p3 = dest.i_sd_parm2;
        if( dest.i_sd_parm2==0 ){
          /* If the Bloom filter won't actually be used, keep it small */
          __vdbe_get_op(v, addr_bloom)->p1 = 10;
        }
      }
      if( rc ){
        __key_info_unref(key_info);
        return;
      }
      assert( key_info!=0 ); /* OOM will cause exit after __select() */
      assert( e_list!=0 );
      assert( e_list->n_expr>0 );
      assert( __key_info_is_writeable(key_info) );
      for(i=0; i<n_val; i++){
        expr_t *p = __vector_field_subexpr(left, i);
        key_info->colls[i] = __binary_compare_coll_seq(
            parser, p, e_list->a[i].expr
        );
      }
    }
  }else if( ALWAYS(expr->x.list!=0) ){
    /* Case 2:     expr IN (exprlist)
    **
    ** For each expression, build an index key from the evaluation and
    ** store it in the temporary table. If <expr> is a column, then use
    ** that columns affinity when building index keys. If <expr> is not
    ** a column, use numeric affinity.
    */
    char affinity;            /* Affinity of the LHS of the IN */
    int i;
    expr_list_t *list = expr->x.list;
    struct expr_list_item_t *item;
    int r1, r2;
    affinity = __expr_affinity(left);
    if( affinity<=DBSQL_AFF_NONE ){
      affinity = DBSQL_AFF_BLOB;
    }else if( affinity==DBSQL_AFF_REAL ){
      affinity = DBSQL_AFF_NUMERIC;
    }
    if( key_info ){
      assert( __key_info_is_writeable(key_info) );
      key_info->colls[0] = __expr_coll_seq(parser, expr->left);
    }

    /* Loop through each expression in <exprlist>. */
    r1 = __get_temp_reg(parser);
    r2 = __get_temp_reg(parser);
    for(i=list->n_expr, item=list->a; i>0; i--, item++){
      expr_t *e2 = item->expr;

      /* If the expression is not constant then we will need to
      ** disable the test that was generated above that makes sure
      ** this code only executes once.  Because for a non-constant
      ** expression we need to rerun this code each time.
      */
      if( addr_once && !__expr_is_constant(parser, e2) ){
        __vdbe_change_to_noop(v, addr_once-1);
        __vdbe_change_to_noop(v, addr_once);
        EXPR_CLEAR_PROPERTY(expr, EP_Subrtn);
        addr_once = 0;
      }

      /* Evaluate the expression and insert it into the temp table */
      __expr_code(parser, e2, r1);
      __vdbe_add_op4(v, OP_MakeRecord, r1, 1, r2, &affinity, 1);
      __vdbe_add_op4_int(v, OP_IdxInsert, i_tab, r2, r1, 1);
    }
    __release_temp_reg(parser, r1);
    __release_temp_reg(parser, r2);
  }
  if( sig ) sig->complete = 1;
  if( key_info ){
    __vdbe_change_p4(v, addr, (void *)key_info, P4_KEYINFO);
  }
  if( addr_once ){
    __vdbe_add_op1(v, OP_NullRow, i_tab);
    __vdbe_jump_here(v, addr_once);
    /* Subroutine return */
    assert( EXPR_USE_Y_SUB(expr) );
    assert( __vdbe_get_op(v,expr->y.sub.i_addr-1)->opcode==OP_BeginSubrtn
            || parser->n_err );
    __vdbe_add_op3(v, OP_Return, expr->y.sub.reg_return,
                      expr->y.sub.i_addr, 1);
    VDBE_COVERAGE(v);
    __clear_temp_reg_cache(parser);
  }
}
#endif /* DBSQL_OMIT_SUBQUERY */

/*
** Generate code for scalar subqueries used as a subquery expression
** or EXISTS operator:
**
**     (SELECT a FROM b)          -- subquery
**     EXISTS (SELECT a FROM b)   -- EXISTS subquery
**
** The expr parameter is the SELECT or EXISTS operator to be coded.
**
** Return the register that holds the result.  For a multi-column SELECT,
** the result is stored in a contiguous array of registers and the
** return value is the register of the left-most result column.
** Return 0 if an error occurs.
*/
#ifndef DBSQL_OMIT_SUBQUERY
int __code_subselect(parser_t *parser, expr_t *expr){
  int addr_once = 0;           /* Address of OP_Once at top of subroutine */
  int r_reg = 0;               /* Register storing resulting */
  select_t *sel;               /* SELECT statement to encode */
  select_dest_t dest;            /* How to deal with SELECT result */
  int n_reg;                   /* Registers to allocate */
  expr_t *limit;               /* New limit expression */
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  int addr_explain;            /* Address of OP_Explain instruction */
#endif

  vdbe_t *v = parser->vdbe;
  assert( v!=0 );
  if( parser->n_err ) return 0;
  testcase( expr->op==TK_EXISTS );
  testcase( expr->op==TK_SELECT );
  assert( expr->op==TK_EXISTS || expr->op==TK_SELECT );
  assert( EXPR_USE_X_SELECT(expr) );
  sel = expr->x.p_select;

  /* If this routine has already been coded, then invoke it as a
  ** subroutine. */
  if( EXPR_HAS_PROPERTY(expr, EP_Subrtn) ){
    EXPLAIN_QUERY_PLAN((parser, 0, "REUSE SUBQUERY %d", sel->sel_id));
    assert( EXPR_USE_Y_SUB(expr) );
    __vdbe_add_op2(v, OP_Gosub, expr->y.sub.reg_return,
                      expr->y.sub.i_addr);
    return expr->i_table;
  }

  /* Begin coding the subroutine */
  assert( !EXPR_USE_Y_WIN(expr) );
  assert( !EXPR_HAS_PROPERTY(expr, EP_Reduced|EP_TokenOnly) );
  EXPR_SET_PROPERTY(expr, EP_Subrtn);
  expr->y.sub.reg_return = ++parser->n_mem;
  expr->y.sub.i_addr =
    __vdbe_add_op2(v, OP_BeginSubrtn, 0, expr->y.sub.reg_return) + 1;

  /* The evaluation of the EXISTS/SELECT must be repeated every time it
  ** is encountered if any of the following is true:
  **
  **    *  The right-hand side is a correlated subquery
  **    *  The right-hand side is an expression list containing variables
  **    *  We are inside a trigger
  **
  ** If all of the above are false, then we can run this code just once
  ** save the results, and reuse the same result on subsequent invocations.
  */
  if( !EXPR_HAS_PROPERTY(expr, EP_VarSelect) ){
    addr_once = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
  }
 
  /* For a SELECT, generate code to put the values for all columns of
  ** the first row into an array of registers and return the index of
  ** the first register.
  **
  ** If this is an EXISTS, write an integer 0 (not exists) or 1 (exists)
  ** into a register and return that register number.
  **
  ** In both cases, the query is augmented with "LIMIT 1".  Any
  ** preexisting limit is discarded in place of the new LIMIT 1.
  */
  EXPLAIN_QUERY_PLAN2(addr_explain, (parser, 1, "%sSCALAR SUBQUERY %d",
        addr_once?"":"CORRELATED ", sel->sel_id));
  __vdbe_scan_status_counters(v, addr_explain, addr_explain, -1);
  n_reg = expr->op==TK_SELECT ? sel->e_list->n_expr : 1;
  __select_dest_init(&dest, 0, parser->n_mem+1);
  parser->n_mem += n_reg;
  if( expr->op==TK_SELECT ){
    dest.dest = SRT_Mem;
    if( (sel->sel_flags&SF_Distinct) && sel->limit && sel->limit->right ){
      /* If there is both a DISTINCT and an OFFSET clause, then allocate
      ** a separate dest.i_sdst array for __select() and other
      ** routines to populate. In this case results will be copied over
      ** into the dest.i_sd_parm array only after OFFSET processing. This
      ** ensures that in the case where OFFSET excludes all rows, the
      ** dest.i_sd_parm array is not left populated with the contents of the
      ** last row visited - it should be all NULLs if all rows were
      ** excluded by OFFSET.  */ 
      dest.i_sdst = parser->n_mem+1;
      parser->n_mem += n_reg;
    }else{
      dest.i_sdst = dest.i_sd_parm;
    }
    dest.n_sdst = n_reg;
    __vdbe_add_op3(v, OP_Null, 0, dest.i_sd_parm, parser->n_mem);
    VDBE_COMMENT((v, "Init subquery result"));
  }else{
    dest.dest = SRT_Exists;
    __vdbe_add_op2(v, OP_Integer, 0, dest.i_sd_parm);
    VDBE_COMMENT((v, "Init EXISTS result"));
  }
  if( sel->limit ){
    /* The subquery already has a limit.  If the pre-existing limit X is 
    ** not already integer value 1 or 0, then make the new limit X<>0 so that
    ** the new limit is either 1 or 0 */
    expr_t *left = sel->limit->left;
    if( EXPR_HAS_PROPERTY(left, EP_IntValue)==0
     || (left->u.i_value!=1 && left->u.i_value!=0)
    ){
      DBSQL *db = parser->db;
      limit = __expr_int32(db, 0);
      if( limit ){
        limit->aff_expr = DBSQL_AFF_NUMERIC;
        limit = __p_expr(parser, TK_NE,
            __expr_dup(db, left, 0), limit);
      }
      __expr_deferred_delete(parser, left);
      sel->limit->left = limit;
    }
  }else{
    /* If there is no pre-existing limit add a limit of 1 */
    limit = __expr_int32(parser->db, 1);
    sel->limit = __p_expr(parser, TK_LIMIT, limit, 0);
  }
  sel->i_limit = 0;
  if( __select(parser, sel, &dest) ){
    expr->op2 = expr->op;
    expr->op = TK_ERROR;
    return 0;
  }
  expr->i_table = r_reg = dest.i_sd_parm;
  EXPR_SET_VVA_PROPERTY(expr, EP_NoReduce);
  if( addr_once ){
    __vdbe_jump_here(v, addr_once);
  }
  __vdbe_scan_status_range(v, addr_explain, addr_explain, -1);

  /* Subroutine return */
  assert( EXPR_USE_Y_SUB(expr) );
  assert( __vdbe_get_op(v,expr->y.sub.i_addr-1)->opcode==OP_BeginSubrtn
          || parser->n_err );
  __vdbe_add_op3(v, OP_Return, expr->y.sub.reg_return,
                    expr->y.sub.i_addr, 1);
  VDBE_COVERAGE(v);
  __clear_temp_reg_cache(parser);
  return r_reg;
}
#endif /* DBSQL_OMIT_SUBQUERY */

#ifndef DBSQL_OMIT_SUBQUERY
/*
** expr_t in is an IN(...) expression. This function checks that the
** sub-select on the RHS of the IN() operator has the same number of
** columns as the vector on the LHS. Or, if the RHS of the IN() is not
** a sub-query, that the LHS is a vector of size 1.
*/
int __expr_check_in(parser_t *parser, expr_t *in){
  int n_vector = __expr_vector_size(in->left);
  if( EXPR_USE_X_SELECT(in) && !parser->db->malloc_failed ){
    if( n_vector!=in->x.p_select->e_list->n_expr ){
      __subselect_error(parser, in->x.p_select->e_list->n_expr, n_vector);
      return 1;
    }
  }else if( n_vector!=1 ){
    __vector_error_msg(parser, in->left);
    return 1;
  }
  return 0;
}
#endif

#ifndef DBSQL_OMIT_SUBQUERY
/*
** Generate code for an IN expression.
**
**      x IN (SELECT ...)
**      x IN (value, value, ...)
**
** The left-hand side (LHS) is a scalar or vector expression.  The
** right-hand side (RHS) is an array of zero or more scalar values, or a
** subquery.  If the RHS is a subquery, the number of result columns must
** match the number of columns in the vector on the LHS.  If the RHS is
** a list of values, the LHS must be a scalar.
**
** The IN operator is true if the LHS value is contained within the RHS.
** The result is false if the LHS is definitely not in the RHS.  The
** result is NULL if the presence of the LHS in the RHS cannot be
** determined due to NULLs.
**
** This routine generates code that jumps to dest_if_false if the LHS is not
** contained within the RHS.  If due to NULLs we cannot determine if the LHS
** is contained in the RHS then jump to dest_if_null.  If the LHS is contained
** within the RHS then fall through.
**
** See the separate in-operator.md documentation file in the canonical
** dbsql source tree for additional information.
*/
static void __expr_code_in(
  parser_t *parser,        /* Parsing and code generating context */
  expr_t *expr,          /* The IN expression */
  int dest_if_false,      /* Jump here if LHS is not contained in the RHS */
  int dest_if_null        /* Jump here if the results are unknown due to NULLs */
){
  int r_rhs_has_null = 0;  /* Register that is true if RHS contains NULL values */
  int type;            /* Type of the RHS */
  int r_lhs;             /* Register(s) holding the LHS values */
  vdbe_t *v;              /* Statement under construction */
  int *maps = 0;       /* Map from vector field to index column */
  char *aff = 0;       /* Affinity string for comparisons */
  int n_vector;          /* Size of vectors for this IN operator */
  int i_dummy;           /* Dummy parameter to __expr_code_vector() */
  expr_t *left;          /* The LHS of the IN operator */
  int i;                /* loop counter */
  int dest_step2;        /* Where to jump when NULLs seen in step 2 */
  int dest_step6 = 0;    /* Start of code for Step 6 */
  int addr_truth_op;      /* Address of opcode that determines the IN is true */
  int dest_not_null;      /* Jump here if a comparison is not true in step 6 */
  int addr_top;          /* Top of the step-6 loop */
  int i_tab = 0;         /* index_t to use */
  u_int8_t ok_const_factor = parser->ok_const_factor;

  assert( !EXPR_HAS_VVA_PROPERTY(expr,EP_Immutable) );
  left = expr->left;
  if( __expr_check_in(parser, expr) ) return;
  aff = __expr_in_affinity(parser, expr);
  n_vector = __expr_vector_size(expr->left);
  maps = (int*)__db_malloc_zero(parser->db, n_vector*sizeof(int));
  if( parser->db->malloc_failed ) goto __expr_code_in_oom_error;

  /* Attempt to compute the RHS. After this step, if anything other than
  ** IN_INDEX_NOOP is returned, the table opened with cursor i_tab
  ** contains the values that make up the RHS. If IN_INDEX_NOOP is returned,
  ** the RHS has not yet been coded.  */
  v = parser->vdbe;
  assert( v!=0 );       /* OOM detected prior to this routine */
  VDBE_NOOP_COMMENT((v, "begin IN expr"));
  type = __find_in_index(parser, expr,
                             IN_INDEX_MEMBERSHIP | IN_INDEX_NOOP_OK,
                             dest_if_false==dest_if_null ? 0 : &r_rhs_has_null,
                             maps, &i_tab);

  assert( parser->n_err || n_vector==1 || type==IN_INDEX_EPH
       || type==IN_INDEX_INDEX_ASC || type==IN_INDEX_INDEX_DESC
  );
#ifdef DBSQL_DEBUG
  /* Confirm that maps[] contains n_vector integer values between 0 and
  ** n_vector-1. */
  for(i=0; i<n_vector; i++){
    int j, cnt;
    for(cnt=j=0; j<n_vector; j++) if( maps[j]==i ) cnt++;
    assert( cnt==1 );
  }
#endif

  /* Code the LHS, the <expr> from "<expr> IN (...)". If the LHS is a
  ** vector, then it is stored in an array of n_vector registers starting
  ** at r1.
  **
  ** __find_in_index() might have reordered the fields of the LHS vector
  ** so that the fields are in the same order as an existing index.   The
  ** maps[] array contains a mapping from the original LHS field order to
  ** the field order that matches the RHS index.
  **
  ** Avoid factoring the LHS of the IN(...) expression out of the loop,
  ** even if it is constant, as OP_Affinity may be used on the register
  ** by code generated below.  */
  assert( parser->ok_const_factor==ok_const_factor );
  parser->ok_const_factor = 0;
  r_lhs = __expr_code_vector(parser, left, &i_dummy);
  parser->ok_const_factor = ok_const_factor;

  /* If __find_in_index() did not find or create an index that is
  ** suitable for evaluating the IN operator, then evaluate using a
  ** sequence of comparisons.
  **
  ** This is step (1) in the in-operator.md optimized algorithm.
  */
  if( type==IN_INDEX_NOOP ){
    expr_list_t *list;
    coll_seq_t *coll;
    int label_ok = __vdbe_make_label(parser);
    int r2, reg_to_free;
    int reg_ck_null = 0;
    int ii;
    assert( n_vector==1 );
    assert( EXPR_USE_X_LIST(expr) );
    list = expr->x.list;
    coll = __expr_coll_seq(parser, expr->left);
    if( dest_if_null!=dest_if_false ){
      reg_ck_null = __get_temp_reg(parser);
      __vdbe_add_op3(v, OP_BitAnd, r_lhs, r_lhs, reg_ck_null);
    }
    for(ii=0; ii<list->n_expr; ii++){
      r2 = __expr_code_temp(parser, list->a[ii].expr, &reg_to_free);
      if( reg_ck_null && __expr_can_be_null(list->a[ii].expr) ){
        __vdbe_add_op3(v, OP_BitAnd, reg_ck_null, r2, reg_ck_null);
      }
      __release_temp_reg(parser, reg_to_free);
      if( ii<list->n_expr-1 || dest_if_null!=dest_if_false ){
        int op = r_lhs!=r2 ? OP_Eq : OP_NotNull;
        __vdbe_add_op4(v, op, r_lhs, label_ok, r2,
                          (void*)coll, P4_COLLSEQ);
        VDBE_COVERAGE_IF(v, ii<list->n_expr-1 && op==OP_Eq);
        VDBE_COVERAGE_IF(v, ii==list->n_expr-1 && op==OP_Eq);
        VDBE_COVERAGE_IF(v, ii<list->n_expr-1 && op==OP_NotNull);
        VDBE_COVERAGE_IF(v, ii==list->n_expr-1 && op==OP_NotNull);
        __vdbe_change_p5(v, aff[0]);
      }else{
        int op = r_lhs!=r2 ? OP_Ne : OP_IsNull;
        assert( dest_if_null==dest_if_false );
        __vdbe_add_op4(v, op, r_lhs, dest_if_false, r2,
                          (void*)coll, P4_COLLSEQ);
        VDBE_COVERAGE_IF(v, op==OP_Ne);
        VDBE_COVERAGE_IF(v, op==OP_IsNull);
        __vdbe_change_p5(v, aff[0] | DBSQL_JUMPIFNULL);
      }
    }
    if( reg_ck_null ){
      __vdbe_add_op2(v, OP_IsNull, reg_ck_null, dest_if_null); VDBE_COVERAGE(v);
      __vdbe_goto(v, dest_if_false);
    }
    __vdbe_resolve_label(v, label_ok);
    __release_temp_reg(parser, reg_ck_null);
    goto __expr_code_in_finished;
  }

  if( type!=IN_INDEX_ROWID ){
    /* If this IN operator will use an index, then the order of columns in the
    ** vector might be different from the order in the index.  In that case,
    ** we need to reorder the LHS values to be in index order.  Run Affinity
    ** before reordering the columns, so that the affinity is correct.
    */
    __vdbe_add_op4(v, OP_Affinity, r_lhs, n_vector, 0, aff, n_vector);
    for(i=0; i<n_vector && maps[i]==i; i++){} /* Are LHS fields reordered? */
    if( i!=n_vector ){
      /* Need to reorder the LHS fields according to maps */
      int r_lhs_orig = r_lhs;
      r_lhs = __get_temp_range(parser, n_vector);
      for(i=0; i<n_vector; i++){
        __vdbe_add_op3(v, OP_Copy, r_lhs_orig+i, r_lhs+maps[i], 0);
      }
      __release_temp_reg(parser, r_lhs_orig);
    }
  }


  /* Step 2: Check to see if the LHS contains any NULL columns.  If the
  ** LHS does contain NULLs then the result must be either FALSE or NULL.
  ** We will then skip the binary search of the RHS.
  */
  if( dest_if_null==dest_if_false ){
    dest_step2 = dest_if_false;
  }else{
    dest_step2 = dest_step6 = __vdbe_make_label(parser);
  }
  for(i=0; i<n_vector; i++){
    expr_t *p = __vector_field_subexpr(expr->left, i);
    if( parser->n_err ) goto __expr_code_in_oom_error;
    if( __expr_can_be_null(p) ){
      __vdbe_add_op2(v, OP_IsNull, r_lhs+maps[i], dest_step2);
      VDBE_COVERAGE(v);
    }
  }

  /* Step 3.  The LHS is now known to be non-NULL.  Do the binary search
  ** of the RHS using the LHS as a probe.  If found, the result is
  ** true.
  */
  if( type==IN_INDEX_ROWID ){
    /* In this case, the RHS is the ROWID of table b-tree and so we also
    ** know that the RHS is non-NULL.  Hence, we combine steps 3 and 4
    ** into a single opcode. */
    assert( n_vector==1 );
    __vdbe_add_op3(v, OP_SeekRowid, i_tab, dest_if_false, r_lhs);
    VDBE_COVERAGE(v);
    addr_truth_op = __vdbe_add_op0(v, OP_Goto);  /* Return True */
  }else{
    if( dest_if_false==dest_if_null ){
      /* Combine Step 3 and Step 5 into a single opcode */
      if( EXPR_HAS_PROPERTY(expr, EP_Subrtn) ){
        const vdbe_op_t *p_op = __vdbe_get_op(v, expr->y.sub.i_addr);
        assert( p_op->opcode==OP_Once || parser->n_err );
        if( p_op->p3>0 ){  /* tag-202407032019 */
          assert( OPTIMIZATION_ENABLED(parser->db, DBSQL_BloomFilter)
                 || parser->n_err );
          __vdbe_add_op4_int(v, OP_Filter, p_op->p3, dest_if_false,
                               r_lhs, n_vector); VDBE_COVERAGE(v);
        }
      }
      __vdbe_add_op4_int(v, OP_NotFound, i_tab, dest_if_false,
                           r_lhs, n_vector); VDBE_COVERAGE(v);
      goto __expr_code_in_finished;
    }
    /* Ordinary Step 3, for the case where FALSE and NULL are distinct */
    addr_truth_op = __vdbe_add_op4_int(v, OP_Found, i_tab, 0,
                                      r_lhs, n_vector); VDBE_COVERAGE(v);
  }

  /* Step 4.  If the RHS is known to be non-NULL and we did not find
  ** an match on the search above, then the result must be FALSE.
  */
  if( r_rhs_has_null && n_vector==1 ){
    __vdbe_add_op2(v, OP_NotNull, r_rhs_has_null, dest_if_false);
    VDBE_COVERAGE(v);
  }

  /* Step 5.  If we do not care about the difference between NULL and
  ** FALSE, then just return false.
  */
  if( dest_if_false==dest_if_null ) __vdbe_goto(v, dest_if_false);

  /* Step 6: Loop through rows of the RHS.  Compare each row to the LHS.
  ** If any comparison is NULL, then the result is NULL.  If all
  ** comparisons are FALSE then the final result is FALSE.
  **
  ** For a scalar LHS, it is sufficient to check just the first row
  ** of the RHS.
  */
  if( dest_step6 ) __vdbe_resolve_label(v, dest_step6);
  addr_top = __vdbe_add_op2(v, OP_Rewind, i_tab, dest_if_false);
  VDBE_COVERAGE(v);
  if( n_vector>1 ){
    dest_not_null = __vdbe_make_label(parser);
  }else{
    /* For n_vector==1, combine steps 6 and 7 by immediately returning
    ** FALSE if the first comparison is not NULL */
    dest_not_null = dest_if_false;
  }
  for(i=0; i<n_vector; i++){
    expr_t *p;
    coll_seq_t *coll;
    int r3 = __get_temp_reg(parser);
    p = __vector_field_subexpr(left, i);
    if( EXPR_USE_X_SELECT(expr) ){
      expr_t *rhs = expr->x.p_select->e_list->a[i].expr;
      coll = __binary_compare_coll_seq(parser, p, rhs);
    }else{
      /* If the RHS of the IN(...) expression are scalar expressions, do
      ** not consider their collation sequences. The documentation says
      ** "The collating sequence used for expressions of the form "x IN (y, z,
      ** ...)" is the collating sequence of x.".  */
      coll = __expr_coll_seq(parser, p);
    }
    __vdbe_add_op3(v, OP_Column, i_tab, maps[i], r3);
    __vdbe_add_op4(v, OP_Ne, r_lhs+maps[i], dest_not_null, r3,
                      (void*)coll, P4_COLLSEQ);
    VDBE_COVERAGE(v);
    __release_temp_reg(parser, r3);
  }
  __vdbe_add_op2(v, OP_Goto, 0, dest_if_null);
  if( n_vector>1 ){
    __vdbe_resolve_label(v, dest_not_null);
    __vdbe_add_op2(v, OP_Next, i_tab, addr_top+1);
    VDBE_COVERAGE(v);

    /* Step 7:  If we reach this point, we know that the result must
    ** be false. */
    __vdbe_add_op2(v, OP_Goto, 0, dest_if_false);
  }

  /* Jumps here in order to return true. */
  __vdbe_jump_here(v, addr_truth_op);

__expr_code_in_finished:
  VDBE_COMMENT((v, "end IN expr"));
__expr_code_in_oom_error:
  __dbsql_db_free(parser->db, maps);
  __dbsql_db_free(parser->db, aff);
}
#endif /* DBSQL_OMIT_SUBQUERY */

#ifndef DBSQL_OMIT_FLOATING_POINT
/*
** Generate an instruction that will put the floating point
** value described by z[0..n-1] into register i_mem.
**
** The z[] string will probably not be zero-terminated.  But the
** z[n] character is guaranteed to be something that does not look
** like the continuation of the number.
*/
static void __code_real(vdbe_t *v, const char *z, int negate_flag, int i_mem){
  if( ALWAYS(z!=0) ){
    double value;
    __ato_f(z, &value);
    assert( !__is_na_n(value) ); /* The new __ato_f never returns __na_n */
    if( negate_flag ) value = -value;
    __vdbe_add_op4_dup8(v, OP_Real, 0, i_mem, 0, (u_int8_t*)&value, P4_REAL);
  }
}
#endif


/*
** Generate an instruction that will put the integer describe by
** text z[0..n-1] into register i_mem.
**
** expr_t.u.token is always UTF8 and zero-terminated.
*/
static void __code_integer(parser_t *parser, expr_t *expr, int neg_flag, int i_mem){
  vdbe_t *v = parser->vdbe;
  if( expr->flags & EP_IntValue ){
    int i = expr->u.i_value;
    assert( i>=0 );
    if( neg_flag ) i = -i;
    __vdbe_add_op2(v, OP_Integer, i, i_mem);
  }else{
    int c;
    int64_t value;
    const char *z = expr->u.token;
    assert( z!=0 );
    c = __dec_or_hex_to_i64(z, &value);
    if( (c==3 && !neg_flag) || (c==2) || (neg_flag && value==SMALLEST_INT64)){
#ifdef DBSQL_OMIT_FLOATING_POINT
      __error_msg(parser, "oversized integer: %s%#T", neg_flag?"-":"",expr);
#else
#ifndef DBSQL_OMIT_HEX_INTEGER
      if( dbsql_strnicmp(z,"0x",2)==0 ){
        __error_msg(parser, "hex literal too big: %s%#T",
                        neg_flag?"-":"",expr);
      }else
#endif
      {
        __code_real(v, z, neg_flag, i_mem);
      }
#endif
    }else{
      if( neg_flag ){ value = c==3 ? SMALLEST_INT64 : -value; }
      __vdbe_add_op4_dup8(v, OP_Int64, 0, i_mem, 0, (u_int8_t*)&value, P4_INT64);
    }
  }
}


/* Generate code that will load into register reg_out a value that is
** appropriate for the i_idx_col-th column of index idx.
*/
void __expr_code_load_index_column(
  parser_t *parser,  /* The parsing context */
  index_t *idx,    /* The index whose column is to be loaded */
  int i_tab_cur,    /* Cursor pointing to a table row */
  int i_idx_col,    /* The column of the index to be loaded */
  int reg_out      /* Store the index column value in this register */
){
  int16_t i_tab_col = idx->columns[i_idx_col];
  if( i_tab_col==XN_EXPR ){
    assert( idx->col_exprs );
    assert( idx->col_exprs->n_expr>i_idx_col );
    parser->i_self_tab = i_tab_cur + 1;
    __expr_code_copy(parser, idx->col_exprs->a[i_idx_col].expr, reg_out);
    parser->i_self_tab = 0;
  }else{
    __expr_code_get_column_of_table(parser->vdbe, idx->table, i_tab_cur,
                                    i_tab_col, reg_out);
  }
}

#ifndef DBSQL_OMIT_GENERATED_COLUMNS
/*
** Generate code that will compute the value of generated column col
** and store the result in register reg_out
*/
void __expr_code_generated_column(
  parser_t *parser,     /* Parsing context */
  table_t *tab,       /* table_t containing the generated column */
  column_t *col,      /* The generated column */
  int reg_out         /* Put the result in this register */
){
  int i_addr;
  vdbe_t *v = parser->vdbe;
  int n_err = parser->n_err;
  assert( v!=0 );
  assert( parser->i_self_tab!=0 );
  if( parser->i_self_tab>0 ){
    i_addr = __vdbe_add_op3(v, OP_IfNullRow, parser->i_self_tab-1, 0, reg_out);
  }else{
    i_addr = 0;
  }
  __expr_code_copy(parser, __column_expr(tab,col), reg_out);
  if( (col->col_flags & COLFLAG_VIRTUAL)!=0
   && (tab->tab_flags & TF_Strict)!=0
  ){
    int p3 = 2+(int)(col - tab->cols);
    __vdbe_add_op4(v, OP_TypeCheck, reg_out, 1, p3, (char*)tab, P4_TABLE);
  }else if( col->affinity>=DBSQL_AFF_TEXT ){
    __vdbe_add_op4(v, OP_Affinity, reg_out, 1, 0, &col->affinity, 1);
  }
  if( i_addr ) __vdbe_jump_here(v, i_addr);
  if( parser->n_err>n_err ) parser->db->err_byte_offset = -1;
}
#endif /* DBSQL_OMIT_GENERATED_COLUMNS */

/*
** Generate code to extract the value of the i_col-th column of a table.
*/
void __expr_code_get_column_of_table(
  vdbe_t *v,        /* Parsing context */
  table_t *tab,    /* The table containing the value */
  int i_tab_cur,    /* The table cursor.  Or the PK cursor for WITHOUT ROWID */
  int i_col,       /* index_t of the column to extract */
  int reg_out      /* Extract the value into this register */
){
  column_t *col;
  assert( v!=0 );
  assert( tab!=0 );
  assert( i_col!=XN_EXPR );
  if( i_col<0 || i_col==tab->i_p_key ){
    __vdbe_add_op2(v, OP_Rowid, i_tab_cur, reg_out);
    VDBE_COMMENT((v, "%s.rowid", tab->name));
  }else{
    int op;
    int x;
    if( IS_VIRTUAL(tab) ){
      op = OP_VColumn;
      x = i_col;
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
    }else if( (col = &tab->cols[i_col])->col_flags & COLFLAG_VIRTUAL ){
      parser_t *parser = __vdbe_parser(v);
      if( col->col_flags & COLFLAG_BUSY ){
        __error_msg(parser, "generated column loop on \"%s\"",
                        col->cn_name);
      }else{
        int saved_self_tab = parser->i_self_tab;
        col->col_flags |= COLFLAG_BUSY;
        parser->i_self_tab = i_tab_cur+1;
        __expr_code_generated_column(parser, tab, col, reg_out);
        parser->i_self_tab = saved_self_tab;
        col->col_flags &= ~COLFLAG_BUSY;
      }
      return;
#endif
    }else if( !HAS_ROWID(tab) ){
      testcase( i_col!=__table_column_to_storage(tab, i_col) );
      x = __table_column_to_index(__primary_key_index(tab), i_col);
      op = OP_Column;
    }else{
      x = __table_column_to_storage(tab,i_col);
      testcase( x!=i_col );
      op = OP_Column;
    }
    __vdbe_add_op3(v, op, i_tab_cur, x, reg_out);
    __column_default(v, tab, i_col, reg_out);
  }
}

/*
** Generate code that will extract the i_column-th column from
** table tab and store the column value in register i_reg.
**
** There must be an open cursor to tab in i_table when this routine
** is called.  If i_column<0 then code is generated that extracts the rowid.
*/
int __expr_code_get_column(
  parser_t *parser,   /* Parsing and code generating context */
  table_t *tab,     /* Description of the table we are reading from */
  int i_column,     /* index_t of the table column */
  int i_table,      /* The cursor pointing to the table */
  int i_reg,        /* Store results here */
  u_int8_t p5            /* P5 value for OP_Column + FLAGS */
){
  assert( parser->vdbe!=0 );
  assert( (p5 & (OPFLAG_NOCHNG|OPFLAG_TYPEOFARG|OPFLAG_LENGTHARG))==p5 );
  assert( IS_VIRTUAL(tab) || (p5 & OPFLAG_NOCHNG)==0 );
  __expr_code_get_column_of_table(parser->vdbe, tab, i_table, i_column, i_reg);
  if( p5 ){
    vdbe_op_t *op = __vdbe_get_last_op(parser->vdbe);
    if( op->opcode==OP_Column ) op->p5 = p5;
    if( op->opcode==OP_VColumn ) op->p5 = (p5 & OPFLAG_NOCHNG);
  }
  return i_reg;
}

/*
** Generate code to move content from registers i_from...i_from+n_reg-1
** over to i_to..i_to+n_reg-1.
*/
void __expr_code_move(parser_t *parser, int i_from, int i_to, int n_reg){
  __vdbe_add_op3(parser->vdbe, OP_Move, i_from, i_to, n_reg);
}

/*
** Convert a scalar expression node to a TK_REGISTER referencing
** register i_reg.  The caller must ensure that i_reg already contains
** the correct value for the expression.
*/
void __expr_to_register(expr_t *expr, int i_reg){
  expr_t *p = __expr_skip_collate_and_likely(expr);
  if( NEVER(p==0) ) return;
  if( p->op==TK_REGISTER ){
    assert( p->i_table==i_reg );
  }else{
    p->op2 = p->op;
    p->op = TK_REGISTER;
    p->i_table = i_reg;
    EXPR_CLEAR_PROPERTY(p, EP_Skip);
  }
}

/*
** Evaluate an expression (either a vector or a scalar expression) and store
** the result in contiguous temporary registers.  Return the index of
** the first register used to store the result.
**
** If the returned result register is a temporary scalar, then also write
** that register number into *pi_freeable.  If the returned result register
** is not a temporary or if the expression is a vector set *pi_freeable
** to 0.
*/
static int __expr_code_vector(parser_t *parser, expr_t *p, int *pi_freeable){
  int i_result;
  int n_result = __expr_vector_size(p);
  if( n_result==1 ){
    i_result = __expr_code_temp(parser, p, pi_freeable);
  }else{
    *pi_freeable = 0;
    if( p->op==TK_SELECT ){
#if DBSQL_OMIT_SUBQUERY
      i_result = 0;
#else
      i_result = __code_subselect(parser, p);
#endif
    }else{
      int i;
      i_result = parser->n_mem+1;
      parser->n_mem += n_result;
      assert( EXPR_USE_X_LIST(p) );
      for(i=0; i<n_result; i++){
        __expr_code_factorable(parser, p->x.list->a[i].expr, i+i_result);
      }
    }
  }
  return i_result;
}

/*
** If the last opcode is a OP_Copy, then set the do-not-merge flag (p5)
** so that a subsequent copy will not be merged into this one.
*/
static void __set_do_not_merge_flag_on_copy(vdbe_t *v){
  if( __vdbe_get_last_op(v)->opcode==OP_Copy ){
    __vdbe_change_p5(v, 1);  /* Tag trailing OP_Copy as not mergeable */
  }
}

/*
** Generate code to implement special SQL functions that are implemented
** in-line rather than by using the usual callbacks.
*/
static int __expr_code_inline_function(
  parser_t *parser,        /* Parsing context */
  expr_list_t *farg,      /* List of function arguments */
  int i_func_id,          /* Function ID.  One of the INTFUNC_... values */
  int target            /* Store function result in this register */
){
  int n_farg;
  vdbe_t *v = parser->vdbe;
  assert( v!=0 );
  assert( farg!=0 );
  n_farg = farg->n_expr;
  assert( n_farg>0 );  /* All in-line functions have at least one argument */
  switch( i_func_id ){
    case INLINEFUNC_coalesce: {
      /* Attempt a direct implementation of the built-in COALESCE() and
      ** IFNULL() functions.  This avoids unnecessary evaluation of
      ** arguments past the first non-NULL argument.
      */
      int end_coalesce = __vdbe_make_label(parser);
      int i;
      assert( n_farg>=2 );
      __expr_code(parser, farg->a[0].expr, target);
      for(i=1; i<n_farg; i++){
        __vdbe_add_op2(v, OP_NotNull, target, end_coalesce);
        VDBE_COVERAGE(v);
        __expr_code(parser, farg->a[i].expr, target);
      }
      __set_do_not_merge_flag_on_copy(v);
      __vdbe_resolve_label(v, end_coalesce);
      break;
    }
    case INLINEFUNC_iif: {
      expr_t case_expr;
      memset(&case_expr, 0, sizeof(case_expr));
      case_expr.op = TK_CASE;
      case_expr.x.list = farg;
      return __expr_code_target(parser, &case_expr, target);
    }
#ifdef DBSQL_ENABLE_OFFSET_SQL_FUNC
    case INLINEFUNC_dbsql_offset: {
      expr_t *arg = farg->a[0].expr;
      if( arg->op==TK_COLUMN && arg->i_table>=0 ){
        __vdbe_add_op3(v, OP_Offset, arg->i_table, arg->i_column, target);
      }else{
        __vdbe_add_op2(v, OP_Null, 0, target);
      }
      break;
    }
#endif
    default: {  
      /* The UNLIKELY() function is a no-op.  The result is the value
      ** of the first argument.
      */
      assert( n_farg==1 || n_farg==2 );
      target = __expr_code_target(parser, farg->a[0].expr, target);
      break;
    }

  /***********************************************************************
  ** Test-only SQL functions that are only usable if enabled
  ** via DBSQL_TESTCTRL_INTERNAL_FUNCTIONS
  */
#if !defined(DBSQL_UNTESTABLE)
    case INLINEFUNC_expr_compare: {
      /* Compare two expressions using __expr_compare() */
      assert( n_farg==2 );
      __vdbe_add_op2(v, OP_Integer,
         __expr_compare(0,farg->a[0].expr, farg->a[1].expr,-1),
         target);
      break;
    }

    case INLINEFUNC_expr_implies_expr: {
      /* Compare two expressions using __expr_implies_expr() */
      assert( n_farg==2 );
      __vdbe_add_op2(v, OP_Integer,
         __expr_implies_expr(parser,farg->a[0].expr, farg->a[1].expr,-1),
         target);
      break;
    }

    case INLINEFUNC_implies_nonnull_row: {
      /* Result of __expr_implies_non_null_row() */
      expr_t *a1;
      assert( n_farg==2 );
      a1 = farg->a[1].expr;
      if( a1->op==TK_COLUMN ){
        __vdbe_add_op2(v, OP_Integer,
           __expr_implies_non_null_row(farg->a[0].expr,a1->i_table,1),
           target);
      }else{
        __vdbe_add_op2(v, OP_Null, 0, target);
      }
      break;
    }

    case INLINEFUNC_affinity: {
      /* The AFFINITY() function evaluates to a string that describes
      ** the type affinity of the argument.  This is used for testing of
      ** the dbsql type logic.
      */
      const char *affs[] = { "blob", "text", "numeric", "integer",
                              "real", "flexnum" };
      char aff;
      assert( n_farg==1 );
      aff = __expr_affinity(farg->a[0].expr);
      assert( aff<=DBSQL_AFF_NONE
           || (aff>=DBSQL_AFF_BLOB && aff<=DBSQL_AFF_FLEXNUM) );
      __vdbe_load_string(v, target,
              (aff<=DBSQL_AFF_NONE) ? "none" : affs[aff-DBSQL_AFF_BLOB]);
      break;
    }
#endif /* !defined(DBSQL_UNTESTABLE) */
  }
  return target;
}

/*
** Expression Node callback for __expr_can_return_subtype().  If
** expr is able to return a subtype, set walker->code and abort
** the search.  If expr can never return a subtype, prune search.
**
** The only expressions that can return a subtype are:
**
**    1.  A function
**    2.  The no-op "+" operator
**    3.  A CASE...END expression
**    4.  A CAST() expression
**    5.  A "expr COLLATE colseq" expression.
**
** For any other kind of expression, prune the search.
**
** For case 1, the expression can yield a subtype if the function has
** the DBSQL_RESULT_SUBTYPE property.  Functions can also return
** a subtype (via dbsql_result_value()) if any of the arguments can
** return a subtype.
**
** In all cases 1 through 5, the expression might also return a subtype
** if any operand can return a subtype.
*/
static int __expr_node_can_return_subtype(walker_t *walker, expr_t *expr){
  int n;
  func_def_t *def;
  DBSQL *db;
  if( expr->op==TK_CASE || expr->op==TK_UPLUS 
   || expr->op==TK_COLLATE || expr->op==TK_CAST 
  ){
    return WRC_Continue;
  }
  if( expr->op!=TK_FUNCTION ){
    return WRC_Prune;
  }
  assert( EXPR_USE_X_LIST(expr) );
  db = walker->parser->db;
  n = ALWAYS(expr->x.list) ? expr->x.list->n_expr : 0;
  def = __find_function(db, expr->u.token, n, ENC(db), 0);
  if( NEVER(def==0) || (def->func_flags & DBSQL_RESULT_SUBTYPE)!=0 ){
    walker->code = 1;
    return WRC_Abort;
  }
  return WRC_Continue;
}

/*
** Return TRUE if expression expr is able to return a subtype.
**
** A TRUE return does not guarantee that a subtype will be returned.
** It only indicates that a subtype return is possible.  False positives
** are acceptable as they only disable an optimization.  False negatives,
** on the other hand, can lead to incorrect answers.
*/
static int __expr_can_return_subtype(parser_t *parser, expr_t *expr){
  walker_t w;
  memset(&w, 0, sizeof(w));
  w.parser = parser;
  w.expr_callback = __expr_node_can_return_subtype;
  __walk_expr(&w, expr);
  return w.code;
}


/*
** Check to see if expr is one of the indexed expressions on parser->idx_epr.
** If it is, then resolve the expression by reading from the index and
** return the register into which the value has been read.  If expr is
** not an indexed expression, then return negative.
*/
static DBSQL_NOINLINE int __indexed_expr_lookup(
  parser_t *parser,   /* The parsing context */
  expr_t *expr,     /* The expression to potentially bypass */
  int target       /* Where to store the result of the expression */
){
  indexed_expr_t *p;
  vdbe_t *v;
  for(p=parser->idx_epr; p; p=p->ie_next){
    u_int8_t expr_aff;
    int i_data_cur = p->i_data_cur;
    if( i_data_cur<0 ) continue;
    if( parser->i_self_tab ){
      if( p->i_data_cur!=parser->i_self_tab-1 ) continue;
      i_data_cur = -1;
    }
    if( __expr_compare(0, expr, p->expr, i_data_cur)!=0 ) continue;
    assert( p->aff>=DBSQL_AFF_BLOB && p->aff<=DBSQL_AFF_NUMERIC );
    expr_aff = __expr_affinity(expr);
    if( (expr_aff<=DBSQL_AFF_BLOB && p->aff!=DBSQL_AFF_BLOB)
     || (expr_aff==DBSQL_AFF_TEXT && p->aff!=DBSQL_AFF_TEXT)
     || (expr_aff>=DBSQL_AFF_NUMERIC && p->aff!=DBSQL_AFF_NUMERIC)
    ){
      /* Affinity mismatch on a generated column */
      continue;
    }


    /* Functions that might set a subtype should not be replaced by the
    ** value taken from an expression index if they are themselves an
    ** argument to another scalar function or aggregate. 
    ** https://sqlite.org/forum/forumpost/68d284c86b082c3e */
    if( EXPR_HAS_PROPERTY(expr, EP_SubtArg)
     && __expr_can_return_subtype(parser, expr) 
    ){
      continue;
    }

    v = parser->vdbe;
    assert( v!=0 );
    if( p->maybe_null_row ){
      /* If the index is on a NULL row due to an outer join, then we
      ** cannot extract the value from the index.  The value must be
      ** computed using the original expression. */
      int addr = __vdbe_current_addr(v);
      __vdbe_add_op3(v, OP_IfNullRow, p->i_idx_cur, addr+3, target);
      VDBE_COVERAGE(v);
      __vdbe_add_op3(v, OP_Column, p->i_idx_cur, p->i_idx_col, target);
      VDBE_COMMENT((v, "%s expr-column %d", p->idx_name, p->i_idx_col));
      __vdbe_goto(v, 0);
      p = parser->idx_epr;
      parser->idx_epr = 0;
      __expr_code(parser, expr, target);
      parser->idx_epr = p;
      __vdbe_jump_here(v, addr+2);
    }else{
      __vdbe_add_op3(v, OP_Column, p->i_idx_cur, p->i_idx_col, target);
      VDBE_COMMENT((v, "%s expr-column %d", p->idx_name, p->i_idx_col));
    }
    return target;
  }
  return -1;  /* Not found */
}


/*
** Expression expr is guaranteed to be a TK_COLUMN or equivalent. This
** function checks the parser_t.idx_part_expr list to see if this column
** can be replaced with a constant value. If so, it generates code to
** put the constant value in a register (ideally, but not necessarily, 
** register i_target) and returns the register number.
**
** Or, if the TK_COLUMN cannot be replaced by a constant, zero is 
** returned.
*/
static int __expr_partidx_expr_lookup(parser_t *parser, expr_t *expr, int i_target){
  indexed_expr_t *p;
  for(p=parser->idx_part_expr; p; p=p->ie_next){
    if( expr->i_column==p->i_idx_col && expr->i_table==p->i_data_cur ){
      vdbe_t *v = parser->vdbe;
      int addr = 0;
      int ret;

      if( p->maybe_null_row ){
        addr = __vdbe_add_op1(v, OP_IfNullRow, p->i_idx_cur);
      }
      ret = __expr_code_target(parser, p->expr, i_target);
      __vdbe_add_op4(parser->vdbe, OP_Affinity, ret, 1, 0,
                        (const char*)&p->aff, 1);
      if( addr ){
        __vdbe_jump_here(v, addr);
        __vdbe_change_p3(v, addr, ret);
      }
      return ret;
    }
  }
  return 0;
}

/*
** Generate code that evaluates an AND or OR operator leaving a
** boolean result in a register.  expr is the AND/OR expression.
** Store the result in the "target" register.  Use short-circuit
** evaluation to avoid computing both operands, if possible.
**
** The code generated might require the use of a temporary register.
** If it does, then write the number of that temporary register
** into *tmp_reg.  If not, leave *tmp_reg unchanged.
*/
static DBSQL_NOINLINE int __expr_code_target_and_or(
  parser_t *parser,     /* Parsing context */
  expr_t *expr,       /* AND or OR expression to be coded */
  int target,        /* Put result in this register, guaranteed */
  int *tmp_reg       /* Write a temporary register here */
){
  int op;            /* The opcode.  TK_AND or TK_OR */
  int skip_op;        /* Opcode for the branch that skips one operand */
  int addr_skip;      /* Branch instruction that skips one of the operands */
  int reg_ss = 0;     /* Register holding computed operand when other omitted */
  int r1, r2;        /* Registers for left and right operands, respectively */
  expr_t *alt;        /* Alternative, simplified expression */
  vdbe_t *v;           /* statement being coded */

  assert( expr!=0 );
  op = expr->op;
  assert( op==TK_AND || op==TK_OR );
  assert( TK_AND==OP_And );            testcase( op==TK_AND );
  assert( TK_OR==OP_Or );              testcase( op==TK_OR );
  assert( parser->vdbe!=0 );
  v = parser->vdbe;
  alt = __expr_simplified_and_or(expr);
  if( alt!=expr ){
    r1 = __expr_code_target(parser, alt, target);
    __vdbe_add_op3(v, OP_And, r1, r1, target);
    return target;
  }
  skip_op = op==TK_AND ? OP_IfNot : OP_If;
  if( __expr_eval_rhs_first(expr) ){
    /* Compute the right operand first.  Skip the computation of the left
    ** operand if the right operand fully determines the result */
    r2 = reg_ss = __expr_code_target(parser, expr->right, target);
    addr_skip = __vdbe_add_op1(v, skip_op, r2);
    VDBE_COMMENT((v, "skip left operand"));
    VDBE_COVERAGE(v); 
    r1 = __expr_code_temp(parser, expr->left, tmp_reg);
  }else{
    /* Compute the left operand first */
    r1 = __expr_code_target(parser, expr->left, target);
    if( EXPR_HAS_PROPERTY(expr->right, EP_Subquery) ){
      /* Skip over the computation of the right operand if the right
      ** operand is a subquery and the left operand completely determines
      ** the result */
      reg_ss = r1;
      addr_skip = __vdbe_add_op1(v, skip_op, r1);
      VDBE_COMMENT((v, "skip right operand"));
      VDBE_COVERAGE(v);
    }else{
      addr_skip = reg_ss = 0;
    }
    r2 = __expr_code_temp(parser, expr->right, tmp_reg);
  }
  __vdbe_add_op3(v, op, r2, r1, target);
  testcase( (*tmp_reg)==0 );
  if( addr_skip ){
    __vdbe_add_op2(v, OP_Goto, 0, __vdbe_current_addr(v)+2);
    __vdbe_jump_here(v, addr_skip);
    __vdbe_add_op3(v, OP_Or, reg_ss, reg_ss, target);
    VDBE_COMMENT((v, "short-circut value"));
  }
  return target;
}



/*
** Generate code into the current vdbe_t to evaluate the given
** expression.  Attempt to store the results in register "target".
** Return the register where results are stored.
**
** with_t this routine, there is no guarantee that results will
** be stored in target.  The result might be stored in some other
** register if it is convenient to do so.  The calling function
** must check the return code and move the results to the desired
** register.
*/
int __expr_code_target(parser_t *parser, expr_t *expr, int target){
  vdbe_t *v = parser->vdbe;  /* The VM under construction */
  int op;                   /* The opcode being coded */
  int in_reg = target;       /* Results stored in register in_reg */
  int reg_free1 = 0;         /* If non-zero free this temporary register */
  int reg_free2 = 0;         /* If non-zero free this temporary register */
  int r1, r2;               /* Various register numbers */
  expr_t temp_x;               /* Temporary expression node */
  int p5 = 0;

  assert( target>0 && target<=parser->n_mem );
  assert( v!=0 );

expr_code_doover:
  if( expr==0 ){
    op = TK_NULL;
  }else if( parser->idx_epr!=0
   && !EXPR_HAS_PROPERTY(expr, EP_Leaf)
   && (r1 = __indexed_expr_lookup(parser, expr, target))>=0
  ){
    return r1;
  }else{
    assert( !EXPR_HAS_VVA_PROPERTY(expr,EP_Immutable) );
    op = expr->op;
  }
  assert( op!=TK_ORDER );
  switch( op ){
    case TK_AGG_COLUMN: {
      agg_info_t *agg_info = expr->agg_info;
      struct agg_info_col_t *col;
      assert( agg_info!=0 );
      assert( expr->i_agg>=0 );
      if( expr->i_agg>=agg_info->n_column ){
        /* Happens when the left table of a RIGHT JOIN is null and
        ** is using an expression index */
        __vdbe_add_op2(v, OP_Null, 0, target);
#ifdef DBSQL_VDBE_COVERAGE
        /* Verify that the OP_Null above is exercised by tests
        ** tag-20230325-2 */
        __vdbe_add_op3(v, OP_NotNull, target, 1, 20230325);
        VDBE_COVERAGE_NEVER_TAKEN(v);
#endif
        break;
      }
      col = &agg_info->cols[expr->i_agg];
      if( !agg_info->direct_mode ){
        return AGG_INFO_COLUMN_REG(agg_info, expr->i_agg);
      }else if( agg_info->use_sorting_idx ){
        table_t *tab = col->tab;
        __vdbe_add_op3(v, OP_Column, agg_info->sorting_idx_p_tab,
                              col->i_sorter_column, target);
        if( tab==0 ){
          /* No comment added */
        }else if( col->i_column<0 ){
          VDBE_COMMENT((v,"%s.rowid",tab->name));
        }else{
          VDBE_COMMENT((v,"%s.%s",
              tab->name, tab->cols[col->i_column].cn_name));
          if( tab->cols[col->i_column].affinity==DBSQL_AFF_REAL ){
            __vdbe_add_op1(v, OP_RealAffinity, target);
          }
        }
        return target;
      }else if( expr->y.tab==0 ){
        /* This case happens when the argument to an aggregate function
        ** is rewritten by __aggregate_convert_indexed_expr_ref_to_column() */
        __vdbe_add_op3(v, OP_Column, expr->i_table, expr->i_column, target);
        return target;
      }
      /* Otherwise, fall thru into the TK_COLUMN case */
      /* no break */ deliberate_fall_through
    }
    case TK_COLUMN: {
      int i_tab = expr->i_table;
      int i_reg;
      if( EXPR_HAS_PROPERTY(expr, EP_FixedCol) ){
        /* This COLUMN expression is really a constant due to WHERE clause
        ** constraints, and that constant is coded by the expr->left
        ** expression.  However, make sure the constant has the correct
        ** datatype by applying the Affinity of the table column to the
        ** constant.
        */
        int aff;
        i_reg = __expr_code_target(parser, expr->left,target);
        assert( EXPR_USE_Y_TAB(expr) );
        assert( expr->y.tab!=0 );
        aff = __table_column_affinity(expr->y.tab, expr->i_column);
        if( aff>DBSQL_AFF_BLOB ){
          static const char z_aff[] = "B\000C\000D\000E\000F";
          assert( DBSQL_AFF_BLOB=='A' );
          assert( DBSQL_AFF_TEXT=='B' );
          __vdbe_add_op4(v, OP_Affinity, i_reg, 1, 0,
                            &z_aff[(aff-'B')*2], P4_STATIC);
        }
        return i_reg;
      }
      if( i_tab<0 ){
        if( parser->i_self_tab<0 ){
          /* Other columns in the same row for CHECK constraints or
          ** generated columns or for inserting into partial index.
          ** The row is unpacked into registers beginning at
          ** 0-(parser->i_self_tab).  The rowid (if any) is in a register
          ** immediately prior to the first column.
          */
          column_t *col;
          table_t *tab;
          int i_src;
          int i_col = expr->i_column;
          assert( EXPR_USE_Y_TAB(expr) );
          tab = expr->y.tab;
          assert( tab!=0 );
          assert( i_col>=XN_ROWID );
          assert( i_col<tab->n_col );
          if( i_col<0 ){
            return -1-parser->i_self_tab;
          }
          col = tab->cols + i_col;
          testcase( i_col!=__table_column_to_storage(tab,i_col) );
          i_src = __table_column_to_storage(tab, i_col) - parser->i_self_tab;
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
          if( col->col_flags & COLFLAG_GENERATED ){
            if( col->col_flags & COLFLAG_BUSY ){
              __error_msg(parser, "generated column loop on \"%s\"",
                              col->cn_name);
              return 0;
            }
            col->col_flags |= COLFLAG_BUSY;
            if( col->col_flags & COLFLAG_NOTAVAIL ){
              __expr_code_generated_column(parser, tab, col, i_src);
            }
            col->col_flags &= ~(COLFLAG_BUSY|COLFLAG_NOTAVAIL);
            return i_src;
          }else
#endif /* DBSQL_OMIT_GENERATED_COLUMNS */
          if( col->affinity==DBSQL_AFF_REAL ){
            __vdbe_add_op2(v, OP_SCopy, i_src, target);
            __vdbe_add_op1(v, OP_RealAffinity, target);
            return target;
          }else{
            return i_src;
          }
        }else{
          /* Coding an expression that is part of an index where column names
          ** in the index refer to the table to which the index belongs */
          i_tab = parser->i_self_tab - 1;
        }
      }
      else if( parser->idx_part_expr 
       && 0!=(r1 = __expr_partidx_expr_lookup(parser, expr, target))
      ){
        return r1;
      }
      assert( EXPR_USE_Y_TAB(expr) );
      assert( expr->y.tab!=0 );
      i_reg = __expr_code_get_column(parser, expr->y.tab,
                               expr->i_column, i_tab, target,
                               expr->op2);
      return i_reg;
    }
    case TK_INTEGER: {
      __code_integer(parser, expr, 0, target);
      return target;
    }
    case TK_TRUEFALSE: {
      __vdbe_add_op2(v, OP_Integer, __expr_truth_value(expr), target);
      return target;
    }
#ifndef DBSQL_OMIT_FLOATING_POINT
    case TK_FLOAT: {
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __code_real(v, expr->u.token, 0, target);
      return target;
    }
#endif
    case TK_STRING: {
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __vdbe_load_string(v, target, expr->u.token);
      return target;
    }
    case TK_NULLS: {
      /* Set a range of registers to NULL.  expr->y.n_reg registers starting
      ** with target */
      __vdbe_add_op3(v, OP_Null, 0, target, target + expr->y.n_reg - 1);
      return target;
    }
    default: {
      /* Make NULL the default case so that if a bug causes an illegal
      ** expr_t node to be passed into this function, it will be handled
      ** sanely and not crash.  But keep the assert() to bring the problem
      ** to the attention of the developers. */
      assert( op==TK_NULL || op==TK_ERROR || parser->db->malloc_failed );
      __vdbe_add_op2(v, OP_Null, 0, target);
      return target;
    }
#ifndef DBSQL_OMIT_BLOB_LITERAL
    case TK_BLOB: {
      int n;
      const char *z;
      char *blob;
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      assert( expr->u.token[0]=='x' || expr->u.token[0]=='X' );
      assert( expr->u.token[1]=='\'' );
      z = &expr->u.token[2];
      n = __strlen30(z) - 1;
      assert( z[n]=='\'' );
      blob = __hex_to_blob(__vdbe_db(v), z, n);
      __vdbe_add_op4(v, OP_Blob, n/2, target, 0, blob, P4_DYNAMIC);
      return target;
    }
#endif
    case TK_VARIABLE: {
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      assert( expr->u.token!=0 );
      assert( expr->u.token[0]!=0 );
      __vdbe_add_op2(v, OP_Variable, expr->i_column, target);
      return target;
    }
    case TK_REGISTER: {
      return expr->i_table;
    }
#ifndef DBSQL_OMIT_CAST
    case TK_CAST: {
      /* Expressions of the form:   CAST(left AS token) */
      __expr_code(parser, expr->left, target);
      assert( in_reg==target );
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __vdbe_add_op2(v, OP_Cast, target,
                        __affinity_type(expr->u.token, 0));
      return in_reg;
    }
#endif /* DBSQL_OMIT_CAST */
    case TK_IS:
    case TK_ISNOT:
      op = (op==TK_IS) ? TK_EQ : TK_NE;
      p5 = DBSQL_NULLEQ;
      /* no break */ deliberate_fall_through
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE:
    case TK_NE:
    case TK_EQ: {
      expr_t *left = expr->left;
      int addr_is_null = 0;
      if( __expr_is_vector(left) ){
        __code_vector_compare(parser, expr, target, op, p5);
      }else{
        if( EXPR_HAS_PROPERTY(expr, EP_Subquery) && p5!=DBSQL_NULLEQ ){
          addr_is_null = __expr_compute_operands(parser, expr,
                                     &r1, &r2, &reg_free1, &reg_free2);
        }else{
          r1 = __expr_code_temp(parser, expr->left, &reg_free1);
          r2 = __expr_code_temp(parser, expr->right, &reg_free2);
        }
        __vdbe_add_op2(v, OP_Integer, 1, in_reg);
        __code_compare(parser, left, expr->right, op, r1, r2,
            __vdbe_current_addr(v)+2, p5,
            EXPR_HAS_PROPERTY(expr,EP_Commuted));
        assert(TK_LT==OP_Lt); testcase(op==OP_Lt); VDBE_COVERAGE_IF(v,op==OP_Lt);
        assert(TK_LE==OP_Le); testcase(op==OP_Le); VDBE_COVERAGE_IF(v,op==OP_Le);
        assert(TK_GT==OP_Gt); testcase(op==OP_Gt); VDBE_COVERAGE_IF(v,op==OP_Gt);
        assert(TK_GE==OP_Ge); testcase(op==OP_Ge); VDBE_COVERAGE_IF(v,op==OP_Ge);
        assert(TK_EQ==OP_Eq); testcase(op==OP_Eq); VDBE_COVERAGE_IF(v,op==OP_Eq);
        assert(TK_NE==OP_Ne); testcase(op==OP_Ne); VDBE_COVERAGE_IF(v,op==OP_Ne);
        if( p5==DBSQL_NULLEQ ){
          __vdbe_add_op2(v, OP_Integer, 0, in_reg);
        }else{
          __vdbe_add_op3(v, OP_ZeroOrNull, r1, in_reg, r2);
          if( addr_is_null ){
            __vdbe_add_op2(v, OP_Goto, 0, __vdbe_current_addr(v)+2);
            __vdbe_jump_here(v, addr_is_null);
            __vdbe_add_op2(v, OP_Null, 0, in_reg);
          }
        }
        testcase( reg_free1==0 );
        testcase( reg_free2==0 );
      }
      break;
    }
    case TK_AND:
    case TK_OR: {
      in_reg = __expr_code_target_and_or(parser, expr, target, &reg_free1);
      break;
    }
    case TK_PLUS:
    case TK_STAR:
    case TK_MINUS:
    case TK_REM:
    case TK_BITAND:
    case TK_BITOR:
    case TK_SLASH:
    case TK_LSHIFT:
    case TK_RSHIFT:
    case TK_CONCAT: {
      int addr_is_null;
      assert( TK_PLUS==OP_Add );           testcase( op==TK_PLUS );
      assert( TK_MINUS==OP_Subtract );     testcase( op==TK_MINUS );
      assert( TK_REM==OP_Remainder );      testcase( op==TK_REM );
      assert( TK_BITAND==OP_BitAnd );      testcase( op==TK_BITAND );
      assert( TK_BITOR==OP_BitOr );        testcase( op==TK_BITOR );
      assert( TK_SLASH==OP_Divide );       testcase( op==TK_SLASH );
      assert( TK_LSHIFT==OP_ShiftLeft );   testcase( op==TK_LSHIFT );
      assert( TK_RSHIFT==OP_ShiftRight );  testcase( op==TK_RSHIFT );
      assert( TK_CONCAT==OP_Concat );      testcase( op==TK_CONCAT );
      if( EXPR_HAS_PROPERTY(expr, EP_Subquery) ){
        addr_is_null = __expr_compute_operands(parser, expr,
                                   &r1, &r2, &reg_free1, &reg_free2);
      }else{
        r1 = __expr_code_temp(parser, expr->left, &reg_free1);
        r2 = __expr_code_temp(parser, expr->right, &reg_free2);
        addr_is_null = 0;
      }
      __vdbe_add_op3(v, op, r2, r1, target);
      testcase( reg_free1==0 );
      testcase( reg_free2==0 );
      if( addr_is_null ){
        __vdbe_add_op2(v, OP_Goto, 0, __vdbe_current_addr(v)+2);
        __vdbe_jump_here(v, addr_is_null);
        __vdbe_add_op2(v, OP_Null, 0, target);
        VDBE_COMMENT((v, "short-circut value"));
      }
      break;
    }
    case TK_UMINUS: {
      expr_t *left = expr->left;
      assert( left );
      if( left->op==TK_INTEGER ){
        __code_integer(parser, left, 1, target);
        return target;
#ifndef DBSQL_OMIT_FLOATING_POINT
      }else if( left->op==TK_FLOAT ){
        assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
        __code_real(v, left->u.token, 1, target);
        return target;
#endif
      }else{
        temp_x.op = TK_INTEGER;
        temp_x.flags = EP_IntValue|EP_TokenOnly;
        temp_x.u.i_value = 0;
        EXPR_CLEAR_VVA_PROPERTIES(&temp_x);
        r1 = __expr_code_temp(parser, &temp_x, &reg_free1);
        r2 = __expr_code_temp(parser, expr->left, &reg_free2);
        __vdbe_add_op3(v, OP_Subtract, r2, r1, target);
        testcase( reg_free2==0 );
      }
      break;
    }
    case TK_BITNOT:
    case TK_NOT: {
      assert( TK_BITNOT==OP_BitNot );   testcase( op==TK_BITNOT );
      assert( TK_NOT==OP_Not );         testcase( op==TK_NOT );
      r1 = __expr_code_temp(parser, expr->left, &reg_free1);
      testcase( reg_free1==0 );
      __vdbe_add_op2(v, op, r1, in_reg);
      break;
    }
    case TK_TRUTH: {
      int is_true;    /* IS TRUE or IS NOT TRUE */
      int normal;   /* IS TRUE or IS FALSE */
      r1 = __expr_code_temp(parser, expr->left, &reg_free1);
      testcase( reg_free1==0 );
      is_true = __expr_truth_value(expr->right);
      normal = expr->op2==TK_IS;
      testcase( is_true && normal);
      testcase( !is_true && normal);
      __vdbe_add_op4_int(v, OP_IsTrue, r1, in_reg, !is_true, is_true ^ normal);
      break;
    }
    case TK_ISNULL:
    case TK_NOTNULL: {
      int addr;
      assert( TK_ISNULL==OP_IsNull );   testcase( op==TK_ISNULL );
      assert( TK_NOTNULL==OP_NotNull ); testcase( op==TK_NOTNULL );
      __vdbe_add_op2(v, OP_Integer, 1, target);
      r1 = __expr_code_temp(parser, expr->left, &reg_free1);
      testcase( reg_free1==0 );
      addr = __vdbe_add_op1(v, op, r1);
      VDBE_COVERAGE_IF(v, op==TK_ISNULL);
      VDBE_COVERAGE_IF(v, op==TK_NOTNULL);
      __vdbe_add_op2(v, OP_Integer, 0, target);
      __vdbe_jump_here(v, addr);
      break;
    }
    case TK_AGG_FUNCTION: {
      agg_info_t *info = expr->agg_info;
      if( info==0
       || NEVER(expr->i_agg<0)
       || NEVER(expr->i_agg>=info->n_func)
      ){
        assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
        __error_msg(parser, "misuse of aggregate: %#T()", expr);
      }else{
        return AGG_INFO_FUNC_REG(info, expr->i_agg);
      }
      break;
    }
    case TK_FUNCTION: {
      expr_list_t *farg;       /* List of function arguments */
      int n_farg;             /* Number of function arguments */
      func_def_t *def;         /* The function definition object */
      const char *id;       /* The function name */
      u_int32_t const_mask = 0;     /* Mask of function arguments that are constant */
      int i;                 /* Loop counter */
      DBSQL *db = parser->db;  /* The database connection */
      u_int8_t enc = ENC(db);      /* The text encoding used by this database */
      coll_seq_t *coll = 0;    /* A collating sequence */

#ifndef DBSQL_OMIT_WINDOWFUNC
      if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
        return expr->y.win->reg_result;
      }
#endif

      if( CONST_FACTOR_OK(parser)
       && __expr_is_constant_not_join(parser,expr)
      ){
        /* SQL functions can be expensive. So try to avoid running them
        ** multiple times if we know they always give the same result */
        return __expr_code_run_just_once(parser, expr, -1);
      }
      assert( !EXPR_HAS_PROPERTY(expr, EP_TokenOnly) );
      assert( EXPR_USE_X_LIST(expr) );
      farg = expr->x.list;
      n_farg = farg ? farg->n_expr : 0;
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      id = expr->u.token;
      def = __find_function(db, id, n_farg, enc, 0);
#ifdef DBSQL_ENABLE_UNKNOWN_SQL_FUNCTION
      if( def==0 && parser->explain ){
        def = __find_function(db, "unknown", n_farg, enc, 0);
      }
#endif
      if( def==0 || def->finalize!=0 ){
        __error_msg(parser, "unknown function: %#T()", expr);
        break;
      }
      if( (def->func_flags & DBSQL_FUNC_INLINE)!=0 && ALWAYS(farg!=0) ){
        assert( (def->func_flags & DBSQL_FUNC_UNSAFE)==0 );
        assert( (def->func_flags & DBSQL_FUNC_DIRECT)==0 );
        return __expr_code_inline_function(parser, farg,
             DBSQL_PTR_TO_INT(def->user_data), target);
      }else if( def->func_flags & (DBSQL_FUNC_DIRECT|DBSQL_FUNC_UNSAFE) ){
        __expr_function_usable(parser, expr, def);
      }

      for(i=0; i<n_farg; i++){
        if( i<32 && __expr_is_constant(parser, farg->a[i].expr) ){
          testcase( i==31 );
          const_mask |= MASKBIT32(i);
        }
        if( (def->func_flags & DBSQL_FUNC_NEEDCOLL)!=0 && !coll ){
          coll = __expr_coll_seq(parser, farg->a[i].expr);
        }
      }
      if( farg ){
        if( const_mask ){
          r1 = parser->n_mem+1;
          parser->n_mem += n_farg;
        }else{
          r1 = __get_temp_range(parser, n_farg);
        }

        /* For length() and typeof() and octet_length() functions,
        ** set the P5 parameter to the OP_Column opcode to OPFLAG_LENGTHARG
        ** or OPFLAG_TYPEOFARG or OPFLAG_BYTELENARG respectively, to avoid
        ** unnecessary data loading.
        */
        if( (def->func_flags & (DBSQL_FUNC_LENGTH|DBSQL_FUNC_TYPEOF))!=0 ){
          u_int8_t expr_op;
          assert( n_farg==1 );
          assert( farg->a[0].expr!=0 );
          expr_op = farg->a[0].expr->op;
          if( expr_op==TK_COLUMN || expr_op==TK_AGG_COLUMN ){
            assert( DBSQL_FUNC_LENGTH==OPFLAG_LENGTHARG );
            assert( DBSQL_FUNC_TYPEOF==OPFLAG_TYPEOFARG );
            assert( DBSQL_FUNC_BYTELEN==OPFLAG_BYTELENARG );
            assert( (OPFLAG_LENGTHARG|OPFLAG_TYPEOFARG)==OPFLAG_BYTELENARG );
            testcase( (def->func_flags & OPFLAG_BYTELENARG)==OPFLAG_LENGTHARG );
            testcase( (def->func_flags & OPFLAG_BYTELENARG)==OPFLAG_TYPEOFARG );
            testcase( (def->func_flags & OPFLAG_BYTELENARG)==OPFLAG_BYTELENARG);
            farg->a[0].expr->op2 = def->func_flags & OPFLAG_BYTELENARG;
          }
        }

        __expr_code_expr_list(parser, farg, r1, 0, DBSQL_ECEL_FACTOR);
      }else{
        r1 = 0;
      }
#ifndef DBSQL_OMIT_VIRTUALTABLE
      /* Possibly overload the function if the first argument is
      ** a virtual table column.
      **
      ** For infix functions (LIKE, GLOB, REGEXP, and MATCH) use the
      ** second argument, not the first, as the argument to test to
      ** see if it is a column in a virtual table.  This is done because
      ** the left operand of infix functions (the operand we want to
      ** control overloading) ends up as the second argument to the
      ** function.  The expression "A glob B" is equivalent to
      ** "glob(B,A).  We want to use the A in "A glob B" to test
      ** for function overloading.  But we use the B term in "glob(B,A)".
      */
      if( n_farg>=2 && EXPR_HAS_PROPERTY(expr, EP_InfixFunc) ){
        def = __vtab_overload_function(db, def, n_farg, farg->a[1].expr);
      }else if( n_farg>0 ){
        def = __vtab_overload_function(db, def, n_farg, farg->a[0].expr);
      }
#endif
      if( def->func_flags & DBSQL_FUNC_NEEDCOLL ){
        if( !coll ) coll = db->dflt_coll;
        __vdbe_add_op4(v, OP_CollSeq, 0, 0, 0, (char *)coll, P4_COLLSEQ);
      }
      __vdbe_add_function_call(parser, const_mask, r1, target, n_farg,
                                 def, expr->op2);
      if( n_farg ){
        if( const_mask==0 ){
          __release_temp_range(parser, r1, n_farg);
        }else{
          __vdbe_release_registers(parser, r1, n_farg, const_mask, 1);
        }
      }
      return target;
    }
#ifndef DBSQL_OMIT_SUBQUERY
    case TK_EXISTS:
    case TK_SELECT: {
      int n_col;
      testcase( op==TK_EXISTS );
      testcase( op==TK_SELECT );
      if( parser->db->malloc_failed ){
        return 0;
      }else if( op==TK_SELECT
             && ALWAYS( EXPR_USE_X_SELECT(expr) )
             && (n_col = expr->x.p_select->e_list->n_expr)!=1
      ){
        __subselect_error(parser, n_col, 1);
      }else{
        return __code_subselect(parser, expr);
      }
      break;
    }
    case TK_SELECT_COLUMN: {
      int n;
      expr_t *left = expr->left;
      if( left->i_table==0 || parser->within_rj_subrtn > left->op2 ){
        left->i_table = __code_subselect(parser, left);
        left->op2 = parser->within_rj_subrtn;
      }
      assert( left->op==TK_SELECT || left->op==TK_ERROR );
      n = __expr_vector_size(left);
      if( expr->i_table!=n ){
        __error_msg(parser, "%d columns assigned %d values",
                                expr->i_table, n);
      }
      return left->i_table + expr->i_column;
    }
    case TK_IN: {
      int dest_if_false = __vdbe_make_label(parser);
      int dest_if_null = __vdbe_make_label(parser);
      __vdbe_add_op2(v, OP_Null, 0, target);
      __expr_code_in(parser, expr, dest_if_false, dest_if_null);
      __vdbe_add_op2(v, OP_Integer, 1, target);
      __vdbe_resolve_label(v, dest_if_false);
      __vdbe_add_op2(v, OP_AddImm, target, 0);
      __vdbe_resolve_label(v, dest_if_null);
      return target;
    }
#endif /* DBSQL_OMIT_SUBQUERY */


    /*
    **    x BETWEEN y AND z
    **
    ** This is equivalent to
    **
    **    x>=y AND x<=z
    **
    ** X is stored in expr->left.
    ** Y is stored in expr->list->a[0].expr.
    ** Z is stored in expr->list->a[1].expr.
    */
    case TK_BETWEEN: {
      __expr_code_between(parser, expr, target, 0, 0);
      return target;
    }
    case TK_COLLATE: {
      if( !EXPR_HAS_PROPERTY(expr, EP_Collate) ){
        /* A TK_COLLATE expr_t node without the EP_Collate tag is a so-called
        ** "SOFT-COLLATE" that is added to constraints that are pushed down
        ** from outer queries into sub-queries by the WHERE-clause push-down
        ** optimization. Clear subtypes as subtypes may not cross a subquery
        ** boundary.
        */
        assert( expr->left );
        __expr_code(parser, expr->left, target);
        __vdbe_add_op1(v, OP_ClrSubtype, target);
        return target;
      }else{
        expr = expr->left;
        goto expr_code_doover; /* 2018-04-28: Prevent deep recursion. */
      }
    }
    case TK_SPAN:
    case TK_UPLUS: {
      expr = expr->left;
      goto expr_code_doover; /* 2018-04-28: Prevent deep recursion. OSSFuzz. */
    }

    case TK_TRIGGER: {
      /* If the opcode is TK_TRIGGER, then the expression is a reference
      ** to a column in the new.* or old.* pseudo-tables available to
      ** trigger programs. In this case expr_t.i_table is set to 1 for the
      ** new.* pseudo-table, or 0 for the old.* pseudo-table. expr_t.i_column
      ** is set to the column of the pseudo-table to read, or to -1 to
      ** read the rowid field.
      **
      ** The expression is implemented using an OP_Param opcode. The p1
      ** parameter is set to 0 for an old.rowid reference, or to (i+1)
      ** to reference another column of the old.* pseudo-table, where
      ** i is the index of the column. For a new.rowid reference, p1 is
      ** set to (n+1), where n is the number of columns in each pseudo-table.
      ** For a reference to any other column in the new.* pseudo-table, p1
      ** is set to (n+2+i), where n and i are as defined previously. For
      ** example, if the table on which triggers are being fired is
      ** declared as:
      **
      **   CREATE TABLE t1(a, b);
      **
      ** Then p1 is interpreted as follows:
      **
      **   p1==0   ->    old.rowid     p1==3   ->    new.rowid
      **   p1==1   ->    old.a         p1==4   ->    new.a
      **   p1==2   ->    old.b         p1==5   ->    new.b      
      */
      table_t *tab;
      int i_col;
      int p1;

      assert( EXPR_USE_Y_TAB(expr) );
      tab = expr->y.tab;
      i_col = expr->i_column;
      p1 = expr->i_table * (tab->n_col+1) + 1
                     + __table_column_to_storage(tab, i_col);

      assert( expr->i_table==0 || expr->i_table==1 );
      assert( i_col>=-1 && i_col<tab->n_col );
      assert( tab->i_p_key<0 || i_col!=tab->i_p_key );
      assert( p1>=0 && p1<(tab->n_col*2+2) );

      __vdbe_add_op2(v, OP_Param, p1, target);
      VDBE_COMMENT((v, "r[%d]=%s.%s", target,
        (expr->i_table ? "new" : "old"),
        (expr->i_column<0 ? "rowid" : expr->y.tab->cols[i_col].cn_name)
      ));

#ifndef DBSQL_OMIT_FLOATING_POINT
      /* If the column has REAL affinity, it may currently be stored as an
      ** integer. Use OP_RealAffinity to make sure it is really real.
      **
      ** EVIDENCE-OF: R-60985-57662 dbsql will convert the value back to
      ** floating point when extracting it from the record.  */
      if( i_col>=0 && tab->cols[i_col].affinity==DBSQL_AFF_REAL ){
        __vdbe_add_op1(v, OP_RealAffinity, target);
      }
#endif
      break;
    }

    case TK_VECTOR: {
      __error_msg(parser, "row value misused");
      break;
    }

    /* TK_IF_NULL_ROW expr_t nodes are inserted ahead of expressions
    ** that derive from the right-hand table of a LEFT JOIN.  The
    ** expr_t.i_table value is the table number for the right-hand table.
    ** The expression is only evaluated if that table is not currently
    ** on a LEFT JOIN NULL row.
    */
    case TK_IF_NULL_ROW: {
      int addr_inr;
      u_int8_t ok_const_factor = parser->ok_const_factor;
      agg_info_t *agg_info = expr->agg_info;
      if( agg_info ){
        assert( expr->i_agg>=0 && expr->i_agg<agg_info->n_column );
        if( !agg_info->direct_mode ){
          in_reg = AGG_INFO_COLUMN_REG(agg_info, expr->i_agg);
          break;
        }
        if( expr->agg_info->use_sorting_idx ){
          __vdbe_add_op3(v, OP_Column, agg_info->sorting_idx_p_tab,
                            agg_info->cols[expr->i_agg].i_sorter_column,
                            target);
          in_reg = target;
          break;
        }
      }
      addr_inr = __vdbe_add_op3(v, OP_IfNullRow, expr->i_table, 0, target);
      /* The OP_IfNullRow opcode above can overwrite the result register with
      ** NULL.  So we have to ensure that the result register is not a value
      ** that is suppose to be a constant.  Two defenses are needed:
      **   (1)  Temporarily disable factoring of constant expressions
      **   (2)  Make sure the computed value really is stored in register
      **        "target" and not someplace else.
      */
      parser->ok_const_factor = 0;   /* note (1) above */
      __expr_code(parser, expr->left, target);
      assert( target==in_reg );
      parser->ok_const_factor = ok_const_factor;
      __vdbe_jump_here(v, addr_inr);
      break;
    }

    /*
    ** Form A:
    **   CASE x WHEN e1 THEN r1 WHEN e2 THEN r2 ... WHEN n THEN r_n ELSE y END
    **
    ** Form B:
    **   CASE WHEN e1 THEN r1 WHEN e2 THEN r2 ... WHEN n THEN r_n ELSE y END
    **
    ** Form A is can be transformed into the equivalent form B as follows:
    **   CASE WHEN x=e1 THEN r1 WHEN x=e2 THEN r2 ...
    **        WHEN x=n THEN r_n ELSE y END
    **
    ** X (if it exists) is in expr->left.
    ** Y is in the last element of expr->x.list if expr->x.list->n_expr is
    ** odd.  The Y is also optional.  If the number of elements in x.list
    ** is even, then Y is omitted and the "otherwise" result is NULL.
    ** Ei is in expr->list->a[i*2] and Ri is expr->list->a[i*2+1].
    **
    ** The result of the expression is the Ri for the first matching Ei,
    ** or if there is no matching Ei, the ELSE term Y, or if there is
    ** no ELSE term, NULL.
    */
    case TK_CASE: {
      int end_label;                     /* GOTO label for end of CASE stmt */
      int next_case;                     /* GOTO label for next WHEN clause */
      int n_expr;                        /* 2x number of WHEN terms */
      int i;                            /* Loop counter */
      expr_list_t *e_list;                 /* List of WHEN terms */
      struct expr_list_item_t *listelems;  /* Array of WHEN terms */
      expr_t op_compare;                   /* The X==Ei expression */
      expr_t *x;                         /* The X expression */
      expr_t *test = 0;                  /* X==Ei (form A) or just Ei (form B) */
      expr_t *del = 0;
      DBSQL *db = parser->db;

      assert( EXPR_USE_X_LIST(expr) && expr->x.list!=0 );
      assert(expr->x.list->n_expr > 0);
      e_list = expr->x.list;
      listelems = e_list->a;
      n_expr = e_list->n_expr;
      end_label = __vdbe_make_label(parser);
      if( (x = expr->left)!=0 ){
        del = __expr_dup(db, x, 0);
        if( db->malloc_failed ){
          __expr_delete(db, del);
          break;
        }
        testcase( x->op==TK_COLUMN );
        __expr_to_register(del, __expr_code_vector(parser, del, &reg_free1));
        testcase( reg_free1==0 );
        memset(&op_compare, 0, sizeof(op_compare));
        op_compare.op = TK_EQ;
        op_compare.left = del;
        test = &op_compare;
        /* Ticket b351d95f9cd5ef17e9d9dbae18f5ca8611190001:
        ** The value in reg_free1 might get SCopy-ed into the file result.
        ** So make sure that the reg_free1 register is not reused for other
        ** purposes and possibly overwritten.  */
        reg_free1 = 0;
      }
      for(i=0; i<n_expr-1; i=i+2){
        if( x ){
          assert( test!=0 );
          op_compare.right = listelems[i].expr;
        }else{
          test = listelems[i].expr;
        }
        next_case = __vdbe_make_label(parser);
        testcase( test->op==TK_COLUMN );
        __expr_if_false(parser, test, next_case, DBSQL_JUMPIFNULL);
        testcase( listelems[i+1].expr->op==TK_COLUMN );
        __expr_code(parser, listelems[i+1].expr, target);
        __vdbe_goto(v, end_label);
        __vdbe_resolve_label(v, next_case);
      }
      if( (n_expr&1)!=0 ){
        __expr_code(parser, e_list->a[n_expr-1].expr, target);
      }else{
        __vdbe_add_op2(v, OP_Null, 0, target);
      }
      __expr_delete(db, del);
      __set_do_not_merge_flag_on_copy(v);
      __vdbe_resolve_label(v, end_label);
      break;
    }
#ifndef DBSQL_OMIT_TRIGGER
    case TK_RAISE: {
      assert( expr->aff_expr==OE_Rollback
           || expr->aff_expr==OE_Abort
           || expr->aff_expr==OE_Fail
           || expr->aff_expr==OE_Ignore
      );
      if( !parser->trigger_tab && !parser->nested ){
        __error_msg(parser,
                       "RAISE() may only be used within a trigger-program");
        return 0;
      }
      if( expr->aff_expr==OE_Abort ){
        __may_abort(parser);
      }
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      if( expr->aff_expr==OE_Ignore ){
        __vdbe_add_op2(v, OP_Halt, DBSQL_OK, OE_Ignore);
        VDBE_COVERAGE(v);
      }else{
        r1 = __expr_code_temp(parser, expr->left, &reg_free1);
        __vdbe_add_op3(v, OP_Halt, 
             parser->trigger_tab ? DBSQL_CONSTRAINT_TRIGGER : DBSQL_ERROR,
             expr->aff_expr, r1);
      }
      break;
    }
#endif
  }
  __release_temp_reg(parser, reg_free1);
  __release_temp_reg(parser, reg_free2);
  return in_reg;
}

/*
** Generate code that will evaluate expression expr just one time
** per prepared statement execution.
**
** If the expression uses functions (that might throw an exception) then
** guard them with an OP_Once opcode to ensure that the code is only executed
** once. If no functions are involved, then factor the code out and put it at
** the end of the prepared statement in the initialization section.
**
** If reg_dest>0 then the result is always stored in that register and the
** result is not reusable.  If reg_dest<0 then this routine is free to
** store the value wherever it wants.  The register where the expression
** is stored is returned.  When reg_dest<0, two identical expressions might
** code to the same register, if they do not contain function calls and hence
** are factored out into the initialization section at the end of the
** prepared statement.
*/
int __expr_code_run_just_once(
  parser_t *parser,    /* Parsing context */
  expr_t *expr,      /* The expression to code when the VDBE initializes */
  int reg_dest       /* Store the value in this register */
){
  expr_list_t *p;
  assert( CONST_FACTOR_OK(parser) );
  assert( reg_dest!=0 );
  p = parser->const_expr;
  if( reg_dest<0 && p ){
    struct expr_list_item_t *item;
    int i;
    for(item=p->a, i=p->n_expr; i>0; item++, i--){
      if( item->fg.reusable
       && __expr_compare(0,item->expr,expr,-1)==0
      ){
        return item->u.i_const_expr_reg;
      }
    }
  }
  expr = __expr_dup(parser->db, expr, 0);
  if( expr!=0 && EXPR_HAS_PROPERTY(expr, EP_HasFunc) ){
    vdbe_t *v = parser->vdbe;
    int addr;
    assert( v );
    addr = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
    parser->ok_const_factor = 0;
    if( !parser->db->malloc_failed ){
      if( reg_dest<0 ) reg_dest = ++parser->n_mem;
      __expr_code(parser, expr, reg_dest);
    }
    parser->ok_const_factor = 1;
    __expr_delete(parser->db, expr);
    __vdbe_jump_here(v, addr);
  }else{
    p = __expr_list_append(parser, p, expr);
    if( p ){
       struct expr_list_item_t *item = &p->a[p->n_expr-1];
       item->fg.reusable = reg_dest<0;
       if( reg_dest<0 ) reg_dest = ++parser->n_mem;
       item->u.i_const_expr_reg = reg_dest;
    }
    parser->const_expr = p;
  }
  return reg_dest;
}

/*
** Make arrangements to invoke OP_Null on a range of registers
** during initialization.
*/
DBSQL_NOINLINE void __expr_null_register_range(
  parser_t *parser,   /* Parsing context */
  int i_reg,        /* First register to set to NULL */
  int n_reg         /* Number of sequential registers to NULL out */
){
  u_int8_t ok_const_factor = parser->ok_const_factor;
  expr_t t;
  memset(&t, 0, sizeof(t));
  t.op = TK_NULLS;
  t.y.n_reg = n_reg;
  parser->ok_const_factor = 1;
  __expr_code_run_just_once(parser, &t, i_reg);
  parser->ok_const_factor = ok_const_factor;
}

/*
** Generate code to evaluate an expression and store the results
** into a register.  Return the register number where the results
** are stored.
**
** If the register is a temporary register that can be deallocated,
** then write its number into *reg.  If the result register is not
** a temporary, then set *reg to zero.
**
** If expr is a constant, then this routine might generate this
** code to fill the register in the initialization section of the
** VDBE program, in order to factor it out of the evaluation loop.
*/
int __expr_code_temp(parser_t *parser, expr_t *expr, int *reg){
  int r2;
  expr = __expr_skip_collate_and_likely(expr);
  if( CONST_FACTOR_OK(parser)
   && ALWAYS(expr!=0)
   && expr->op!=TK_REGISTER
   && __expr_is_constant_not_join(parser, expr)
  ){
    *reg  = 0;
    r2 = __expr_code_run_just_once(parser, expr, -1);
  }else{
    int r1 = __get_temp_reg(parser);
    r2 = __expr_code_target(parser, expr, r1);
    if( r2==r1 ){
      *reg = r1;
    }else{
      __release_temp_reg(parser, r1);
      *reg = 0;
    }
  }
  return r2;
}

/*
** Generate code that will evaluate expression expr and store the
** results in register target.  The results are guaranteed to appear
** in register target.
*/
void __expr_code(parser_t *parser, expr_t *expr, int target){
  int in_reg;

  assert( expr==0 || !EXPR_HAS_VVA_PROPERTY(expr,EP_Immutable) );
  assert( target>0 && target<=parser->n_mem );
  assert( parser->vdbe!=0 || parser->db->malloc_failed );
  if( parser->vdbe==0 ) return;
  in_reg = __expr_code_target(parser, expr, target);
  if( in_reg!=target ){
    u_int8_t op;
    expr_t *x = __expr_skip_collate_and_likely(expr);
    testcase( x!=expr );
    if( ALWAYS(x)
     && (EXPR_HAS_PROPERTY(x,EP_Subquery) || x->op==TK_REGISTER)
    ){
      op = OP_Copy;
    }else{
      op = OP_SCopy;
    }
    __vdbe_add_op2(parser->vdbe, op, in_reg, target);
  }
}

/*
** Make a transient copy of expression expr and then code it using
** __expr_code().  This routine works just like __expr_code()
** except that the input expression is guaranteed to be unchanged.
*/
void __expr_code_copy(parser_t *parser, expr_t *expr, int target){
  DBSQL *db = parser->db;
  expr = __expr_dup(db, expr, 0);
  if( !db->malloc_failed ) __expr_code(parser, expr, target);
  __expr_delete(db, expr);
}

/*
** Generate code that will evaluate expression expr and store the
** results in register target.  The results are guaranteed to appear
** in register target.  If the expression is constant, then this routine
** might choose to code the expression at initialization time.
*/
void __expr_code_factorable(parser_t *parser, expr_t *expr, int target){
  if( parser->ok_const_factor && __expr_is_constant_not_join(parser,expr) ){
    __expr_code_run_just_once(parser, expr, target);
  }else{
    __expr_code_copy(parser, expr, target);
  }
}

/*
** Generate code that pushes the value of every element of the given
** expression list into a sequence of registers beginning at target.
**
** Return the number of elements evaluated.  The number returned will
** usually be list->n_expr but might be reduced if DBSQL_ECEL_OMITREF
** is defined.
**
** The DBSQL_ECEL_DUP flag prevents the arguments from being
** filled using OP_SCopy.  OP_Copy must be used instead.
**
** The DBSQL_ECEL_FACTOR argument allows constant arguments to be
** factored out into initialization code.
**
** The DBSQL_ECEL_REF flag means that expressions in the list with
** expr_list_t.a[].u.x.i_order_by_col>0 have already been evaluated and stored
** in registers at src_reg, and so the value can be copied from there.
** If DBSQL_ECEL_OMITREF is also set, then the values with u.x.i_order_by_col>0
** are simply omitted rather than being copied from src_reg.
*/
int __expr_code_expr_list(
  parser_t *parser,     /* Parsing context */
  expr_list_t *list,   /* The expression list to be coded */
  int target,        /* Where to write results */
  int src_reg,        /* Source registers if DBSQL_ECEL_REF */
  u_int8_t flags           /* DBSQL_ECEL_* flags */
){
  struct expr_list_item_t *item;
  int i, j, n;
  u_int8_t copy_op = (flags & DBSQL_ECEL_DUP) ? OP_Copy : OP_SCopy;
  vdbe_t *v = parser->vdbe;
  assert( list!=0 );
  assert( target>0 );
  assert( parser->vdbe!=0 );  /* Never gets this far otherwise */
  n = list->n_expr;
  if( !CONST_FACTOR_OK(parser) ) flags &= ~DBSQL_ECEL_FACTOR;
  for(item=list->a, i=0; i<n; i++, item++){
    expr_t *expr = item->expr;
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
    if( item->fg.sorter_ref ){
      i--;
      n--;
    }else
#endif
    if( (flags & DBSQL_ECEL_REF)!=0 && (j = item->u.x.i_order_by_col)>0 ){
      if( flags & DBSQL_ECEL_OMITREF ){
        i--;
        n--;
      }else{
        __vdbe_add_op2(v, copy_op, j+src_reg-1, target+i);
      }
    }else if( (flags & DBSQL_ECEL_FACTOR)!=0
           && __expr_is_constant_not_join(parser,expr)
    ){
      __expr_code_run_just_once(parser, expr, target+i);
    }else{
      int in_reg = __expr_code_target(parser, expr, target+i);
      if( in_reg!=target+i ){
        vdbe_op_t *op;
        if( copy_op==OP_Copy
         && (op=__vdbe_get_last_op(v))->opcode==OP_Copy
         && op->p1+op->p3+1==in_reg
         && op->p2+op->p3+1==target+i
         && op->p5==0  /* The do-not-merge flag must be clear */
        ){
          op->p3++;
        }else{
          __vdbe_add_op2(v, copy_op, in_reg, target+i);
        }
      }
    }
  }
  return n;
}

/*
** Generate code for a BETWEEN operator.
**
**    x BETWEEN y AND z
**
** The above is equivalent to
**
**    x>=y AND x<=z
**
** Code it as such, taking care to do the common subexpression
** elimination of x.
**
** The jump_if parameter determines details:
**
**    NULL:                   Store the boolean result in reg[dest]
**    __expr_if_true:      Jump to dest if true
**    __expr_if_false:     Jump to dest if false
**
** The jump_if_null parameter is ignored if jump_if is NULL.
*/
static void __expr_code_between(
  parser_t *parser,    /* Parsing and code generating context */
  expr_t *expr,      /* The BETWEEN expression */
  int dest,         /* Jump destination or storage location */
  void (*jump)(parser_t*,expr_t*,int,int), /* Action to take */
  int jump_if_null    /* Take the jump if the BETWEEN is NULL */
){
  expr_t expr_and;     /* The AND operator in  x>=y AND x<=z  */
  expr_t comp_left;    /* The  x>=y  term */
  expr_t comp_right;   /* The  x<=z  term */
  int reg_free1 = 0; /* Temporary use register */
  expr_t *del = 0;
  DBSQL *db = parser->db;

  memset(&comp_left, 0, sizeof(expr_t));
  memset(&comp_right, 0, sizeof(expr_t));
  memset(&expr_and, 0, sizeof(expr_t));

  assert( EXPR_USE_X_LIST(expr) );
  del = __expr_dup(db, expr->left, 0);
  if( db->malloc_failed==0 ){
    expr_and.op = TK_AND;
    expr_and.left = &comp_left;
    expr_and.right = &comp_right;
    comp_left.op = TK_GE;
    comp_left.left = del;
    comp_left.right = expr->x.list->a[0].expr;
    comp_right.op = TK_LE;
    comp_right.left = del;
    comp_right.right = expr->x.list->a[1].expr;
    __expr_to_register(del, __expr_code_vector(parser, del, &reg_free1));
    if( jump ){
      jump(parser, &expr_and, dest, jump_if_null);
    }else{
      /* Mark the expression is being from the ON or USING clause of a join
      ** so that the __expr_code_target() routine will not attempt to move
      ** it into the parser_t.const_expr list.  We should use a new bit for this,
      ** for clarity, but we are out of bits in the expr_t.flags field so we
      ** have to reuse the EP_OuterON bit.  Bummer. */
      del->flags |= EP_OuterON;
      __expr_code_target(parser, &expr_and, dest);
    }
    __release_temp_reg(parser, reg_free1);
  }
  __expr_delete(db, del);

  /* Ensure adequate test coverage */
  testcase( jump==__expr_if_true  && jump_if_null==0 && reg_free1==0 );
  testcase( jump==__expr_if_true  && jump_if_null==0 && reg_free1!=0 );
  testcase( jump==__expr_if_true  && jump_if_null!=0 && reg_free1==0 );
  testcase( jump==__expr_if_true  && jump_if_null!=0 && reg_free1!=0 );
  testcase( jump==__expr_if_false && jump_if_null==0 && reg_free1==0 );
  testcase( jump==__expr_if_false && jump_if_null==0 && reg_free1!=0 );
  testcase( jump==__expr_if_false && jump_if_null!=0 && reg_free1==0 );
  testcase( jump==__expr_if_false && jump_if_null!=0 && reg_free1!=0 );
  testcase( jump==0 );
}

/*
** Generate code for a boolean expression such that a jump is made
** to the label "dest" if the expression is true but execution
** continues straight thru if the expression is false.
**
** If the expression evaluates to NULL (neither true nor false), then
** take the jump if the jump_if_null flag is DBSQL_JUMPIFNULL.
**
** This code depends on the fact that certain token values (ex: TK_EQ)
** are the same as opcode values (ex: OP_Eq) that implement the corresponding
** operation.  Special comments in vdbe.c and the mkopcodeh.awk script in
** the make process cause these values to align.  Assert()s in the code
** below verify that the numbers are aligned correctly.
*/
void __expr_if_true(parser_t *parser, expr_t *expr, int dest, int jump_if_null){
  vdbe_t *v = parser->vdbe;
  int op = 0;
  int reg_free1 = 0;
  int reg_free2 = 0;
  int r1, r2;

  assert( jump_if_null==DBSQL_JUMPIFNULL || jump_if_null==0 );
  if( NEVER(v==0) )     return;  /* Existence of VDBE checked by caller */
  if( NEVER(expr==0) ) return;  /* No way this can happen */
  assert( !EXPR_HAS_VVA_PROPERTY(expr, EP_Immutable) );
  op = expr->op;
  switch( op ){
    case TK_AND:
    case TK_OR: {
      expr_t *alt = __expr_simplified_and_or(expr);
      if( alt!=expr ){
        __expr_if_true(parser, alt, dest, jump_if_null);
      }else{
        expr_t *first, *second;
        if( __expr_eval_rhs_first(expr) ){
          first = expr->right;
          second = expr->left;
        }else{
          first = expr->left;
          second = expr->right;
        }
        if( op==TK_AND ){
          int d2 = __vdbe_make_label(parser);
          testcase( jump_if_null==0 );
          __expr_if_false(parser, first, d2,
                             jump_if_null^DBSQL_JUMPIFNULL);
          __expr_if_true(parser, second, dest, jump_if_null);
          __vdbe_resolve_label(v, d2);
        }else{
          testcase( jump_if_null==0 );
          __expr_if_true(parser, first, dest, jump_if_null);
          __expr_if_true(parser, second, dest, jump_if_null);
        }
      }
      break;
    }
    case TK_NOT: {
      testcase( jump_if_null==0 );
      __expr_if_false(parser, expr->left, dest, jump_if_null);
      break;
    }
    case TK_TRUTH: {
      int is_not;      /* IS NOT TRUE or IS NOT FALSE */
      int is_true;     /* IS TRUE or IS NOT TRUE */
      testcase( jump_if_null==0 );
      is_not = expr->op2==TK_ISNOT;
      is_true = __expr_truth_value(expr->right);
      testcase( is_true && is_not );
      testcase( !is_true && is_not );
      if( is_true ^ is_not ){
        __expr_if_true(parser, expr->left, dest,
                          is_not ? DBSQL_JUMPIFNULL : 0);
      }else{
        __expr_if_false(parser, expr->left, dest,
                           is_not ? DBSQL_JUMPIFNULL : 0);
      }
      break;
    }
    case TK_IS:
    case TK_ISNOT:
      testcase( op==TK_IS );
      testcase( op==TK_ISNOT );
      op = (op==TK_IS) ? TK_EQ : TK_NE;
      jump_if_null = DBSQL_NULLEQ;
      /* no break */ deliberate_fall_through
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE:
    case TK_NE:
    case TK_EQ: {
      int addr_is_null;
      if( __expr_is_vector(expr->left) ) goto default_expr;
      if( EXPR_HAS_PROPERTY(expr, EP_Subquery) && jump_if_null!=DBSQL_NULLEQ ){
        addr_is_null = __expr_compute_operands(parser, expr,
                                   &r1, &r2, &reg_free1, &reg_free2);
      }else{
        r1 = __expr_code_temp(parser, expr->left, &reg_free1);
        r2 = __expr_code_temp(parser, expr->right, &reg_free2);
        addr_is_null = 0;
      }
      __code_compare(parser, expr->left, expr->right, op,
                  r1, r2, dest, jump_if_null, EXPR_HAS_PROPERTY(expr,EP_Commuted));
      assert(TK_LT==OP_Lt); testcase(op==OP_Lt); VDBE_COVERAGE_IF(v,op==OP_Lt);
      assert(TK_LE==OP_Le); testcase(op==OP_Le); VDBE_COVERAGE_IF(v,op==OP_Le);
      assert(TK_GT==OP_Gt); testcase(op==OP_Gt); VDBE_COVERAGE_IF(v,op==OP_Gt);
      assert(TK_GE==OP_Ge); testcase(op==OP_Ge); VDBE_COVERAGE_IF(v,op==OP_Ge);
      assert(TK_EQ==OP_Eq); testcase(op==OP_Eq);
      VDBE_COVERAGE_IF(v, op==OP_Eq && jump_if_null==DBSQL_NULLEQ);
      VDBE_COVERAGE_IF(v, op==OP_Eq && jump_if_null!=DBSQL_NULLEQ);
      assert(TK_NE==OP_Ne); testcase(op==OP_Ne);
      VDBE_COVERAGE_IF(v, op==OP_Ne && jump_if_null==DBSQL_NULLEQ);
      VDBE_COVERAGE_IF(v, op==OP_Ne && jump_if_null!=DBSQL_NULLEQ);
      testcase( reg_free1==0 );
      testcase( reg_free2==0 );
      if( addr_is_null ){
        if( jump_if_null ){
          __vdbe_change_p2(v, addr_is_null, dest);
        }else{
          __vdbe_jump_here(v, addr_is_null);
        }
      }
      break;
    }
    case TK_ISNULL:
    case TK_NOTNULL: {
      assert( TK_ISNULL==OP_IsNull );   testcase( op==TK_ISNULL );
      assert( TK_NOTNULL==OP_NotNull ); testcase( op==TK_NOTNULL );
      r1 = __expr_code_temp(parser, expr->left, &reg_free1);
      assert( reg_free1==0 || reg_free1==r1 );
      if( reg_free1 ) __vdbe_typeof_column(v, r1);
      __vdbe_add_op2(v, op, r1, dest);
      VDBE_COVERAGE_IF(v, op==TK_ISNULL);
      VDBE_COVERAGE_IF(v, op==TK_NOTNULL);
      break;
    }
    case TK_BETWEEN: {
      testcase( jump_if_null==0 );
      __expr_code_between(parser, expr, dest, __expr_if_true, jump_if_null);
      break;
    }
#ifndef DBSQL_OMIT_SUBQUERY
    case TK_IN: {
      int dest_if_false = __vdbe_make_label(parser);
      int dest_if_null = jump_if_null ? dest : dest_if_false;
      __expr_code_in(parser, expr, dest_if_false, dest_if_null);
      __vdbe_goto(v, dest);
      __vdbe_resolve_label(v, dest_if_false);
      break;
    }
#endif
    default: {
    default_expr:
      if( EXPR_ALWAYS_TRUE(expr) ){
        __vdbe_goto(v, dest);
      }else if( EXPR_ALWAYS_FALSE(expr) ){
        /* No-op */
      }else{
        r1 = __expr_code_temp(parser, expr, &reg_free1);
        __vdbe_add_op3(v, OP_If, r1, dest, jump_if_null!=0);
        VDBE_COVERAGE(v);
        testcase( reg_free1==0 );
        testcase( jump_if_null==0 );
      }
      break;
    }
  }
  __release_temp_reg(parser, reg_free1);
  __release_temp_reg(parser, reg_free2); 
}

/*
** Generate code for a boolean expression such that a jump is made
** to the label "dest" if the expression is false but execution
** continues straight thru if the expression is true.
**
** If the expression evaluates to NULL (neither true nor false) then
** jump if jump_if_null is DBSQL_JUMPIFNULL or fall through if jump_if_null
** is 0.
*/
void __expr_if_false(parser_t *parser, expr_t *expr, int dest, int jump_if_null){
  vdbe_t *v = parser->vdbe;
  int op = 0;
  int reg_free1 = 0;
  int reg_free2 = 0;
  int r1, r2;

  assert( jump_if_null==DBSQL_JUMPIFNULL || jump_if_null==0 );
  if( NEVER(v==0) ) return; /* Existence of VDBE checked by caller */
  if( expr==0 )    return;
  assert( !EXPR_HAS_VVA_PROPERTY(expr,EP_Immutable) );

  /* The value of expr->op and op are related as follows:
  **
  **       expr->op            op
  **       ---------          ----------
  **       TK_ISNULL          OP_NotNull
  **       TK_NOTNULL         OP_IsNull
  **       TK_NE              OP_Eq
  **       TK_EQ              OP_Ne
  **       TK_GT              OP_Le
  **       TK_LE              OP_Gt
  **       TK_GE              OP_Lt
  **       TK_LT              OP_Ge
  **
  ** For other values of expr->op, op is undefined and unused.
  ** The value of TK_ and OP_ constants are arranged such that we
  ** can compute the mapping above using the following expression.
  ** Assert()s verify that the computation is correct.
  */
  op = ((expr->op+(TK_ISNULL&1))^1)-(TK_ISNULL&1);

  /* Verify correct alignment of TK_ and OP_ constants
  */
  assert( expr->op!=TK_ISNULL || op==OP_NotNull );
  assert( expr->op!=TK_NOTNULL || op==OP_IsNull );
  assert( expr->op!=TK_NE || op==OP_Eq );
  assert( expr->op!=TK_EQ || op==OP_Ne );
  assert( expr->op!=TK_LT || op==OP_Ge );
  assert( expr->op!=TK_LE || op==OP_Gt );
  assert( expr->op!=TK_GT || op==OP_Le );
  assert( expr->op!=TK_GE || op==OP_Lt );

  switch( expr->op ){
    case TK_AND:
    case TK_OR: {
      expr_t *alt = __expr_simplified_and_or(expr);
      if( alt!=expr ){
        __expr_if_false(parser, alt, dest, jump_if_null);
      }else{
        expr_t *first, *second;
        if( __expr_eval_rhs_first(expr) ){
          first = expr->right;
          second = expr->left;
        }else{
          first = expr->left;
          second = expr->right;
        }
        if( expr->op==TK_AND ){
          testcase( jump_if_null==0 );
          __expr_if_false(parser, first, dest, jump_if_null);
          __expr_if_false(parser, second, dest, jump_if_null);
        }else{
          int d2 = __vdbe_make_label(parser);
          testcase( jump_if_null==0 );
          __expr_if_true(parser, first, d2,
                            jump_if_null^DBSQL_JUMPIFNULL);
          __expr_if_false(parser, second, dest, jump_if_null);
          __vdbe_resolve_label(v, d2);
        }
      }
      break;
    }
    case TK_NOT: {
      testcase( jump_if_null==0 );
      __expr_if_true(parser, expr->left, dest, jump_if_null);
      break;
    }
    case TK_TRUTH: {
      int is_not;   /* IS NOT TRUE or IS NOT FALSE */
      int is_true;  /* IS TRUE or IS NOT TRUE */
      testcase( jump_if_null==0 );
      is_not = expr->op2==TK_ISNOT;
      is_true = __expr_truth_value(expr->right);
      testcase( is_true && is_not );
      testcase( !is_true && is_not );
      if( is_true ^ is_not ){
        /* IS TRUE and IS NOT FALSE */
        __expr_if_false(parser, expr->left, dest,
                           is_not ? 0 : DBSQL_JUMPIFNULL);

      }else{
        /* IS FALSE and IS NOT TRUE */
        __expr_if_true(parser, expr->left, dest,
                          is_not ? 0 : DBSQL_JUMPIFNULL);
      }
      break;
    }
    case TK_IS:
    case TK_ISNOT:
      testcase( expr->op==TK_IS );
      testcase( expr->op==TK_ISNOT );
      op = (expr->op==TK_IS) ? TK_NE : TK_EQ;
      jump_if_null = DBSQL_NULLEQ;
      /* no break */ deliberate_fall_through
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE:
    case TK_NE:
    case TK_EQ: {
      int addr_is_null;
      if( __expr_is_vector(expr->left) ) goto default_expr;
      if( EXPR_HAS_PROPERTY(expr, EP_Subquery) && jump_if_null!=DBSQL_NULLEQ ){
        addr_is_null = __expr_compute_operands(parser, expr,
                                   &r1, &r2, &reg_free1, &reg_free2);
      }else{
        r1 = __expr_code_temp(parser, expr->left, &reg_free1);
        r2 = __expr_code_temp(parser, expr->right, &reg_free2);
        addr_is_null = 0;
      }
      __code_compare(parser, expr->left, expr->right, op,
                  r1, r2, dest, jump_if_null,EXPR_HAS_PROPERTY(expr,EP_Commuted));
      assert(TK_LT==OP_Lt); testcase(op==OP_Lt); VDBE_COVERAGE_IF(v,op==OP_Lt);
      assert(TK_LE==OP_Le); testcase(op==OP_Le); VDBE_COVERAGE_IF(v,op==OP_Le);
      assert(TK_GT==OP_Gt); testcase(op==OP_Gt); VDBE_COVERAGE_IF(v,op==OP_Gt);
      assert(TK_GE==OP_Ge); testcase(op==OP_Ge); VDBE_COVERAGE_IF(v,op==OP_Ge);
      assert(TK_EQ==OP_Eq); testcase(op==OP_Eq);
      VDBE_COVERAGE_IF(v, op==OP_Eq && jump_if_null!=DBSQL_NULLEQ);
      VDBE_COVERAGE_IF(v, op==OP_Eq && jump_if_null==DBSQL_NULLEQ);
      assert(TK_NE==OP_Ne); testcase(op==OP_Ne);
      VDBE_COVERAGE_IF(v, op==OP_Ne && jump_if_null!=DBSQL_NULLEQ);
      VDBE_COVERAGE_IF(v, op==OP_Ne && jump_if_null==DBSQL_NULLEQ);
      testcase( reg_free1==0 );
      testcase( reg_free2==0 );
      if( addr_is_null ){
        if( jump_if_null ){
          __vdbe_change_p2(v, addr_is_null, dest);
        }else{
          __vdbe_jump_here(v, addr_is_null);
        }
      }
      break;
    }
    case TK_ISNULL:
    case TK_NOTNULL: {
      r1 = __expr_code_temp(parser, expr->left, &reg_free1);
      assert( reg_free1==0 || reg_free1==r1 );
      if( reg_free1 ) __vdbe_typeof_column(v, r1);
      __vdbe_add_op2(v, op, r1, dest);
      testcase( op==TK_ISNULL );   VDBE_COVERAGE_IF(v, op==TK_ISNULL);
      testcase( op==TK_NOTNULL );  VDBE_COVERAGE_IF(v, op==TK_NOTNULL);
      break;
    }
    case TK_BETWEEN: {
      testcase( jump_if_null==0 );
      __expr_code_between(parser, expr, dest, __expr_if_false, jump_if_null);
      break;
    }
#ifndef DBSQL_OMIT_SUBQUERY
    case TK_IN: {
      if( jump_if_null ){
        __expr_code_in(parser, expr, dest, dest);
      }else{
        int dest_if_null = __vdbe_make_label(parser);
        __expr_code_in(parser, expr, dest, dest_if_null);
        __vdbe_resolve_label(v, dest_if_null);
      }
      break;
    }
#endif
    default: {
    default_expr:
      if( EXPR_ALWAYS_FALSE(expr) ){
        __vdbe_goto(v, dest);
      }else if( EXPR_ALWAYS_TRUE(expr) ){
        /* no-op */
      }else{
        r1 = __expr_code_temp(parser, expr, &reg_free1);
        __vdbe_add_op3(v, OP_IfNot, r1, dest, jump_if_null!=0);
        VDBE_COVERAGE(v);
        testcase( reg_free1==0 );
        testcase( jump_if_null==0 );
      }
      break;
    }
  }
  __release_temp_reg(parser, reg_free1);
  __release_temp_reg(parser, reg_free2);
}

/*
** Like __expr_if_false() except that a copy is made of expr before
** code generation, and that copy is deleted after code generation. This
** ensures that the original expr is unchanged.
*/
void __expr_if_false_dup(parser_t *parser, expr_t *expr, int dest,int jump_if_null){
  DBSQL *db = parser->db;
  expr_t *copy = __expr_dup(db, expr, 0);
  if( db->malloc_failed==0 ){
    __expr_if_false(parser, copy, dest, jump_if_null);
  }
  __expr_delete(db, copy);
}

/*
** Expression var is guaranteed to be an SQL variable. expr may be any
** type of expression.
**
** If expr is a simple SQL value - an integer, real, string, blob
** or NULL value - then the VDBE currently being prepared is configured
** to re-prepare each time a new value is bound to variable var.
**
** Additionally, if expr is a simple SQL value and the value is the
** same as that currently bound to variable var, non-zero is returned.
** Otherwise, if the values are not the same or if expr is not a simple
** SQL value, zero is returned.
**
** If the DBSQL_EnableQPSG flag is set on the database connection, then
** this routine always returns false.
*/
static DBSQL_NOINLINE int __expr_compare_variable(
  const parser_t *parser,
  const expr_t *var,
  const expr_t *expr
){
  int res = 2;
  int i_var;
  dbsql_value_t *l, *r = 0;
 
  if( expr->op==TK_VARIABLE && var->i_column==expr->i_column ){
    return 0;
  }
  if( (parser->db->flags & DBSQL_EnableQPSG)!=0 ) return 2;
  __value_from_expr(parser->db, expr, DBSQL_UTF8, DBSQL_AFF_BLOB, &r);
  if( r ){
    i_var = var->i_column;
    __vdbe_set_varmask(parser->vdbe, i_var);
    l = __vdbe_get_bound_value(parser->reprepare, i_var, DBSQL_AFF_BLOB);
    if( l ){
      if( dbsql_value_type(l)==DBSQL_TEXT ){
        dbsql_value_text(l); /* Make sure the encoding is UTF-8 */
      }
      res = __mem_compare(l, r, 0) ? 2 : 0;
    }
    __value_free(r);
    __value_free(l);
  }
  return res;
}

/*
** Do a deep comparison of two expression trees.  Return 0 if the two
** expressions are completely identical.  Return 1 if they differ only
** by a COLLATE operator at the top level.  Return 2 if there are differences
** other than the top-level COLLATE operator.
**
** If any subelement of b has expr_t.i_table==(-1) then it is allowed
** to compare equal to an equivalent element in a with expr_t.i_table==i_tab.
**
** The a side might be using TK_REGISTER.  If that is the case and b is
** not using TK_REGISTER but is otherwise equivalent, then still return 0.
**
** Sometimes this routine will return 2 even if the two expressions
** really are equivalent.  If we cannot prove that the expressions are
** identical, we return 2 just to be safe.  So if this routine
** returns 2, then you do not really know for certain if the two
** expressions are the same.  But if you get a 0 or 1 return, then you
** can be sure the expressions are the same.  In the places where
** this routine is used, it does not hurt to get an extra 2 - that
** just might result in some slightly slower code.  But returning
** an incorrect 0 or 1 could lead to a malfunction.
**
** If parser is not NULL and DBSQL_EnableQPSG is off then TK_VARIABLE
** terms in a with bindings in parser->reprepare can be matched against
** literals in b.  The parser->vdbe->expmask bitmask is updated for
** each variable referenced.
*/
int __expr_compare(
  const parser_t *parser,
  const expr_t *a,
  const expr_t *b,
  int i_tab
){
  u_int32_t combined_flags;
  if( a==0 || b==0 ){
    return b==a ? 0 : 2;
  }
  if( parser && a->op==TK_VARIABLE ){
    return __expr_compare_variable(parser, a, b);
  }
  combined_flags = a->flags | b->flags;
  if( combined_flags & EP_IntValue ){
    if( (a->flags&b->flags&EP_IntValue)!=0 && a->u.i_value==b->u.i_value ){
      return 0;
    }
    return 2;
  }
  if( a->op!=b->op || a->op==TK_RAISE ){
    if( a->op==TK_COLLATE && __expr_compare(parser, a->left,b,i_tab)<2 ){
      return 1;
    }
    if( b->op==TK_COLLATE && __expr_compare(parser, a,b->left,i_tab)<2 ){
      return 1;
    }
    if( a->op==TK_AGG_COLUMN && b->op==TK_COLUMN
     && b->i_table<0 && a->i_table==i_tab
    ){
      /* fall through */
    }else{
      return 2;
    }
  }
  assert( !EXPR_HAS_PROPERTY(a, EP_IntValue) );
  assert( !EXPR_HAS_PROPERTY(b, EP_IntValue) );
  if( a->u.token ){
    if( a->op==TK_FUNCTION || a->op==TK_AGG_FUNCTION ){
      if( __str_icmp(a->u.token,b->u.token)!=0 ) return 2;
#ifndef DBSQL_OMIT_WINDOWFUNC
      assert( a->op==b->op );
      if( EXPR_HAS_PROPERTY(a,EP_WinFunc)!=EXPR_HAS_PROPERTY(b,EP_WinFunc) ){
        return 2;
      }
      if( EXPR_HAS_PROPERTY(a,EP_WinFunc) ){
        if( __window_compare(parser, a->y.win, b->y.win, 1)!=0 ){
          return 2;
        }
      }
#endif
    }else if( a->op==TK_NULL ){
      return 0;
    }else if( a->op==TK_COLLATE ){
      if( dbsql_stricmp(a->u.token,b->u.token)!=0 ) return 2;
    }else
    if( b->u.token!=0
     && a->op!=TK_COLUMN
     && a->op!=TK_AGG_COLUMN
     && strcmp(a->u.token,b->u.token)!=0
    ){
      return 2;
    }
  }
  if( (a->flags & (EP_Distinct|EP_Commuted))
     != (b->flags & (EP_Distinct|EP_Commuted)) ) return 2;
  if( ALWAYS((combined_flags & EP_TokenOnly)==0) ){
    if( combined_flags & EP_xIsSelect ) return 2;
    if( (combined_flags & EP_FixedCol)==0
     && __expr_compare(parser, a->left, b->left, i_tab) ) return 2;
    if( __expr_compare(parser, a->right, b->right, i_tab) ) return 2;
    if( __expr_list_compare(a->x.list, b->x.list, i_tab) ) return 2;
    if( a->op!=TK_STRING
     && a->op!=TK_TRUEFALSE
     && ALWAYS((combined_flags & EP_Reduced)==0)
    ){
      if( a->i_column!=b->i_column ) return 2;
      if( a->op2!=b->op2 && a->op==TK_TRUTH ) return 2;
      if( a->op!=TK_IN && a->i_table!=b->i_table && a->i_table!=i_tab ){
        return 2;
      }
    }
  }
  return 0;
}

/*
** Compare two expr_list_t objects.  Return 0 if they are identical, 1
** if they are certainly different, or 2 if it is not possible to
** determine if they are identical or not.
**
** If any subelement of b has expr_t.i_table==(-1) then it is allowed
** to compare equal to an equivalent element in a with expr_t.i_table==i_tab.
**
** This routine might return non-zero for equivalent __expr_lists.  The
** only consequence will be disabled optimizations.  But this routine
** must never return 0 if the two expr_list_t objects are different, or
** a malfunction will result.
**
** Two NULL pointers are considered to be the same.  But a NULL pointer
** always differs from a non-NULL pointer.
*/
int __expr_list_compare(const expr_list_t *a, const expr_list_t *b, int i_tab){
  int i;
  if( a==0 && b==0 ) return 0;
  if( a==0 || b==0 ) return 1;
  if( a->n_expr!=b->n_expr ) return 1;
  for(i=0; i<a->n_expr; i++){
    int res;
    expr_t *expr_a = a->a[i].expr;
    expr_t *expr_b = b->a[i].expr;
    if( a->a[i].fg.sort_flags!=b->a[i].fg.sort_flags ) return 1;
    if( (res = __expr_compare(0, expr_a, expr_b, i_tab)) ) return res;
  }
  return 0;
}

/*
** Like __expr_compare() except COLLATE operators at the top-level
** are ignored.
*/
int __expr_compare_skip(expr_t *a,expr_t *b, int i_tab){
  return __expr_compare(0,
             __expr_skip_collate(a),
             __expr_skip_collate(b),
             i_tab);
}

/*
** Return non-zero if expr_t p can only be true if nn is not NULL.
**
** Or if seen_not is true, return non-zero if expr_t p can only be
** non-NULL if nn is not NULL
*/
static int __expr_implies_not_null(
  const parser_t *parser,/* Parsing context */
  const expr_t *p,      /* The expression to be checked */
  const expr_t *nn,    /* The expression that is NOT NULL */
  int i_tab,           /* table_t being evaluated */
  int seen_not         /* Return true only if p can be any non-NULL value */
){
  assert( p );
  assert( nn );
  if( __expr_compare(parser, p, nn, i_tab)==0 ){
    return nn->op!=TK_NULL;
  }
  switch( p->op ){
    case TK_IN: {
      if( seen_not && EXPR_HAS_PROPERTY(p, EP_xIsSelect) ) return 0;
      assert( EXPR_USE_X_SELECT(p) || (p->x.list!=0 && p->x.list->n_expr>0) );
      return __expr_implies_not_null(parser, p->left, nn, i_tab, 1);
    }
    case TK_BETWEEN: {
      expr_list_t *list;
      assert( EXPR_USE_X_LIST(p) );
      list = p->x.list;
      assert( list!=0 );
      assert( list->n_expr==2 );
      if( seen_not ) return 0;
      if( __expr_implies_not_null(parser, list->a[0].expr, nn, i_tab, 1)
       || __expr_implies_not_null(parser, list->a[1].expr, nn, i_tab, 1)
      ){
        return 1;
      }
      return __expr_implies_not_null(parser, p->left, nn, i_tab, 1);
    }
    case TK_EQ:
    case TK_NE:
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE:
    case TK_PLUS:
    case TK_MINUS:
    case TK_BITOR:
    case TK_LSHIFT:
    case TK_RSHIFT:
    case TK_CONCAT:
      seen_not = 1;
      /* no break */ deliberate_fall_through
    case TK_STAR:
    case TK_REM:
    case TK_BITAND:
    case TK_SLASH: {
      if( __expr_implies_not_null(parser, p->right, nn, i_tab, seen_not) ) return 1;
      /* no break */ deliberate_fall_through
    }
    case TK_SPAN:
    case TK_COLLATE:
    case TK_UPLUS:
    case TK_UMINUS: {
      return __expr_implies_not_null(parser, p->left, nn, i_tab, seen_not);
    }
    case TK_TRUTH: {
      if( seen_not ) return 0;
      if( p->op2!=TK_IS ) return 0;
      return __expr_implies_not_null(parser, p->left, nn, i_tab, 1);
    }
    case TK_BITNOT:
    case TK_NOT: {
      return __expr_implies_not_null(parser, p->left, nn, i_tab, 1);
    }
  }
  return 0;
}

/*
** Return true if the boolean value of the expression is always either
** FALSE or NULL.
*/
static int __expr_is_not_true(expr_t *expr){
  int v;
  if( expr->op==TK_NULL ) return 1;
  if( expr->op==TK_TRUEFALSE && __expr_truth_value(expr)==0 ) return 1;
  v = 1;
  if( __expr_is_integer(expr, &v, 0) && v==0 ) return 1;
  return 0;
}

/*
** Return true if the expression is one of the following:
**
**    CASE WHEN x THEN y END
**    CASE WHEN x THEN y ELSE NULL END
**    CASE WHEN x THEN y ELSE false END
**    iif(x,y)
**    iif(x,y,NULL)
**    iif(x,y,false)
*/
static int __expr_is_iif(DBSQL *db, const expr_t *expr){
  expr_list_t *list;
  if( expr->op==TK_FUNCTION ){
    const char *z = expr->u.token;
    func_def_t *def;
    if( (z[0]!='i' && z[0]!='I') ) return 0;
    if( expr->x.list==0 ) return 0;
    def = __find_function(db, z, expr->x.list->n_expr, ENC(db), 0);
#ifdef DBSQL_ENABLE_UNKNOWN_SQL_FUNCTION
    if( def==0 ) return 0;
#else
    if( NEVER(def==0) ) return 0;
#endif
    if( (def->func_flags & DBSQL_FUNC_INLINE)==0 ) return 0;
    if( DBSQL_PTR_TO_INT(def->user_data)!=INLINEFUNC_iif ) return 0;
  }else if( expr->op==TK_CASE ){
    if( expr->left!=0 ) return 0;
  }else{
    return 0;
  }
  list = expr->x.list;
  assert( list!=0 );
  if( list->n_expr==2 ) return 1;
  if( list->n_expr==3 && __expr_is_not_true(list->a[2].expr) ) return 1;
  return 0;
}

/*
** Return true if we can prove the e2 will always be true if e1 is
** true.  Return false if we cannot complete the proof or if e2 might
** be false.  Examples:
**
**     e1: x==5        e2: x==5             Result: true
**     e1: x>0         e2: x==5             Result: false
**     e1: x=21        e2: x=21 OR y=43     Result: true
**     e1: x!=123      e2: x IS NOT NULL    Result: true
**     e1: x!=?1       e2: x IS NOT NULL    Result: true
**     e1: x IS NULL   e2: x IS NOT NULL    Result: false
**     e1: x IS ?2     e2: x IS NOT NULL    Result: false
**     e1: iif(x,y)    e2: x                Result: true
**     PE1: iif(x,y,0)  e2: x                Result: true
**
** When comparing TK_COLUMN nodes between e1 and e2, if e2 has
** expr_t.i_table<0 then assume a table number given by i_tab.
**
** If parser is not NULL, then the values of bound variables in e1 are
** compared against literal values in e2 and parser->vdbe->expmask is
** modified to record which bound variables are referenced.  If parser
** is NULL, then false will be returned if e1 contains any bound variables.
**
** When in doubt, return false.  returning_t true might give a performance
** improvement.  returning_t false might cause a performance reduction, but
** it will always give the correct answer and is hence always safe.
*/
int __expr_implies_expr(
  const parser_t *parser,
  const expr_t *e1,
  const expr_t *e2,
  int i_tab
){
  if( __expr_compare(parser, e1, e2, i_tab)==0 ){
    return 1;
  }
  if( e2->op==TK_OR
   && (__expr_implies_expr(parser, e1, e2->left, i_tab)
             || __expr_implies_expr(parser, e1, e2->right, i_tab) )
  ){
    return 1;
  }
  if( e2->op==TK_NOTNULL
   && __expr_implies_not_null(parser, e1, e2->left, i_tab, 0)
  ){
    return 1;
  }
  if( __expr_is_iif(parser->db, e1) ){
    return __expr_implies_expr(parser,e1->x.list->a[0].expr,e2,i_tab);
  }
  return 0;
}

/* This is a helper function to __implies_not_null_row().  In this routine,
** set walker->code to one only if *both* of the input expressions
** separately have the implies-not-null-row property.
*/
static void __both_imply_not_null_row(walker_t *walker, expr_t *e1, expr_t *e2){
  if( walker->code==0 ){
    __walk_expr(walker, e1);
    if( walker->code ){
      walker->code = 0;
      __walk_expr(walker, e2);
    }
  }
}

/*
** This is the expr_t node callback for __expr_implies_non_null_row().
** If the expression node requires that the table at walker->i_cur
** have one or more non-NULL column, then set walker->code to 1 and abort.
**
** walker->m_w_flags is non-zero if this inquiry is being undertaking on
** behalf of a RIGHT JOIN (or FULL JOIN).  That makes a difference when
** evaluating terms in the ON clause of an inner join.
**
** This routine controls an optimization.  False positives (setting
** walker->code to 1 when it should not be) are deadly, but false-negatives
** (never setting walker->code) is a harmless missed optimization.
*/
static int __implies_not_null_row(walker_t *walker, expr_t *expr){
  testcase( expr->op==TK_AGG_COLUMN );
  testcase( expr->op==TK_AGG_FUNCTION );
  if( EXPR_HAS_PROPERTY(expr, EP_OuterON) ) return WRC_Prune;
  if( EXPR_HAS_PROPERTY(expr, EP_InnerON) && walker->m_w_flags ){
    /* If i_cur is used in an inner-join ON clause to the left of a
    ** RIGHT JOIN, that does *not* mean that the table must be non-null.
    ** But it is difficult to check for that condition precisely.
    ** To keep things simple, any use of i_cur from any inner-join is
    ** ignored while attempting to simplify a RIGHT JOIN. */
    return WRC_Prune;
  }
  switch( expr->op ){
    case TK_ISNOT:
    case TK_ISNULL:
    case TK_NOTNULL:
    case TK_IS:
    case TK_VECTOR:
    case TK_FUNCTION:
    case TK_TRUTH:
    case TK_CASE:
      testcase( expr->op==TK_ISNOT );
      testcase( expr->op==TK_ISNULL );
      testcase( expr->op==TK_NOTNULL );
      testcase( expr->op==TK_IS );
      testcase( expr->op==TK_VECTOR );
      testcase( expr->op==TK_FUNCTION );
      testcase( expr->op==TK_TRUTH );
      testcase( expr->op==TK_CASE );
      return WRC_Prune;

    case TK_COLUMN:
      if( walker->u.i_cur==expr->i_table ){
        walker->code = 1;
        return WRC_Abort;
      }
      return WRC_Prune;

    case TK_OR:
    case TK_AND:
      /* Both sides of an AND or OR must separately imply non-null-row.
      ** Consider these cases:
      **    1.  NOT (x AND y)
      **    2.  x OR y
      ** If only one of x or y is non-null-row, then the overall expression
      ** can be true if the other arm is false (case 1) or true (case 2).
      */
      testcase( expr->op==TK_OR );
      testcase( expr->op==TK_AND );
      __both_imply_not_null_row(walker, expr->left, expr->right);
      return WRC_Prune;
       
    case TK_IN:
      /* Beware of "x NOT IN ()" and "x NOT IN (SELECT 1 WHERE false)",
      ** both of which can be true.  But apart from these cases, if
      ** the left-hand side of the IN is NULL then the IN itself will be
      ** NULL. */
      if( EXPR_USE_X_LIST(expr) && ALWAYS(expr->x.list->n_expr>0) ){
        __walk_expr(walker, expr->left);
      }
      return WRC_Prune;

    case TK_BETWEEN:
      /* In "x NOT BETWEEN y AND z" either x must be non-null-row or else
      ** both y and z must be non-null row */
      assert( EXPR_USE_X_LIST(expr) );
      assert( expr->x.list->n_expr==2 );
      __walk_expr(walker, expr->left);
      __both_imply_not_null_row(walker, expr->x.list->a[0].expr,
                                   expr->x.list->a[1].expr);
      return WRC_Prune;

    /* Virtual tables are allowed to use constraints like x=NULL.  So
    ** a term of the form x=y does not prove that y is not null if x
    ** is the column of a virtual table */
    case TK_EQ:
    case TK_NE:
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE: {
      expr_t *left = expr->left;
      expr_t *right = expr->right;
      testcase( expr->op==TK_EQ );
      testcase( expr->op==TK_NE );
      testcase( expr->op==TK_LT );
      testcase( expr->op==TK_LE );
      testcase( expr->op==TK_GT );
      testcase( expr->op==TK_GE );
      /* The y.tab=0 assignment in wherecode.c always happens after the
      ** __implies_not_null_row() test */
      assert( left->op!=TK_COLUMN || EXPR_USE_Y_TAB(left) );
      assert( right->op!=TK_COLUMN || EXPR_USE_Y_TAB(right) );
      if( (left->op==TK_COLUMN
           && ALWAYS(left->y.tab!=0)
           && IS_VIRTUAL(left->y.tab))
       || (right->op==TK_COLUMN
           && ALWAYS(right->y.tab!=0)
           && IS_VIRTUAL(right->y.tab))
      ){
        return WRC_Prune;
      }
      /* no break */ deliberate_fall_through
    }
    default:
      return WRC_Continue;
  }
}

/*
** Return true (non-zero) if expression p can only be true if at least
** one column of table i_tab is non-null.  In other words, return true
** if expression p will always be NULL or false if every column of i_tab
** is NULL.
**
** False negatives are acceptable.  In other words, it is ok to return
** zero even if expression p will never be true of every column of i_tab
** is NULL.  A false negative is merely a missed optimization opportunity.
**
** False positives are not allowed, however.  A false positive may result
** in an incorrect answer.
**
** Terms of p that are marked with EP_OuterON (and hence that come from
** the ON or USING clauses of OUTER JOINS) are excluded from the analysis.
**
** This routine is used to check if a LEFT JOIN can be converted into
** an ordinary JOIN.  The p argument is the WHERE clause.  If the WHERE
** clause requires that some column of the right table of the LEFT JOIN
** be non-NULL, then the LEFT JOIN can be safely converted into an
** ordinary join.
*/
int __expr_implies_non_null_row(expr_t *p, int i_tab, int is_rj){
  walker_t w;
  p = __expr_skip_collate_and_likely(p);
  if( p==0 ) return 0;
  if( p->op==TK_NOTNULL ){
    p = p->left;
  }else{
    while( p->op==TK_AND ){
      if( __expr_implies_non_null_row(p->left, i_tab, is_rj) ) return 1;
      p = p->right;
    }
  }
  w.expr_callback = __implies_not_null_row;
  w.select_callback = 0;
  w.select_callback2 = 0;
  w.code = 0;
  w.m_w_flags = is_rj!=0;
  w.u.i_cur = i_tab;
  __walk_expr(&w, p);
  return w.code;
}

/*
** An instance of the following structure is used by the tree walker
** to determine if an expression can be evaluated by reference to the
** index only, without having to do a search for the corresponding
** table entry.  The idx_cover_t.idx field is the index.  idx_cover_t.i_cur
** is the cursor for the table.
*/
struct idx_cover_t {
  index_t *idx;     /* The index to be tested for coverage */
  int i_cur;        /* Cursor number for the table corresponding to the index */
};

/*
** Check to see if there are references to columns in table
** walker->u.idx_cover->i_cur can be satisfied using the index
** walker->u.idx_cover->idx.
*/
static int __expr_idx_cover(walker_t *walker, expr_t *expr){
  if( expr->op==TK_COLUMN
   && expr->i_table==walker->u.idx_cover->i_cur
   && __table_column_to_index(walker->u.idx_cover->idx, expr->i_column)<0
  ){
    walker->code = 1;
    return WRC_Abort;
  }
  return WRC_Continue;
}

/*
** Determine if an index idx on table with cursor i_cur contains will
** the expression expr.  Return true if the index does cover the
** expression and false if the expr expression references table columns
** that are not found in the index idx.
**
** An index covering an expression means that the expression can be
** evaluated using only the index and without having to lookup the
** corresponding table entry.
*/
int __expr_covered_by_index(
  expr_t *expr,        /* The index to be tested */
  int i_cur,           /* The cursor number for the corresponding table */
  index_t *idx         /* The index that might be used for coverage */
){
  walker_t w;
  struct idx_cover_t xcov;
  memset(&w, 0, sizeof(w));
  xcov.i_cur = i_cur;
  xcov.idx = idx;
  w.expr_callback = __expr_idx_cover;
  w.u.idx_cover = &xcov;
  __walk_expr(&w, expr);
  return !w.code;
}


/* Structure used to pass information throughout the walker_t in order to
** implement __references_src_list().
*/
struct ref_src_list_t {
  DBSQL *db;         /* Database connection used for __db_realloc() */
  src_list_t *ref;       /* Looking for references to these tables */
  int64_t n_exclude;        /* Number of tables to exclude from the search */
  int *excludes;      /* Cursor IDs for tables to exclude from the search */
};

/*
** walker_t SELECT callbacks for __references_src_list().
**
** When entering a new subquery on the expr argument, add all FROM clause
** entries for that subquery to the exclude list.
**
** When leaving the subquery, remove those entries from the exclude list.
*/
static int __select_ref_enter(walker_t *walker, select_t *p_select){
  struct ref_src_list_t *p = walker->u.ref_src_list;
  src_list_t *src = p_select->src;
  int64_t i, j;
  int *pi_new;
  if( src->n_src==0 ) return WRC_Continue;
  j = p->n_exclude;
  p->n_exclude += src->n_src;
  pi_new = __db_realloc(p->db, p->excludes, p->n_exclude*sizeof(int));
  if( pi_new==0 ){
    p->n_exclude = 0;
    return WRC_Abort;
  }else{
    p->excludes = pi_new;
  }
  for(i=0; i<src->n_src; i++, j++){
     p->excludes[j] = src->a[i].i_cursor;
  }
  return WRC_Continue;
}
static void __select_ref_leave(walker_t *walker, select_t *p_select){
  struct ref_src_list_t *p = walker->u.ref_src_list;
  src_list_t *src = p_select->src;
  if( p->n_exclude ){
    assert( p->n_exclude>=src->n_src );
    p->n_exclude -= src->n_src;
  }
}

/* This is the walker_t EXPR callback for __references_src_list().
**
** Set the 0x01 bit of walker->code if there is a reference to any
** of the tables shown in ref_src_list_t.ref.
**
** Set the 0x02 bit of walker->code if there is a reference to a
** table is in neither ref_src_list_t.ref nor ref_src_list_t.excludes.
*/
static int __expr_ref_to_src_list(walker_t *walker, expr_t *expr){
  if( expr->op==TK_COLUMN
   || expr->op==TK_AGG_COLUMN
  ){
    int i;
    struct ref_src_list_t *p = walker->u.ref_src_list;
    src_list_t *src = p->ref;
    int n_src = src ? src->n_src : 0;
    for(i=0; i<n_src; i++){
      if( expr->i_table==src->a[i].i_cursor ){
        walker->code |= 1;
        return WRC_Continue;
      }
    }
    for(i=0; i<p->n_exclude && p->excludes[i]!=expr->i_table; i++){}
    if( i>=p->n_exclude ){
      walker->code |= 2;
    }
  }
  return WRC_Continue;
}

/*
** Check to see if expr references any tables in src_list.
** Possible return values:
**
**    1         expr does references a table in src_list.
**
**    0         expr references some table that is not defined in either
**              src_list or in subqueries of expr itself.
**
**   -1         expr only references no tables at all, or it only
**              references tables defined in subqueries of expr itself.
**
** As currently used, expr is always an aggregate function call.  That
** fact is exploited for efficiency.
*/
int __references_src_list(parser_t *parser, expr_t *expr, src_list_t *src_list){
  walker_t w;
  struct ref_src_list_t x;
  assert( parser->db!=0 );
  memset(&w, 0, sizeof(w));
  memset(&x, 0, sizeof(x));
  w.expr_callback = __expr_ref_to_src_list;
  w.select_callback = __select_ref_enter;
  w.select_callback2 = __select_ref_leave;
  w.u.ref_src_list = &x;
  x.db = parser->db;
  x.ref = src_list;
  assert( expr->op==TK_AGG_FUNCTION );
  assert( EXPR_USE_X_LIST(expr) );
  __walk_expr_list(&w, expr->x.list);
  if( expr->left ){
    assert( expr->left->op==TK_ORDER );
    assert( EXPR_USE_X_LIST(expr->left) );
    assert( expr->left->x.list!=0 );
    __walk_expr_list(&w, expr->left->x.list);
  }
#ifndef DBSQL_OMIT_WINDOWFUNC
  if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
    __walk_expr(&w, expr->y.win->filter);
  }
#endif
  if( x.excludes ) __db_nn_free_nn(parser->db, x.excludes);
  if( w.code & 0x01 ){
    return 1;
  }else if( w.code ){
    return 0;
  }else{
    return -1;
  }
}

/*
** This is a walker_t expression node callback.
**
** For expr_t nodes that contain agg_info pointers, make sure the agg_info_t
** object that is referenced does not refer directly to the expr_t.  If
** it does, make a copy.  This is done because the expr argument is
** subject to change.
**
** The copy is scheduled for deletion using the __expr_deferred_delete()
** which builds on the __parser_add_cleanup() mechanism.
*/
static int __agginfo_persist_expr_cb(walker_t *walker, expr_t *expr){
  if( ALWAYS(!EXPR_HAS_PROPERTY(expr, EP_TokenOnly|EP_Reduced))
   && expr->agg_info!=0
  ){
    agg_info_t *agg_info = expr->agg_info;
    int i_agg = expr->i_agg;
    parser_t *parser = walker->parser;
    DBSQL *db = parser->db;
    assert( i_agg>=0 );
    if( expr->op!=TK_AGG_FUNCTION ){
      if( i_agg<agg_info->n_column
       && agg_info->cols[i_agg].c_expr==expr
      ){
        expr = __expr_dup(db, expr, 0);
        if( expr && !__expr_deferred_delete(parser, expr) ){
          agg_info->cols[i_agg].c_expr = expr;
        }
      }
    }else{
      assert( expr->op==TK_AGG_FUNCTION );
      if( ALWAYS(i_agg<agg_info->n_func)
       && agg_info->funcs[i_agg].f_expr==expr
      ){
        expr = __expr_dup(db, expr, 0);
        if( expr && !__expr_deferred_delete(parser, expr) ){
          agg_info->funcs[i_agg].f_expr = expr;
        }
      }
    }
  }
  return WRC_Continue;
}

/*
** Initialize a walker_t object so that will persist agg_info_t entries referenced
** by the tree that is walked.
*/
void __agg_info_persist_walker_init(walker_t *walker, parser_t *parser){
  memset(walker, 0, sizeof(*walker));
  walker->parser = parser;
  walker->expr_callback = __agginfo_persist_expr_cb;
  walker->select_callback = __select_walk_noop;
}

/*
** Add a new element to the agg_info->cols[] array.  Return the index of
** the new element.  Return a negative number if malloc fails.
*/
static int __add_agg_info_column(DBSQL *db, agg_info_t *info){
  int i;
  info->cols = __array_allocate(
       db,
       info->cols,
       sizeof(info->cols[0]),
       &info->n_column,
       &i
  );
  return i;
}   

/*
** Add a new element to the agg_info->funcs[] array.  Return the index of
** the new element.  Return a negative number if malloc fails.
*/
static int __add_agg_info_func(DBSQL *db, agg_info_t *info){
  int i;
  info->funcs = __array_allocate(
       db,
       info->funcs,
       sizeof(info->funcs[0]),
       &info->n_func,
       &i
  );
  return i;
}

/*
** Search the agg_info_t object for an cols[] entry that has i_table and i_column.
** Return the index in cols[] of the entry that describes that column.
**
** If no prior entry is found, create a new one and return -1.  The
** new column will have an index of agg_info->n_column-1.
*/
static void __find_or_create_agg_info_column(
  parser_t *parser,       /* Parsing context */
  agg_info_t *agg_info,   /* The agg_info_t object to search and/or modify */
  expr_t *expr          /* expr_t describing the column to find or insert */
){
  struct agg_info_col_t *col;
  int k;
  int mx_term = parser->db->limits[DBSQL_LIMIT_COLUMN];

  assert( mx_term <= SMXV(int16_t) );
  assert( agg_info->i_first_reg==0 );
  col = agg_info->cols;
  for(k=0; k<agg_info->n_column; k++, col++){
    if( col->c_expr==expr ) return;
    if( col->i_table==expr->i_table
     && col->i_column==expr->i_column
     && expr->op!=TK_IF_NULL_ROW
    ){
      goto fix_up_expr;
    }
  }
  k = __add_agg_info_column(parser->db, agg_info);
  if( k<0 ){
    /* OOM on resize */
    assert( parser->db->malloc_failed );
    return;
  }
  if( k>mx_term ){
    __error_msg(parser, "more than %d aggregate terms", mx_term);
    k = mx_term;
  }
  col = &agg_info->cols[k];
  assert( EXPR_USE_Y_TAB(expr) );
  col->tab = expr->y.tab;
  col->i_table = expr->i_table;
  col->i_column = expr->i_column;
  col->i_sorter_column = -1;
  col->c_expr = expr;
  if( agg_info->group_by && expr->op!=TK_IF_NULL_ROW ){
    int j, n;
    expr_list_t *gb = agg_info->group_by;
    struct expr_list_item_t *term = gb->a;
    n = gb->n_expr;
    for(j=0; j<n; j++, term++){
      expr_t *e = term->expr;
      if( e->op==TK_COLUMN
       && e->i_table==expr->i_table
       && e->i_column==expr->i_column
      ){
        col->i_sorter_column = j;
        break;
      }
    }
  }
  if( col->i_sorter_column<0 ){
    col->i_sorter_column = agg_info->n_sorting_column++;
  }
fix_up_expr:
  EXPR_SET_VVA_PROPERTY(expr, EP_NoReduce);
  assert( expr->agg_info==0 || expr->agg_info==agg_info );
  expr->agg_info = agg_info;
  if( expr->op==TK_COLUMN ){
    expr->op = TK_AGG_COLUMN;
  }
  assert( k <= SMXV(expr->i_agg) );
  expr->i_agg = (int16_t)k;
}

/*
** This is the expr_callback for a tree walker.  It is used to
** implement __expr_analyze_aggregates().  See __expr_analyze_aggregates
** for additional information.
*/
static int __analyze_aggregate(walker_t *walker, expr_t *expr){
  int i;
  name_context_t *nc = walker->u.nc;
  parser_t *parser = nc->parser;
  src_list_t *src_list = nc->src_list;
  agg_info_t *agg_info = nc->u_nc.agg_info;

  assert( nc->nc_flags & NC_UAggInfo );
  assert( agg_info->i_first_reg==0 );
  switch( expr->op ){
    default: {
      indexed_expr_t *i_epr;
      expr_t tmp;
      assert( parser->i_self_tab==0 );
      if( (nc->nc_flags & NC_InAggFunc)==0 ) break;
      if( parser->idx_epr==0 ) break;
      for(i_epr=parser->idx_epr; i_epr; i_epr=i_epr->ie_next){
        int i_data_cur = i_epr->i_data_cur;
        if( i_data_cur<0 ) continue;
        if( __expr_compare(0, expr, i_epr->expr, i_data_cur)==0 ) break;
      }
      if( i_epr==0 ) break;
      if( NEVER(!EXPR_USE_Y_TAB(expr)) ) break;
      for(i=0; i<src_list->n_src; i++){
         if( src_list->a[i].i_cursor==i_epr->i_data_cur ){
           testcase( i>0 );
           break;
         }
      }
      if( i>=src_list->n_src ) break;
      if( NEVER(expr->agg_info!=0) ) break; /* Resolved by outer context */
      if( parser->n_err ){ return WRC_Abort; }

      /* If we reach this point, it means that expression expr can be
      ** translated into a reference to an index column as described by
      ** i_epr.
      */
      memset(&tmp, 0, sizeof(tmp));
      tmp.op = TK_AGG_COLUMN;
      tmp.i_table = i_epr->i_idx_cur;
      tmp.i_column = i_epr->i_idx_col;
      __find_or_create_agg_info_column(parser, agg_info, &tmp);
      if( parser->n_err ){ return WRC_Abort; }
      assert( agg_info->cols!=0 );
      assert( tmp.i_agg<agg_info->n_column );
      agg_info->cols[tmp.i_agg].c_expr = expr;
      expr->agg_info = agg_info;
      expr->i_agg = tmp.i_agg;
      return WRC_Prune;
    }
    case TK_IF_NULL_ROW:
    case TK_AGG_COLUMN:
    case TK_COLUMN: {
      testcase( expr->op==TK_AGG_COLUMN );
      testcase( expr->op==TK_COLUMN );
      testcase( expr->op==TK_IF_NULL_ROW );
      /* Check to see if the column is in one of the tables in the FROM
      ** clause of the aggregate query */
      if( ALWAYS(src_list!=0) ){
        src_item_t *item = src_list->a;
        for(i=0; i<src_list->n_src; i++, item++){
          assert( !EXPR_HAS_PROPERTY(expr, EP_TokenOnly|EP_Reduced) );
          if( expr->i_table==item->i_cursor ){
            __find_or_create_agg_info_column(parser, agg_info, expr);
            break;
          } /* endif expr->i_table==item->i_cursor */
        } /* end loop over src_list */
      }
      return WRC_Continue;
    }
    case TK_AGG_FUNCTION: {
      if( (nc->nc_flags & NC_InAggFunc)==0
       && walker->walker_depth==expr->op2
       && expr->agg_info==0
      ){
        /* Check to see if expr is a duplicate of another aggregate
        ** function that is already in the agg_info structure
        */
        struct agg_info_func_t *item = agg_info->funcs;
        int mx_term = parser->db->limits[DBSQL_LIMIT_COLUMN];
        assert( mx_term <= SMXV(int16_t) );
        for(i=0; i<agg_info->n_func; i++, item++){
          if( NEVER(item->f_expr==expr) ) break;
          if( __expr_compare(0, item->f_expr, expr, -1)==0 ){
            break;
          }
        }
        if( i>mx_term ){
          __error_msg(parser, "more than %d aggregate terms", mx_term);
          i = mx_term;
          assert( i<agg_info->n_func );
        }else if( i>=agg_info->n_func ){
          /* expr is original.  Make a new entry in agg_info->funcs[]
          */
          u_int8_t enc = ENC(parser->db);
          i = __add_agg_info_func(parser->db, agg_info);
          if( i>=0 ){
            int n_arg;
            assert( !EXPR_HAS_PROPERTY(expr, EP_xIsSelect) );
            item = &agg_info->funcs[i];
            item->f_expr = expr;
            assert( EXPR_USE_U_TOKEN(expr) );
            n_arg = expr->x.list ? expr->x.list->n_expr : 0;
            item->func = __find_function(parser->db,
                                         expr->u.token, n_arg, enc, 0);
            assert( item->ob_unique==0 );
            if( expr->left
             && (item->func->func_flags & DBSQL_FUNC_NEEDCOLL)==0
            ){
              /* The NEEDCOLL test above causes any ORDER BY clause on
              ** aggregate min() or max() to be ignored. */
              expr_list_t *ob_list;
              assert( n_arg>0 );
              assert( expr->left->op==TK_ORDER );
              assert( EXPR_USE_X_LIST(expr->left) );
              item->i_ob_tab = parser->n_tab++;
              ob_list = expr->left->x.list;
              assert( ob_list->n_expr>0 );
              assert( item->ob_unique==0 );
              if( ob_list->n_expr==1
               && n_arg==1
               && __expr_compare(0,ob_list->a[0].expr,
                               expr->x.list->a[0].expr,0)==0
              ){
                item->ob_payload = 0;
                item->ob_unique = EXPR_HAS_PROPERTY(expr, EP_Distinct);
              }else{
                item->ob_payload = 1;
              }
              item->use_subtype =
                    (item->func->func_flags & DBSQL_SUBTYPE)!=0;
            }else{
              item->i_ob_tab = -1;
            }
            if( EXPR_HAS_PROPERTY(expr, EP_Distinct) && !item->ob_unique ){
              item->i_distinct = parser->n_tab++;
            }else{
              item->i_distinct = -1;
            }
          }
        }
        /* Make expr point to the appropriate agg_info->funcs[] entry
        */
        assert( !EXPR_HAS_PROPERTY(expr, EP_TokenOnly|EP_Reduced) );
        EXPR_SET_VVA_PROPERTY(expr, EP_NoReduce);
        assert( i <= SMXV(expr->i_agg) );
        expr->i_agg = (int16_t)i;
        expr->agg_info = agg_info;
        return WRC_Prune;
      }else{
        return WRC_Continue;
      }
    }
  }
  return WRC_Continue;
}

/*
** Analyze the expr expression looking for aggregate functions and
** for variables that need to be added to agg_info_t object that nc->agg_info
** points to.  Additional entries are made on the agg_info_t object as
** necessary.
**
** This routine should only be called after the expression has been
** analyzed by __resolve_expr_names().
*/
void __expr_analyze_aggregates(name_context_t *nc, expr_t *expr){
  walker_t w;
  w.expr_callback = __analyze_aggregate;
  w.select_callback = __walker_depth_increase;
  w.select_callback2 = __walker_depth_decrease;
  w.walker_depth = 0;
  w.u.nc = nc;
  w.parser = 0;
  assert( nc->src_list!=0 );
  __walk_expr(&w, expr);
}

/*
** Call __expr_analyze_aggregates() for every expression in an
** expression list.  Return the number of errors.
**
** If an error is found, the analysis is cut short.
*/
void __expr_analyze_agg_list(name_context_t *nc, expr_list_t *list){
  struct expr_list_item_t *item;
  int i;
  if( list ){
    for(item=list->a, i=0; i<list->n_expr; i++, item++){
      __expr_analyze_aggregates(nc, item->expr);
    }
  }
}

/*
** Allocate a single new register for use to hold some intermediate result.
*/
int __get_temp_reg(parser_t *parser){
  if( parser->n_temp_reg==0 ){
    return ++parser->n_mem;
  }
  return parser->temp_regs[--parser->n_temp_reg];
}

/*
** Deallocate a register, making available for reuse for some other
** purpose.
*/
void __release_temp_reg(parser_t *parser, int i_reg){
  if( i_reg ){
    __vdbe_release_registers(parser, i_reg, 1, 0, 0);
    if( parser->n_temp_reg<ARRAY_SIZE(parser->temp_regs) ){
      parser->temp_regs[parser->n_temp_reg++] = i_reg;
    }
  }
}

/*
** Allocate or deallocate a block of n_reg consecutive registers.
*/
int __get_temp_range(parser_t *parser, int n_reg){
  int i, n;
  if( n_reg==1 ) return __get_temp_reg(parser);
  i = parser->i_range_reg;
  n = parser->n_range_reg;
  if( n_reg<=n ){
    parser->i_range_reg += n_reg;
    parser->n_range_reg -= n_reg;
  }else{
    i = parser->n_mem+1;
    parser->n_mem += n_reg;
  }
  return i;
}
void __release_temp_range(parser_t *parser, int i_reg, int n_reg){
  if( n_reg==1 ){
    __release_temp_reg(parser, i_reg);
    return;
  }
  __vdbe_release_registers(parser, i_reg, n_reg, 0, 0);
  if( n_reg>parser->n_range_reg ){
    parser->n_range_reg = n_reg;
    parser->i_range_reg = i_reg;
  }
}

/*
** Mark all temporary registers as being unavailable for reuse.
**
** Always invoke this procedure after coding a subroutine or co-routine
** that might be invoked from other parts of the code, to ensure that
** the sub/co-routine does not use registers in common with the code that
** invokes the sub/co-routine.
*/
void __clear_temp_reg_cache(parser_t *parser){
  parser->n_temp_reg = 0;
  parser->n_range_reg = 0;
}

/*
** Make sure sufficient registers have been allocated so that
** i_reg is a valid register number.
*/
void __touch_register(parser_t *parser, int i_reg){
  if( parser->n_mem<i_reg ) parser->n_mem = i_reg;
}

#if defined(DBSQL_ENABLE_STAT4) || defined(DBSQL_DEBUG)
/*
** Return the latest reusable register in the set of all registers.
** The value returned is no less than i_min.  If any register i_min or
** greater is in permanent use, then return one more than that last
** permanent register.
*/
int __first_available_register(parser_t *parser, int i_min){
  const expr_list_t *list = parser->const_expr;
  if( list ){
    int i;
    for(i=0; i<list->n_expr; i++){
      if( list->a[i].u.i_const_expr_reg>=i_min ){
        i_min = list->a[i].u.i_const_expr_reg + 1;
      }
    }
  }
  parser->n_temp_reg = 0;
  parser->n_range_reg = 0;
  return i_min;
}
#endif /* DBSQL_ENABLE_STAT4 || DBSQL_DEBUG */

/*
** Validate that no temporary register falls within the range of
** i_first..i_last, inclusive.  This routine is only call from within assert()
** statements.
*/
#ifdef DBSQL_DEBUG
int __no_temps_in_range(parser_t *parser, int i_first, int i_last){
  int i;
  if( parser->n_range_reg>0
   && parser->i_range_reg+parser->n_range_reg > i_first
   && parser->i_range_reg <= i_last
  ){
     return 0;
  }
  for(i=0; i<parser->n_temp_reg; i++){
    if( parser->temp_regs[i]>=i_first && parser->temp_regs[i]<=i_last ){
      return 0;
    }
  }
  if( parser->const_expr ){
    expr_list_t *list = parser->const_expr;
    for(i=0; i<list->n_expr; i++){
      int i_reg = list->a[i].u.i_const_expr_reg;
      if( i_reg==0 ) continue;
      if( i_reg>=i_first && i_reg<=i_last ) return 0;
    }
  }
  return 1;
}
#endif /* DBSQL_DEBUG */
