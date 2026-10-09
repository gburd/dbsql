/*
** 2013-11-12
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains structure and macro definitions for the query
** planner logic in "where.c".  These definitions are broken out into
** a separate source file for easier editing.
*/
#ifndef DBSQL_WHEREINT_H
#define DBSQL_WHEREINT_H


/* Forward references
*/
typedef struct where_clause_t where_clause_t;
typedef struct where_mask_set_t where_mask_set_t;
typedef struct where_or_info_t where_or_info_t;
typedef struct where_and_info_t where_and_info_t;
typedef struct where_level_t where_level_t;
typedef struct where_loop_t where_loop_t;
typedef struct where_path_t where_path_t;
typedef struct where_term_t where_term_t;
typedef struct where_loop_builder_t where_loop_builder_t;
typedef struct where_scan_t where_scan_t;
typedef struct where_or_cost_t where_or_cost_t;
typedef struct where_or_set_t where_or_set_t;
typedef struct where_mem_block_t where_mem_block_t;
typedef struct where_right_join_t where_right_join_t;

/*
** This object is a header on a block of allocated memory that will be
** automatically freed when its WInfo object is destructed.
*/
struct where_mem_block_t {
  where_mem_block_t *next;      /* Next block in the chain */
  u_int64_t sz;                    /* Bytes of space */
};

/*
** Extra information attached to a where_level_t that is a RIGHT JOIN.
*/
struct where_right_join_t {
  int i_match;          /* Cursor used to determine prior matched rows */
  int reg_bloom;        /* Bloom filter for i_rj_match */
  int reg_return;       /* Return register for the interior subroutine */
  int addr_subrtn;      /* Starting address for the interior subroutine */
  int end_subrtn;       /* The last opcode in the interior subroutine */
};

/*
** This object contains information needed to implement a single nested
** loop in WHERE clause.
**
** Contrast this object with where_loop_t.  This object describes the
** implementation of the loop.  where_loop_t describes the algorithm.
** This object contains a pointer to the where_loop_t algorithm as one of
** its elements.
**
** The where_info_t object contains a single instance of this object for
** each term in the FROM clause (which is to say, for each of the
** nested loops as implemented).  The order of where_level_t objects determines
** the loop nested order, with where_info_t.a[0] being the outer loop and
** where_info_t.a[where_info_t.n_level-1] being the inner loop.
*/
struct where_level_t {
  int i_left_join;        /* Memory cell used to implement LEFT OUTER JOIN */
  int i_tab_cur;          /* The VDBE cursor used to access the table */
  int i_idx_cur;          /* The VDBE cursor used to access idx */
  int addr_brk;          /* Jump here to break out of the loop */
  int addr_halt;         /* Abort the query due to empty table or similar */
  int addr_nxt;          /* Jump here to start the next IN combination */
  int addr_skip;         /* Jump here for next iteration of skip-scan */
  int addr_cont;         /* Jump here to continue with the next loop cycle */
  int addr_first;        /* First instruction of interior of the loop */
  int addr_body;         /* Beginning of the body of this loop */
  int reg_bignull;       /* big-null flag reg. True if a NULL-scan is needed */
  int addr_bignull;      /* Jump here for next part of big-null scan */
#ifndef DBSQL_LIKE_DOESNT_MATCH_BLOBS
  u_int32_t i_like_rep_cntr;     /* LIKE range processing counter register (times 2) */
  int addr_like_rep;      /* LIKE range processing address */
#endif
  int reg_filter;        /* Bloom filter */
  where_right_join_t *rj;  /* Extra information for RIGHT JOIN */
  u_int8_t i_from;             /* Which entry in the FROM clause */
  u_int8_t op, p3, p5;        /* Opcode, P3 & P5 of the opcode that ends the loop */
  int p1, p2;           /* Operands of the opcode used to end the loop */
  union {               /* Information that depends on w_loop->ws_flags */
    struct {
      int n_in;              /* Number of entries in in_loops[] */
      struct in_loop_t {
        int i_cur;              /* The VDBE cursor used by this IN operator */
        int addr_in_top;         /* Top of the IN loop */
        int i_base;             /* Base register of multi-key index record */
        int n_prefix;           /* Number of prior entries in the key */
        u_int8_t end_loop_op;         /* IN Loop terminator. OP_Next or OP_Prev */
      } *in_loops;           /* Information about each nested IN operator */
    } in;                 /* Used when w_loop->ws_flags&WHERE_IN_ABLE */
    index_t *covering_idx;  /* Possible covering index for WHERE_MULTI_OR */
  } u;
  struct where_loop_t *w_loop;  /* The selected where_loop_t object */
  Bitmask not_ready;          /* FROM entries not usable at this level */
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  int addr_visit;        /* Address at which row is visited */
#endif
};

/*
** Each instance of this object represents an algorithm for evaluating one
** term of a join.  Every term of the FROM clause will have at least
** one corresponding where_loop_t object (unless INDEXED BY constraints
** prevent a query solution - which is an error) and many terms of the
** FROM clause will have multiple where_loop_t objects, each describing a
** potential way of implementing that FROM-clause term, together with
** dependencies and cost estimates for using the chosen algorithm.
**
** Query planning consists of building up a collection of these where_loop_t
** objects, then computing a particular sequence of where_loop_t objects, with
** one where_loop_t object per FROM clause term, that satisfy all dependencies
** and that minimize the overall cost.
*/
struct where_loop_t {
  Bitmask prereq;       /* Bitmask of other loops that must run first */
  Bitmask mask_self;     /* Bitmask identifying table i_tab */
#ifdef DBSQL_DEBUG
  char c_id;             /* Symbolic ID of this loop for debugging use */
#endif
  u_int8_t i_tab;              /* Position in FROM clause of table for this loop */
  u_int8_t i_sort_idx;          /* Sorting index number.  0==None */
  log_est_t r_setup;        /* One-time setup cost (ex: create transient index) */
  log_est_t r_run;          /* Cost of running each loop */
  log_est_t n_out;          /* Estimated number of output rows */
  union {
    struct {               /* Information for internal btree tables */
      u_int16_t n_eq;               /* Number of equality constraints */
      u_int16_t n_btm;              /* Size of BTM vector */
      u_int16_t n_top;              /* Size of TOP vector */
      u_int16_t n_distinct_col;      /* index_t columns used to sort for DISTINCT */
      index_t *p_index;         /* index_t used, or NULL */
      expr_list_t *order_by;    /* ORDER BY clause if this is really a subquery */
    } btree;
    struct {               /* Information for virtual tables */
      int idx_num;            /* index_t number */
      u_int32_t need_free : 1;      /* True if dbsql_free(idx_str) is needed */
      u_int32_t omit_offset : 1;   /* True to let virtual table handle offset */
      u_int32_t idx_num_hex : 1;    /* Show idx_num as hex in EXPLAIN QUERY PLAN */
      int8_t is_ordered;          /* True if satisfies ORDER BY */
      u_int16_t omit_mask;          /* Terms that may be omitted */
      char *idx_str;          /* index_t identifier string */
      u_int32_t m_handle_in;         /* Terms to handle as IN(...) instead of == */
    } vtab;
  } u;
  u_int32_t ws_flags;          /* WHERE_* flags describing the plan */
  u_int16_t n_l_term;           /* Number of entries in l_terms[] */
  u_int16_t n_skip;            /* Number of NULL l_terms[] entries */
  /**** __where_loop_xfer() copies fields above ***********************/
# define WHERE_LOOP_XFER_SZ offsetof(where_loop_t,n_l_slot)
  u_int16_t n_l_slot;           /* Number of slots allocated for l_terms[] */
#ifdef WHERETRACE_ENABLED
  log_est_t r_star_delta;    /* Cost delta due to star-schema heuristic.  Not
                        ** initialized unless w_info->star_used */
#endif
  where_term_t **l_terms;   /* __where_terms used */
  where_loop_t *next_loop; /* Next where_loop_t object in the where_clause_t */
  where_term_t *l_term_spaces[3];  /* Initial l_terms[] space */
};

/* This object holds the prerequisites and the cost of running a
** subquery on one operand of an OR operator in the WHERE clause.
** See where_or_set_t for additional information
*/
struct where_or_cost_t {
  Bitmask prereq;     /* Prerequisites */
  log_est_t r_run;        /* Cost of running this subquery */
  log_est_t n_out;        /* Number of outputs for this subquery */
};

/* The where_or_set_t object holds a set of possible __where_or_costs that
** correspond to the subquery(s) of OR-clause processing.  Only the
** best N_OR_COST elements are retained.
*/
#define N_OR_COST 3
struct where_or_set_t {
  u_int16_t n;                      /* Number of valid a[] entries */
  where_or_cost_t a[N_OR_COST];   /* Set of best costs */
};

/*
** Each instance of this object holds a sequence of where_loop_t objects
** that implement some or all of a query plan.
**
** Think of each where_loop_t object as a node in a graph with arcs
** showing dependencies and costs for travelling between nodes.  (That is
** not a completely accurate description because where_loop_t costs are a
** vector, not a scalar, and because dependencies are many-to-one, not
** one-to-one as are graph nodes.  But it is a useful visualization aid.)
** Then a where_path_t object is a path through the graph that visits some
** or all of the where_loop_t objects once.
**
** The "solver" works by creating the N best where_path_t objects of length
** 1.  Then using those as a basis to compute the N best where_path_t objects
** of length 2.  And so forth until the length of __where_paths equals the
** number of nodes in the FROM clause.  The best (lowest cost) where_path_t
** at the end is the chosen query plan.
*/
struct where_path_t {
  Bitmask mask_loop;     /* Bitmask of all where_loop_t objects in this path */
  Bitmask rev_loop;      /* loops[]s that should be reversed for ORDER BY */
  log_est_t n_row;          /* Estimated number of rows generated by this path */
  log_est_t r_cost;         /* Total cost of this path */
  log_est_t r_unsort;       /* Total cost of this path ignoring sorting costs */
  int8_t is_ordered;         /* No. of ORDER BY terms satisfied. -1 for unknown */
  where_loop_t **loops;    /* Array of where_loop_t objects implementing this path */
};

/*
** The query generator uses an array of instances of this structure to
** help it analyze the subexpressions of the WHERE clause.  Each WHERE
** clause subexpression is separated from the others by AND operators,
** usually, or sometimes subexpressions separated by OR.
**
** All __where_terms are collected into a single where_clause_t structure. 
** The following identity holds:
**
**        where_term_t.wc->a[where_term_t.idx] == where_term_t
**
** When a term is of the form:
**
**              X <op> <expr>
**
** where X is a column name and <op> is one of certain operators,
** then where_term_t.left_cursor and where_term_t.u.left_column record the
** cursor number and column number for X.  where_term_t.e_operator records
** the <op> using a bitmask encoding defined by WO_xxx below.  The
** use of a bitmask encoding for the operator allows us to search
** quickly for terms that match any of several different operators.
**
** A where_term_t might also be two or more subterms connected by OR:
**
**         (t1.X <op> <expr>) OR (t1.Y <op> <expr>) OR ....
**
** In this second case, wt_flag has the TERM_ORINFO bit set and e_operator==WO_OR
** and the where_term_t.u.or_info field points to auxiliary information that
** is collected about the OR clause.
**
** If a term in the WHERE clause does not match either of the two previous
** categories, then e_operator==0.  The where_term_t.expr field is still set
** to the original subexpression content and wt_flags is set up appropriately
** but no other fields in the where_term_t object are meaningful.
**
** When e_operator!=0, prereq_right and prereq_all record sets of cursor numbers,
** but they do so indirectly.  A single where_mask_set_t structure translates
** cursor number into bits and the translated bit is stored in the prereq
** fields.  The translation is used in order to maximize the number of
** bits that will fit in a Bitmask.  The VDBE cursor numbers might be
** spread out over the non-negative integers.  For example, the cursor
** numbers might be 3, 8, 9, 10, 20, 23, 41, and 45.  The where_mask_set_t
** translates these sparse cursor numbers into consecutive integers
** beginning with 0 in order to make the best possible use of the available
** bits in the Bitmask.  So, in the example above, the cursor numbers
** would be mapped into integers 0 through 7.
**
** The number of terms in a join is limited by the number of bits
** in prereq_right and prereq_all.  The default is 64 bits, hence dbsql
** is only able to process joins with 64 or fewer tables.
*/
struct where_term_t {
  expr_t *expr;            /* Pointer to the subexpression that is this term */
  where_clause_t *wc;       /* The clause this term is part of */
  log_est_t truth_prob;       /* Probability of truth for this expression */
  u_int16_t wt_flags;            /* TERM_xxx bit flags.  See below */
  u_int16_t e_operator;          /* A WO_xx value describing <op> */
  u_int8_t n_child;              /* Number of children that must disable us */
  u_int8_t match_op;            /* op_t for vtab MATCH/LIKE/GLOB/REGEXP terms */
  int i_parent;            /* Disable wc->a[i_parent] when this term disabled */
  int left_cursor;         /* Cursor number of X in "X <op> <expr>" */
#ifdef DBSQL_DEBUG
  int i_term;              /* Which where_term_t is this, for debug purposes */
#endif
  union {
    struct {
      int left_column;         /* column_t number of X in "X <op> <expr>" */
      int i_field;             /* Field in (?,?,?) IN (SELECT...) vector */
    } x;                    /* Opcode other than OP_OR or OP_AND */
    where_or_info_t *or_info;   /* Extra information if (e_operator & WO_OR)!=0 */
    where_and_info_t *and_info; /* Extra information if (e_operator& WO_AND)!=0 */
  } u;
  Bitmask prereq_right;    /* Bitmask of tables used by expr->right */
  Bitmask prereq_all;      /* Bitmask of tables referenced by expr */
};

/*
** Allowed values of where_term_t.wt_flags
*/
#define TERM_DYNAMIC    0x0001 /* Need to call __expr_delete(db, expr) */
#define TERM_VIRTUAL    0x0002 /* Added by the optimizer.  Do not code */
#define TERM_CODED      0x0004 /* This term is already coded */
#define TERM_COPIED     0x0008 /* Has a child */
#define TERM_ORINFO     0x0010 /* Need to free the where_term_t.u.or_info object */
#define TERM_ANDINFO    0x0020 /* Need to free the where_term_t.u.and_info obj */
#define TERM_OK         0x0040 /* Used during OR-clause processing */
#define TERM_VNULL      0x0080 /* Manufactured x>NULL or x<=NULL term */
#define TERM_LIKEOPT    0x0100 /* Virtual terms from the LIKE optimization */
#define TERM_LIKECOND   0x0200 /* Conditionally this LIKE operator term */
#define TERM_LIKE       0x0400 /* The original LIKE operator */
#define TERM_IS         0x0800 /* Term.expr is an IS operator */
#define TERM_VARSELECT  0x1000 /* Term.expr contains a correlated sub-query */
#define TERM_HEURTRUTH  0x2000 /* Heuristic truth_prob used */
#ifdef DBSQL_ENABLE_STAT4
#  define TERM_HIGHTRUTH  0x4000 /* Term excludes few rows */
#else
#  define TERM_HIGHTRUTH  0      /* Only used with STAT4 */
#endif
#define TERM_SLICE      0x8000 /* One slice of a row-value/vector comparison */

/*
** An instance of the where_scan_t object is used as an iterator for locating
** terms in the WHERE clause that are useful to the query planner.
*/
struct where_scan_t {
  where_clause_t *orig_wc;      /* Original, innermost where_clause_t */
  where_clause_t *wc;          /* where_clause_t currently being scanned */
  const char *coll_name;     /* Required collating sequence, if not NULL */
  expr_t *idx_expr;            /* Search for this index expression */
  int k;                     /* Resume scanning at this->wc->a[this->k] */
  u_int32_t op_mask;                /* Acceptable operators */
  char idxaff;               /* Must match this affinity, if coll_name!=NULL */
  unsigned char i_equiv;      /* Current slot in curs[] and columns[] */
  unsigned char n_equiv;      /* Number of entries in curs[] and columns[] */
  int curs[11];             /* Cursors in the equivalence class */
  int16_t columns[11];          /* Corresponding column number in the eq-class */
};

/*
** An instance of the following structure holds all information about a
** WHERE clause.  Mostly this is a container for one or more __where_terms.
**
** Explanation of outer:  For a WHERE clause of the form
**
**           a AND ((b AND c) OR (d AND e)) AND f
**
** There are separate where_clause_t objects for the whole clause and for
** the subclauses "(b AND c)" and "(d AND e)".  The outer field of the
** subclauses points to the where_clause_t object for the whole clause.
*/
struct where_clause_t {
  where_info_t *w_info;       /* WHERE clause processing context */
  where_clause_t *outer;     /* Outer conjunction */
  u_int8_t op;                   /* Split operator.  TK_AND or TK_OR */
  u_int8_t has_or;                /* True if any a[].e_operator is WO_OR */
  int n_term;               /* Number of terms */
  int n_slot;               /* Number of entries in a[] */
  int n_base;               /* Number of terms through the last non-Virtual */
  where_term_t *a;            /* Each a[] describes a term of the WHERE clause */
#if defined(DBSQL_SMALL_STACK)
  where_term_t statics[1];    /* Initial static space for a[] */
#else
  where_term_t statics[8];    /* Initial static space for a[] */
#endif
};

/*
** A where_term_t with e_operator==WO_OR has its u.or_info pointer set to
** a dynamically allocated instance of the following structure.
*/
struct where_or_info_t {
  where_clause_t wc;          /* Decomposition into subterms */
  Bitmask indexable;       /* Bitmask of all indexable tables in the clause */
};

/*
** A where_term_t with e_operator==WO_AND has its u.and_info pointer set to
** a dynamically allocated instance of the following structure.
*/
struct where_and_info_t {
  where_clause_t wc;          /* The subexpression broken out */
};

/*
** An instance of the following structure keeps track of a mapping
** between VDBE cursor numbers and bits of the bitmasks in where_term_t.
**
** The VDBE cursor numbers are small integers contained in
** src_item_t.i_cursor and expr_t.i_table fields.  For any given WHERE
** clause, the cursor numbers might not begin with 0 and they might
** contain gaps in the numbering sequence.  But we want to make maximum
** use of the bits in our bitmasks.  This structure provides a mapping
** from the sparse cursor numbers into consecutive integers beginning
** with 0.
**
** If where_mask_set_t.ix[A]==B it means that The A-th bit of a Bitmask
** corresponds VDBE cursor number B.  The A-th bit of a bitmask is 1<<A.
**
** For example, if the WHERE clause expression used these VDBE
** cursors:  4, 5, 8, 29, 57, 73.  Then the  where_mask_set_t structure
** would map those cursor numbers into bits 0 through 5.
**
** Note that the mapping is not necessarily ordered.  In the example
** above, the mapping might go like this:  4->3, 5->1, 8->2, 29->0,
** 57->5, 73->4.  Or one of 719 other combinations might be used. It
** does not really matter.  What is important is that sparse cursor
** numbers all get mapped into bit numbers that begin with 0 and contain
** no gaps.
*/
struct where_mask_set_t {
  int var_select;               /* Used by __where_expr_usage() */
  int n;                        /* Number of assigned cursor values */
  int ix[BMS];                  /* Cursor assigned to each bit */
};

/*
** This object is a convenience wrapper holding all information needed
** to construct where_loop_t objects for a particular query.
*/
struct where_loop_builder_t {
  where_info_t *w_info;        /* Information about this WHERE */
  where_clause_t *wc;         /* WHERE clause terms */
  where_loop_t *p_new;          /* Template where_loop_t */
  where_or_set_t *or_set;       /* Record best loops here, if not NULL */
#ifdef DBSQL_ENABLE_STAT4
  unpacked_record_t *rec;     /* Probe for stat4 (if required) */
  int n_rec_valid;            /* Number of valid fields currently in rec */
#endif
  unsigned char bld_flags1;  /* First set of DBSQL_BLDF_* flags */
  unsigned char bld_flags2;  /* Second set of DBSQL_BLDF_* flags */
  unsigned int i_plan_limit;  /* Search limiter */
};

/* Allowed values for __where_loop_buider.bld_flags */
#define DBSQL_BLDF1_INDEXED  0x0001   /* An index is used */
#define DBSQL_BLDF1_UNIQUE   0x0002   /* All keys of a UNIQUE index used */

#define DBSQL_BLDF2_2NDPASS  0x0004   /* Second builder pass needed */

/* The where_loop_builder_t.i_plan_limit is used to limit the number of
** index+constraint combinations the query planner will consider for a
** particular query.  If this parameter is unlimited, then certain
** pathological queries can spend excess time in the __where_begin()
** routine.  The limit is high enough that is should not impact real-world
** queries.
**
** DBSQL_QUERY_PLANNER_LIMIT is the baseline limit.  The limit is
** increased by DBSQL_QUERY_PLANNER_LIMIT_INCR before each term of the FROM
** clause is processed, so that every table in a join is guaranteed to be
** able to propose a some index+constraint combinations even if the initial
** baseline limit was exhausted by prior tables of the join.
*/
#ifndef DBSQL_QUERY_PLANNER_LIMIT
# define DBSQL_QUERY_PLANNER_LIMIT 20000
#endif
#ifndef DBSQL_QUERY_PLANNER_LIMIT_INCR
# define DBSQL_QUERY_PLANNER_LIMIT_INCR 1000
#endif

/*
** The WHERE clause processing routine has two halves.  The
** first part does the start of the WHERE loop and the second
** half does the tail of the WHERE loop.  An instance of
** this structure is returned by the first half and passed
** into the second half to give some continuity.
**
** An instance of this object holds the complete state of the query
** planner.
*/
struct where_info_t {
  parser_t *parser;            /* Parsing and code generating context */
  src_list_t *tab_list;        /* List of tables in the join */
  expr_list_t *order_by;       /* The ORDER BY clause or NULL */
  expr_list_t *result_set;     /* Result set of the query */
#if WHERETRACE_ENABLED
  expr_t *where;             /* The complete WHERE clause */
#endif
  select_t *p_select;          /* The entire SELECT statement containing WHERE */
  int cur_one_passes[2];      /* OP_OpenWrite cursors for the ONEPASS opt */
  int i_continue;            /* Jump here to continue with next record */
  int i_break;               /* Jump here to break out of the loop */
  int saved_n_query_loop;      /* parser->n_query_loop outside the WHERE loop */
  u_int16_t wctrl_flags;           /* Flags originally passed to __where_begin() */
  log_est_t i_limit;            /* LIMIT if wctrl_flags has WHERE_USE_LIMIT */
  u_int8_t n_level;                /* Number of nested loop */
  int8_t n_ob_sat;                /* Number of ORDER BY terms satisfied by indices */
  u_int8_t one_pass;              /* ONEPASS_OFF, or _SINGLE, or _MULTI */
  u_int8_t distinct;             /* One of the WHERE_DISTINCT_* values */
  unsigned deferred_seek :1;   /* Uses OP_DeferredSeek */
  unsigned untested_terms :1;   /* Not all WHERE terms resolved by outer loop */
  unsigned ordered_inner_loop:1;/* True if only the inner-most loop is ordered */
  unsigned sorted        :1;   /* True if really sorted (not just grouped) */
  unsigned star_done     :1;   /* True if check for star-query is complete */
  unsigned star_used     :1;   /* True if star-query heuristic is used */
  log_est_t n_row_out;           /* Estimated number of output rows */
#ifdef WHERETRACE_ENABLED
  log_est_t r_total_cost;        /* Total cost of the solution */
#endif
  int i_top;                 /* The very beginning of the WHERE loop */
  int i_end_where;            /* End of the WHERE clause itself */
  where_loop_t *loops;        /* List of all where_loop_t objects */
  where_mem_block_t *mem_to_free;/* Memory to free when this object destroyed */
  Bitmask rev_mask;          /* Mask of ORDER BY terms that need reversing */
  where_clause_t s_wc;          /* Decomposition of the WHERE clause */
  where_mask_set_t s_mask_set;    /* Map cursor numbers to bitmasks */
  where_level_t a[FLEXARRAY];  /* Information about each nest loop in WHERE */
};

/*
** The size (in bytes) of a where_info_t object that holds N __where_levels.
*/
#define SZ_WHEREINFO(N) ROUND8(offsetof(where_info_t,a)+(N)*sizeof(where_level_t))

/*
** Private interfaces - callable only by other where.c routines.
**
** where.c:
*/
Bitmask __where_get_mask(where_mask_set_t*,int);
#ifdef WHERETRACE_ENABLED
void __where_clause_print(where_clause_t *wc);
void __where_term_print(where_term_t *term, int i_term);
void __where_loop_print(const where_loop_t *p, const where_clause_t *wc);
#endif
where_term_t *__where_find_term(
  where_clause_t *wc,     /* The WHERE clause to be searched */
  int i_cur,             /* Cursor number of LHS */
  int i_column,          /* column_t number of LHS */
  Bitmask not_ready,     /* RHS must not overlap with this mask */
  u_int32_t op,               /* Mask of WO_xx values describing operator */
  index_t *idx           /* Must be compatible with this index, if not NULL */
);
void *__where_malloc(where_info_t *w_info, u_int64_t n_byte);
void *__where_realloc(where_info_t *w_info, void *old, u_int64_t n_byte);

/* wherecode.c: */
#ifndef DBSQL_OMIT_EXPLAIN
int __where_explain_one_scan(
  parser_t *parser,                  /* parser_t context */
  src_list_t *tab_list,              /* table_t list this loop refers to */
  where_level_t *level,             /* Scan to write OP_Explain opcode for */
  u_int16_t wctrl_flags                  /* Flags passed to __where_begin() */
);
int __where_explain_bloom_filter(
  const parser_t *parser,            /* parser_t context */
  const where_info_t *w_info,        /* WHERE clause */
  const where_level_t *level        /* Bloom filter on this level */
);
void __where_add_explain_text(
  parser_t *parser,                  /* parser_t context */
  int addr, 
  src_list_t *tab_list,              /* table_t list this loop refers to */
  where_level_t *level,             /* Scan to write OP_Explain opcode for */
  u_int16_t wctrl_flags                  /* Flags passed to __where_begin() */
);
#else
# define __where_explain_one_scan(u,v,w,x) 0
# define __where_explain_bloom_filter(u,v,w) 0
# define  __where_add_explain_text(u,v,w,x,y)
#endif /* DBSQL_OMIT_EXPLAIN */
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
void __where_add_scan_status(
  vdbe_t *v,                        /* vdbe_t to add scanstatus entry to */
  src_list_t *srclist,              /* FROM clause lvl reads data from */
  where_level_t *lvl,               /* Level to add scanstatus() entry for */
  int addr_explain                 /* Address of OP_Explain (or 0) */
);
#else
# define __where_add_scan_status(a, b, c, d) ((void)d)
#endif
Bitmask __where_code_one_loop_start(
  parser_t *parser,       /* Parsing context */
  vdbe_t *v,             /* Prepared statement under construction */
  where_info_t *w_info,   /* Complete information about the WHERE clause */
  int i_level,          /* Which level of w_info->a[] should be coded */
  where_level_t *level,  /* The current level pointer */
  Bitmask not_ready     /* Which tables are currently available */
);
DBSQL_NOINLINE void __where_right_join_loop(
  where_info_t *w_info,
  int i_level,
  where_level_t *level
);

/* whereexpr.c: */
void __where_clause_init(where_clause_t*,where_info_t*);
void __where_clause_clear(where_clause_t*);
void __where_split(where_clause_t*,expr_t*,u_int8_t);
void __where_add_limit(where_clause_t*, select_t*);
Bitmask __where_expr_usage(where_mask_set_t*, expr_t*);
Bitmask __where_expr_usage_nn(where_mask_set_t*, expr_t*);
Bitmask __where_expr_list_usage(where_mask_set_t*, expr_list_t*);
void __where_expr_analyze(src_list_t*, where_clause_t*);
void __where_tab_func_args(parser_t*, src_item_t*, where_clause_t*);





/*
** Bitmasks for the operators on where_term_t objects.  These are all
** operators that are of interest to the query planner.  An
** OR-ed combination of these values can be used when searching for
** particular __where_terms within a where_clause_t.
**
** Value constraints:
**     WO_EQ    == DBSQL_INDEX_CONSTRAINT_EQ
**     WO_LT    == DBSQL_INDEX_CONSTRAINT_LT
**     WO_LE    == DBSQL_INDEX_CONSTRAINT_LE
**     WO_GT    == DBSQL_INDEX_CONSTRAINT_GT
**     WO_GE    == DBSQL_INDEX_CONSTRAINT_GE
*/
#define WO_IN     0x0001
#define WO_EQ     0x0002
#define WO_LT     (WO_EQ<<(TK_LT-TK_EQ))
#define WO_LE     (WO_EQ<<(TK_LE-TK_EQ))
#define WO_GT     (WO_EQ<<(TK_GT-TK_EQ))
#define WO_GE     (WO_EQ<<(TK_GE-TK_EQ))
#define WO_AUX    0x0040       /* op_t useful to virtual tables only */
#define WO_IS     0x0080
#define WO_ISNULL 0x0100
#define WO_OR     0x0200       /* Two or more OR-connected terms */
#define WO_AND    0x0400       /* Two or more AND-connected terms */
#define WO_EQUIV  0x0800       /* Of the form A==B, both columns */
#define WO_NOOP   0x1000       /* This term does not restrict search space */
#define WO_ROWVAL 0x2000       /* A row-value term */

#define WO_ALL    0x3fff       /* Mask of all possible WO_* values */
#define WO_SINGLE 0x01ff       /* Mask of all non-compound WO_* values */

/*
** These are definitions of bits in the where_loop_t.ws_flags field.
** The particular combination of bits in each where_loop_t help to
** determine the algorithm that where_loop_t represents.
*/
#define WHERE_COLUMN_EQ    0x00000001  /* x=EXPR */
#define WHERE_COLUMN_RANGE 0x00000002  /* x<EXPR and/or x>EXPR */
#define WHERE_COLUMN_IN    0x00000004  /* x IN (...) */
#define WHERE_COLUMN_NULL  0x00000008  /* x IS NULL */
#define WHERE_CONSTRAINT   0x0000000f  /* Any of the WHERE_COLUMN_xxx values */
#define WHERE_TOP_LIMIT    0x00000010  /* x<EXPR or x<=EXPR constraint */
#define WHERE_BTM_LIMIT    0x00000020  /* x>EXPR or x>=EXPR constraint */
#define WHERE_BOTH_LIMIT   0x00000030  /* Both x>EXPR and x<EXPR */
#define WHERE_IDX_ONLY     0x00000040  /* Use index only - omit table */
#define WHERE_IPK          0x00000100  /* x is the INTEGER PRIMARY KEY */
#define WHERE_INDEXED      0x00000200  /* where_loop_t.u.btree.p_index is valid */
#define WHERE_VIRTUALTABLE 0x00000400  /* where_loop_t.u.vtab is valid */
#define WHERE_IN_ABLE      0x00000800  /* Able to support an IN operator */
#define WHERE_ONEROW       0x00001000  /* Selects no more than one row */
#define WHERE_MULTI_OR     0x00002000  /* OR using multiple indices */
#define WHERE_AUTO_INDEX   0x00004000  /* Uses an ephemeral index */
#define WHERE_SKIPSCAN     0x00008000  /* Uses the skip-scan algorithm */
#define WHERE_UNQ_WANTED   0x00010000  /* WHERE_ONEROW would have been helpful*/
#define WHERE_PARTIALIDX   0x00020000  /* The automatic index is partial */
#define WHERE_IN_EARLYOUT  0x00040000  /* Perhaps quit IN loops early */
#define WHERE_BIGNULL_SORT 0x00080000  /* column_t n_eq of index is BIGNULL */
#define WHERE_IN_SEEKSCAN  0x00100000  /* Seek-scan optimization for IN */
#define WHERE_TRANSCONS    0x00200000  /* Uses a transitive constraint */
#define WHERE_BLOOMFILTER  0x00400000  /* Consider using a Bloom-filter */
#define WHERE_SELFCULL     0x00800000  /* n_out reduced by extra WHERE terms */
#define WHERE_OMIT_OFFSET  0x01000000  /* Set offset counter to zero */
#define WHERE_COROUTINE    0x02000000  /* Implemented by co-routine.
                                       ** NB: False-negatives are possible */
#define WHERE_EXPRIDX      0x04000000  /* Uses an index-on-expressions */

#endif /* !defined(DBSQL_WHEREINT_H) */
