/*
** 2011 March 24
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
** Code for a demonstration virtual table that generates variations
** on an input word at increasing edit distances from the original.
**
** A fuzzer virtual table is created like this:
**
**     CREATE VIRTUAL TABLE f USING fuzzer(<fuzzer-data-table>);
**
** When it is created, the new fuzzer table must be supplied with the
** name of a "fuzzer data table", which must reside in the same database
** file as the new fuzzer table. The fuzzer data table contains the various
** transformations and their costs that the fuzzer logic uses to generate
** variations.
**
** The fuzzer data table must contain exactly four columns (more precisely,
** the statement "SELECT * FROM <fuzzer_data_table>" must return records
** that consist of four columns). It does not matter what the columns are
** named. 
**
** Each row in the fuzzer data table represents a single character
** transformation. The left most column of the row (column 0) contains an
** integer value - the identifier of the ruleset to which the transformation
** rule belongs (see "MULTIPLE RULE SETS" below). The second column of the
** row (column 0) contains the input character or characters. The third 
** column contains the output character or characters. And the fourth column
** contains the integer cost of making the transformation. For example:
**
**    CREATE TABLE f_data(ruleset, c_from, c_to, Cost);
**    INSERT INTO f_data(ruleset, c_from, c_to, Cost) VALUES(0, '', 'a', 100);
**    INSERT INTO f_data(ruleset, c_from, c_to, Cost) VALUES(0, 'b', '', 87);
**    INSERT INTO f_data(ruleset, c_from, c_to, Cost) VALUES(0, 'o', 'oe', 38);
**    INSERT INTO f_data(ruleset, c_from, c_to, Cost) VALUES(0, 'oe', 'o', 40);
**
** The first row inserted into the fuzzer data table by the SQL script
** above indicates that the cost of inserting a letter 'a' is 100.  (All 
** costs are integers.  We recommend that costs be scaled so that the 
** average cost is around 100.) The second INSERT statement creates a rule
** saying that the cost of deleting a single letter 'b' is 87.  The third
** and fourth INSERT statements mean that the cost of transforming a
** single letter "o" into the two-letter sequence "oe" is 38 and that the
** cost of transforming "oe" back into "o" is 40.
**
** The contents of the fuzzer data table are loaded into main memory when
** a fuzzer table is first created, and may be internally reloaded by the
** system at any subsequent time. Therefore, the fuzzer data table should be 
** populated before the fuzzer table is created and not modified thereafter.
** If you do need to modify the contents of the fuzzer data table, it is
** recommended that the associated fuzzer table be dropped, the fuzzer data
** table edited, and the fuzzer table recreated within a single transaction.
** Alternatively, the fuzzer data table can be edited then the database
** connection can be closed and reopened.
**
** Once it has been created, the fuzzer table can be queried as follows:
**
**    SELECT word, distance FROM f
**     WHERE word MATCH 'abcdefg'
**       AND distance<200;
**
** This first query outputs the string "abcdefg" and all strings that
** can be derived from that string by applying the specified transformations.
** The strings are output together with their total transformation cost
** (called "distance") and appear in order of increasing cost.  No string
** is output more than once.  If there are multiple ways to transform the
** target string into the output string then the lowest cost transform is
** the one that is returned.  In the example, the search is limited to 
** strings with a total distance of less than 200.
**
** The fuzzer is a read-only table.  Any attempt to DELETE, INSERT, or
** UPDATE on a fuzzer table will throw an error.
**
** It is important to put some kind of a limit on the fuzzer output.  This
** can be either in the form of a LIMIT clause at the end of the query,
** or better, a "distance<NNN" constraint where NNN is some number.  The
** running time and memory requirement is exponential in the value of NNN 
** so you want to make sure that NNN is not too big.  A value of NNN that
** is about twice the average transformation cost seems to give good results.
**
** The fuzzer table can be useful for tasks such as spelling correction.
** Suppose there is a second table vocabulary(w) where the w column contains
** all correctly spelled words.   Let $word be a word you want to look up.
**
**   SELECT vocabulary.w FROM f, vocabulary
**    WHERE f.word MATCH $word
**      AND f.distance<=200
**      AND f.word=vocabulary.w
**    LIMIT 20
**
** The query above gives the 20 closest words to the $word being tested.
** (Note that for good performance, the vocabulary.w column should be
** indexed.)
**
** A similar query can be used to find all words in the dictionary that
** begin with some prefix $prefix:
**
**   SELECT vocabulary.w FROM f, vocabulary
**    WHERE f.word MATCH $prefix
**      AND f.distance<=200
**      AND vocabulary.w BETWEEN f.word AND (f.word || x'F7BFBFBF')
**    LIMIT 50
**
** This last query will show up to 50 words out of the vocabulary that
** match or nearly match the $prefix.
**
** MULTIPLE RULE SETS
**
** Normally, the "ruleset" value associated with all character transformations
** in the fuzzer data table is zero. However, if required, the fuzzer table
** allows multiple rulesets to be defined. Each query uses only a single
** ruleset. This allows, for example, a single fuzzer table to support 
** multiple languages.
**
** By default, only the rules from ruleset 0 are used. To specify an 
** alternative ruleset, a "ruleset = ?" expression must be added to the
** WHERE clause of a SELECT, where ? is the identifier of the desired 
** ruleset. For example:
**
**   SELECT vocabulary.w FROM f, vocabulary
**    WHERE f.word MATCH $word
**      AND f.distance<=200
**      AND f.word=vocabulary.w
**      AND f.ruleset=1  -- Specify the ruleset to use here
**    LIMIT 20
**
** If no "ruleset = ?" constraint is specified in the WHERE clause, ruleset 
** 0 is used.
**
** LIMITS
**
** The maximum ruleset number is 2147483647.  The maximum length of either
** of the strings in the second or third column of the fuzzer data table
** is 50 bytes.  The maximum cost on a rule is 1000.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1

/* If DBSQL_DEBUG is not defined, disable assert statements. */
#if !defined(NDEBUG) && !defined(DBSQL_DEBUG)
# define NDEBUG
#endif

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** Forward declaration of objects used by this implementation
*/
typedef struct fuzzer_vtab fuzzer_vtab;
typedef struct fuzzer_cursor fuzzer_cursor;
typedef struct fuzzer_rule fuzzer_rule;
typedef struct fuzzer_seen fuzzer_seen;
typedef struct fuzzer_stem fuzzer_stem;

/*
** Various types.
**
** fuzzer_cost is the "cost" of an edit operation.
**
** fuzzer_len is the length of a matching string.  
**
** fuzzer_ruleid is an ruleset identifier.
*/
typedef int fuzzer_cost;
typedef signed char fuzzer_len;
typedef int fuzzer_ruleid;

/*
** Limits
*/
#define FUZZER_MX_LENGTH           50   /* Maximum length of a rule string */
#define FUZZER_MX_RULEID   2147483647   /* Maximum rule ID */
#define FUZZER_MX_COST           1000   /* Maximum single-rule cost */
#define FUZZER_MX_OUTPUT_LENGTH   100   /* Maximum length of an output string */


/*
** Each transformation rule is stored as an instance of this object.
** All rules are kept on a linked list sorted by r_cost.
*/
struct fuzzer_rule {
  fuzzer_rule *next;         /* Next rule in order of increasing r_cost */
  char *from;                /* Transform from */
  fuzzer_cost r_cost;          /* Cost of this transformation */
  fuzzer_len n_from, n_to;      /* Length of the from and to strings */
  fuzzer_ruleid i_ruleset;     /* The rule set to which this rule belongs */
  char to[4];                /* Transform to (extra space appended) */
};

/*
** A stem object is used to generate variants.  It is also used to record
** previously generated outputs.
**
** Every stem is added to a hash table as it is output.  Generation of
** duplicate stems is suppressed.
**
** Active stems (those that might generate new outputs) are kept on a linked
** list sorted by increasing cost.  The cost is the sum of r_base_cost and
** rule->r_cost.
*/
struct fuzzer_stem {
  char *basis;              /* Word being fuzzed */
  const fuzzer_rule *rule;  /* Current rule to apply */
  fuzzer_stem *next;        /* Next stem in r_cost order */
  fuzzer_stem *hash;        /* Next stem with same hash on basis */
  fuzzer_cost r_base_cost;     /* Base cost of getting to basis */
  fuzzer_cost r_cost_x;        /* Precomputed r_base_cost + rule->r_cost */
  fuzzer_len n_basis;         /* Length of the basis string */
  fuzzer_len n;              /* Apply rule at this character offset */
};

/* 
** A fuzzer virtual-table object 
*/
struct fuzzer_vtab {
  dbsql_vtab_t base;         /* Base class - must be first */
  char *class_name;          /* Name of this class.  Default: "fuzzer" */
  fuzzer_rule *rule;        /* All active rules in this fuzzer */
  int n_cursor;               /* Number of active cursors */
};

#define FUZZER_HASH  4001    /* hash_t table size */
#define FUZZER_NQUEUE  20    /* Number of slots on the stem queue */

/* A fuzzer cursor object */
struct fuzzer_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  int64_t i_rowid;      /* The rowid of the current word */
  fuzzer_vtab *vtab;        /* The virtual table this cursor belongs to */
  fuzzer_cost r_limit;        /* Maximum cost of any term */
  fuzzer_stem *stem;        /* Stem with smallest r_cost_x */
  fuzzer_stem *done;        /* Stems already processed to completion */
  fuzzer_stem *queues[FUZZER_NQUEUE];  /* Queue of stems with higher r_cost_x */
  int mx_queue;               /* Largest used index in queues[] */
  char *buf;                /* Temporary use buffer */
  int n_buf;                  /* Bytes allocated for buf */
  int n_stem;                 /* Number of stems allocated */
  int i_ruleset;              /* Only process rules from this ruleset */
  fuzzer_rule null_rule;      /* Null rule used first */
  fuzzer_stem *hashes[FUZZER_HASH]; /* hash_t of previously generated terms */
};

/*
** The two input rule lists are both sorted in order of increasing
** cost.  Merge them together into a single list, sorted by cost, and
** return a pointer to the head of that list.
*/
static fuzzer_rule *__fuzzer_merge_rules(fuzzer_rule *a, fuzzer_rule *b){
  fuzzer_rule head;
  fuzzer_rule *tail;

  tail =  &head;
  while( a && b ){
    if( a->r_cost<=b->r_cost ){
      tail->next = a;
      tail = a;
      a = a->next;
    }else{
      tail->next = b;
      tail = b;
      b = b->next;
    }
  }
  if( a==0 ){
    tail->next = b;
  }else{
    tail->next = a;
  }
  return head.next;
}

/*
** Statement stmt currently points to a row in the fuzzer data table. This
** function allocates and populates a fuzzer_rule structure according to
** the content of the row.
**
** If successful, *rule is set to point to the new object and DBSQL_OK
** is returned. Otherwise, *rule is zeroed, *err_p may be set to point
** to an error message and an dbsql error code returned.
*/
static int __fuzzer_load_one_rule(
  fuzzer_vtab *p,                 /* Fuzzer virtual table handle */
  dbsql_stmt_t *stmt,            /* Base rule on statements current row */
  fuzzer_rule **pp_rule,           /* OUT: New rule object */
  char **err_p                    /* OUT: Error message */
){
  int64_t i_ruleset = dbsql_column_int64(stmt, 0);
  const char *from = (const char *)dbsql_column_text(stmt, 1);
  const char *to = (const char *)dbsql_column_text(stmt, 2);
  int n_cost = dbsql_column_int(stmt, 3);

  int rc = DBSQL_OK;             /* Return code */
  int n_from;                      /* Size of string from, in bytes */
  int n_to;                        /* Size of string to, in bytes */
  fuzzer_rule *rule = 0;         /* New rule object to return */

  if( from==0 ) from = "";
  if( to==0 ) to = "";
  n_from = (int)strlen(from);
  n_to = (int)strlen(to);

  /* Silently ignore null transformations */
  if( strcmp(from, to)==0 ){
    *pp_rule = 0;
    return DBSQL_OK;
  }

  if( n_cost<=0 || n_cost>FUZZER_MX_COST ){
    *err_p = dbsql_mprintf("%s: cost must be between 1 and %d", 
        p->class_name, FUZZER_MX_COST
    );
    rc = DBSQL_ERROR;
  }else
  if( n_from>FUZZER_MX_LENGTH || n_to>FUZZER_MX_LENGTH ){
    *err_p = dbsql_mprintf("%s: maximum string length is %d", 
        p->class_name, FUZZER_MX_LENGTH
    );
    rc = DBSQL_ERROR;    
  }else
  if( i_ruleset<0 || i_ruleset>FUZZER_MX_RULEID ){
    *err_p = dbsql_mprintf("%s: ruleset must be between 0 and %d", 
        p->class_name, FUZZER_MX_RULEID
    );
    rc = DBSQL_ERROR;    
  }else{

    rule = dbsql_malloc64( sizeof(*rule) + n_from + n_to );
    if( rule==0 ){
      rc = DBSQL_NOMEM;
    }else{
      memset(rule, 0, sizeof(*rule));
      rule->from = rule->to;
      rule->from += n_to + 1;
      rule->n_from = (fuzzer_len)n_from;
      memcpy(rule->from, from, n_from+1);
      memcpy(rule->to, to, n_to+1);
      rule->n_to = (fuzzer_len)n_to;
      rule->r_cost = n_cost;
      rule->i_ruleset = (int)i_ruleset;
    }
  }

  *pp_rule = rule;
  return rc;
}

/*
** Load the content of the fuzzer data table into memory.
*/
static int __fuzzer_load_rules(
  DBSQL *db,                    /* Database handle */
  fuzzer_vtab *p,                 /* Virtual fuzzer table to configure */
  const char *z_db,                /* Database containing rules data */
  const char *data,              /* table_t containing rules data */
  char **err_p                    /* OUT: Error message */
){
  int rc = DBSQL_OK;             /* Return code */
  char *sql;                     /* SELECT used to read from rules table */
  fuzzer_rule *head = 0;

  sql = dbsql_mprintf("SELECT * FROM %Q.%Q", z_db, data);
  if( sql==0 ){
    rc = DBSQL_NOMEM;
  }else{
    int rc2;                      /* finalize() return code */
    dbsql_stmt_t *stmt = 0;
    rc = dbsql_prepare_v2(db, sql, -1, &stmt, 0);
    if( rc!=DBSQL_OK ){
      *err_p = dbsql_mprintf("%s: %s", p->class_name, dbsql_errmsg(db));
    }else if( dbsql_column_count(stmt)!=4 ){
      *err_p = dbsql_mprintf("%s: %s has %d columns, expected 4",
          p->class_name, data, dbsql_column_count(stmt)
      );
      rc = DBSQL_ERROR;
    }else{
      while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(stmt) ){
        fuzzer_rule *rule = 0;
        rc = __fuzzer_load_one_rule(p, stmt, &rule, err_p);
        if( rule ){
          rule->next = head;
          head = rule;
        }
      }
    }
    rc2 = dbsql_finalize(stmt);
    if( rc==DBSQL_OK ) rc = rc2;
  }
  dbsql_free(sql);

  /* All rules are now in a singly linked list starting at head. This
  ** block sorts them by cost and then sets fuzzer_vtab.rule to point to 
  ** point to the head of the sorted list.
  */
  if( rc==DBSQL_OK ){
    unsigned int i;
    fuzzer_rule *x;
    fuzzer_rule *a[15];
    for(i=0; i<sizeof(a)/sizeof(a[0]); i++) a[i] = 0;
    while( (x = head)!=0 ){
      head = x->next;
      x->next = 0;
      for(i=0; a[i] && i<sizeof(a)/sizeof(a[0])-1; i++){
        x = __fuzzer_merge_rules(a[i], x);
        a[i] = 0;
      }
      a[i] = __fuzzer_merge_rules(a[i], x);
    }
    for(x=a[0], i=1; i<sizeof(a)/sizeof(a[0]); i++){
      x = __fuzzer_merge_rules(a[i], x);
    }
    p->rule = __fuzzer_merge_rules(p->rule, x);
  }else{
    /* An error has occurred. Setting p->rule to point to the head of the
    ** allocated list ensures that the list will be cleaned up in this case.
    */
    assert( p->rule==0 );
    p->rule = head;
  }

  return rc;
}

/*
** This function converts an SQL quoted string into an unquoted string
** and returns a pointer to a buffer allocated using dbsql_malloc() 
** containing the result. The caller should eventually free this buffer
** using dbsql_free.
**
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
*/
static char *__fuzzer_dequote(const char *in){
  int64_t n_in;              /* Size of input string, in bytes */
  char *out;                     /* Output (dequoted) string */

  n_in = strlen(in);
  out = dbsql_malloc64(n_in+1);
  if( out ){
    char q = in[0];              /* Quote character (if any ) */

    if( q!='[' && q!= '\'' && q!='"' && q!='`' ){
      memcpy(out, in, (size_t)(n_in+1));
    }else{
      int i_out = 0;               /* index_t of next byte to write to output */
      int i_in;                    /* index_t of next byte to read from input */

      if( q=='[' ) q = ']';
      for(i_in=1; i_in<n_in; i_in++){
        if( in[i_in]==q ) i_in++;
        out[i_out++] = in[i_in];
      }
      assert( i_out<=n_in );
      out[i_out] = 0;
    }
    assert( (int)strlen(out)<=n_in );
  }
  return out;
}

/*
** disconnect/destroy method for the fuzzer module.
*/
static int __fuzzer_disconnect(dbsql_vtab_t *vtab){
  fuzzer_vtab *p = (fuzzer_vtab*)vtab;
  assert( p->n_cursor==0 );
  while( p->rule ){
    fuzzer_rule *rule = p->rule;
    p->rule = rule->next;
    dbsql_free(rule);
  }
  dbsql_free(p);
  return DBSQL_OK;
}

/*
** connect/create method for the fuzzer module. Arguments are:
**
**   argv[0]   -> module name  ("fuzzer")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[3]   -> fuzzer rule table name
*/
static int __fuzzer_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  int rc = DBSQL_OK;             /* Return code */
  fuzzer_vtab *p_new = 0;          /* New virtual table */
  const char *module = argv[0];
  const char *z_db = argv[1];

  if( argc!=4 ){
    *err_p = dbsql_mprintf(
        "%s: wrong number of CREATE VIRTUAL TABLE arguments", module
    );
    rc = DBSQL_ERROR;
  }else{
    int64_t n_module;        /* Length of module, in bytes */

    n_module = strlen(module);
    p_new = dbsql_malloc64( sizeof(*p_new) + n_module + 1);
    if( p_new==0 ){
      rc = DBSQL_NOMEM;
    }else{
      char *tab;                 /* Dequoted name of fuzzer data table */

      memset(p_new, 0, sizeof(*p_new));
      p_new->class_name = (char*)&p_new[1];
      memcpy(p_new->class_name, module, (size_t)(n_module+1));

      tab = __fuzzer_dequote(argv[3]);
      if( tab==0 ){
        rc = DBSQL_NOMEM;
      }else{
        rc = __fuzzer_load_rules(db, p_new, z_db, tab, err_p);
        dbsql_free(tab);
      }

      if( rc==DBSQL_OK ){
        rc = dbsql_declare_vtab(db, "CREATE TABLE x(word,distance,ruleset)");
      }
      if( rc!=DBSQL_OK ){
        __fuzzer_disconnect((dbsql_vtab_t *)p_new);
        p_new = 0;
      }else{
        dbsql_vtab_config(db, DBSQL_VTAB_INNOCUOUS);
      }
    }
  }

  *vtab = (dbsql_vtab_t *)p_new;
  return rc;
}

/*
** Open a new fuzzer cursor.
*/
static int __fuzzer_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  fuzzer_vtab *p = (fuzzer_vtab*)v_tab;
  fuzzer_cursor *cur;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  cur->vtab = p;
  *cursor = &cur->base;
  p->n_cursor++;
  return DBSQL_OK;
}

/*
** Free all stems in a list.
*/
static void __fuzzer_clear_stem_list(fuzzer_stem *stem){
  while( stem ){
    fuzzer_stem *next = stem->next;
    dbsql_free(stem);
    stem = next;
  }
}

/*
** Free up all the memory allocated by a cursor.  Set it r_limit to 0
** to indicate that it is at EOF.
*/
static void __fuzzer_clear_cursor(fuzzer_cursor *cur, int clear_hash){
  int i;
  __fuzzer_clear_stem_list(cur->stem);
  __fuzzer_clear_stem_list(cur->done);
  for(i=0; i<FUZZER_NQUEUE; i++) __fuzzer_clear_stem_list(cur->queues[i]);
  cur->r_limit = (fuzzer_cost)0;
  if( clear_hash && cur->n_stem ){
    cur->mx_queue = 0;
    cur->stem = 0;
    cur->done = 0;
    memset(cur->queues, 0, sizeof(cur->queues));
    memset(cur->hashes, 0, sizeof(cur->hashes));
  }
  cur->n_stem = 0;
}

/*
** Close a fuzzer cursor.
*/
static int __fuzzer_close(dbsql_vtab_cursor_t *cur){
  fuzzer_cursor *p_cur = (fuzzer_cursor *)cur;
  __fuzzer_clear_cursor(p_cur, 0);
  dbsql_free(p_cur->buf);
  p_cur->vtab->n_cursor--;
  dbsql_free(p_cur);
  return DBSQL_OK;
}

/*
** Compute the current output term for a fuzzer_stem.
*/
static int __fuzzer_render(
  fuzzer_stem *stem,   /* The stem to be rendered */
  char **buf,         /* Write results into this buffer.  realloc if needed */
  int *pn_buf            /* Size of the buffer */
){
  const fuzzer_rule *rule = stem->rule;
  int64_t n;                /* Size of output term without nul-term */
  char *z;                        /* Buffer to assemble output term in */

  n = stem->n_basis + rule->n_to - rule->n_from;
  if( (*pn_buf)<n+1 ){
    (*buf) = dbsql_realloc64((*buf), n+100);
    if( (*buf)==0 ) return DBSQL_NOMEM;
    (*pn_buf) = n+100;
  }
  n = stem->n;
  z = *buf;
  if( n<0 ){
    memcpy(z, stem->basis, stem->n_basis+1);
  }else{
    memcpy(z, stem->basis, n);
    memcpy(&z[n], rule->to, rule->n_to);
    memcpy(&z[n+rule->n_to], &stem->basis[n+rule->n_from], 
           stem->n_basis-n-rule->n_from+1);
  }

  assert( z[stem->n_basis + rule->n_to - rule->n_from]==0 );
  return DBSQL_OK;
}

/*
** Compute a hash on basis.
*/
static unsigned int __fuzzer_hash(const char *z){
  unsigned int h = 0;
  while( *z ){ h = (h<<3) ^ (h>>29) ^ *(z++); }
  return h % FUZZER_HASH;
}

/*
** Current cost of a stem
*/
static fuzzer_cost __fuzzer_cost(fuzzer_stem *stem){
  return stem->r_cost_x = stem->r_base_cost + stem->rule->r_cost;
}

#if 0
/*
** Print a description of a fuzzer_stem on stderr.
*/
static void __fuzzer_stem_print(
  const char *prefix,
  fuzzer_stem *stem,
  const char *suffix
){
  if( stem->n<0 ){
    fprintf(stderr, "%s[%s](%d)-->self%s",
       prefix,
       stem->basis, stem->r_base_cost,
       suffix
    );
  }else{
    char *buf = 0;
    int n_buf = 0;
    if( __fuzzer_render(stem, &buf, &n_buf)!=DBSQL_OK ) return;
    fprintf(stderr, "%s[%s](%d)-->{%s}(%d)%s",
      prefix,
      stem->basis, stem->r_base_cost, buf, stem->,
      suffix
    );
    dbsql_free(buf);
  }
}
#endif

/*
** Return 1 if the string to which the cursor is point has already
** been emitted.  Return 0 if not.  Return -1 on a memory allocation
** failures.
*/
static int __fuzzer_seen(fuzzer_cursor *cur, fuzzer_stem *stem){
  unsigned int h;
  fuzzer_stem *lookup;

  if( __fuzzer_render(stem, &cur->buf, &cur->n_buf)==DBSQL_NOMEM ){
    return -1;
  }
  h = __fuzzer_hash(cur->buf);
  lookup = cur->hashes[h];
  while( lookup && strcmp(lookup->basis, cur->buf)!=0 ){
    lookup = lookup->hash;
  }
  return lookup!=0;
}

/*
** If argument rule is NULL, this function returns false.
**
** Otherwise, it returns true if rule rule should be skipped. A rule 
** should be skipped if it does not belong to rule-set i_ruleset, or if
** applying it to stem stem would create a string longer than 
** FUZZER_MX_OUTPUT_LENGTH bytes.
*/
static int __fuzzer_skip_rule(
  const fuzzer_rule *rule,       /* Determine whether or not to skip this */
  fuzzer_stem *stem,             /* Stem rule may be applied to */
  int i_ruleset                    /* Rule-set used by the current query */
){
  return rule && (
      (rule->i_ruleset!=i_ruleset)
   || (stem->n_basis + rule->n_to - rule->n_from)>FUZZER_MX_OUTPUT_LENGTH
  );
}

/*
** Advance a fuzzer_stem to its next value.   Return 0 if there are
** no more values that can be generated by this fuzzer_stem.  Return
** -1 on a memory allocation failure.
*/
static int __fuzzer_advance(fuzzer_cursor *cur, fuzzer_stem *stem){
  const fuzzer_rule *rule;
  while( (rule = stem->rule)!=0 ){
    assert( rule==&cur->null_rule || rule->i_ruleset==cur->i_ruleset );
    while( stem->n < stem->n_basis - rule->n_from ){
      stem->n++;
      if( rule->n_from==0
       || memcmp(&stem->basis[stem->n], rule->from, rule->n_from)==0
      ){
        /* Found a rewrite case.  Make sure it is not a duplicate */
        int rc = __fuzzer_seen(cur, stem);
        if( rc<0 ) return -1;
        if( rc==0 ){
          __fuzzer_cost(stem);
          return 1;
        }
      }
    }
    stem->n = -1;
    do{
      rule = rule->next;
    }while( __fuzzer_skip_rule(rule, stem, cur->i_ruleset) );
    stem->rule = rule;
    if( rule && __fuzzer_cost(stem)>cur->r_limit ) stem->rule = 0;
  }
  return 0;
}

/*
** The two input stem lists are both sorted in order of increasing
** r_cost_x.  Merge them together into a single list, sorted by r_cost_x, and
** return a pointer to the head of that new list.
*/
static fuzzer_stem *__fuzzer_merge_stems(fuzzer_stem *a, fuzzer_stem *b){
  fuzzer_stem head;
  fuzzer_stem *tail;

  tail =  &head;
  while( a && b ){
    if( a->r_cost_x<=b->r_cost_x ){
      tail->next = a;
      tail = a;
      a = a->next;
    }else{
      tail->next = b;
      tail = b;
      b = b->next;
    }
  }
  if( a==0 ){
    tail->next = b;
  }else{
    tail->next = a;
  }
  return head.next;
}

/*
** Load cur->stem with the lowest-cost stem.  Return a pointer
** to the lowest-cost stem.
*/
static fuzzer_stem *__fuzzer_lowest_cost_stem(fuzzer_cursor *cur){
  fuzzer_stem *best, *x;
  int i_best;
  int i;

  if( cur->stem==0 ){
    i_best = -1;
    best = 0;
    for(i=0; i<=cur->mx_queue; i++){
      x = cur->queues[i];
      if( x==0 ) continue;
      if( best==0 || best->r_cost_x>x->r_cost_x ){
        best = x;
        i_best = i;
      }
    } 
    if( best ){
      cur->queues[i_best] = best->next;
      best->next = 0;
      cur->stem = best;
    }
  }
  return cur->stem;
}

/*
** Insert p_new into queue of pending stems.  Then find the stem
** with the lowest r_cost_x and move it into cur->stem.
** list.  The insert is done such the p_new is in the correct order
** according to fuzzer_stem.base_cost+fuzzer_stem.rule->r_cost.
*/
static fuzzer_stem *__fuzzer_insert(fuzzer_cursor *cur, fuzzer_stem *p_new){
  fuzzer_stem *x;
  int i;

  /* If cur->stem exists and is greater than p_new, then make p_new
  ** the new cur->stem and insert the old cur->stem instead.
  */
  if( (x = cur->stem)!=0 && x->r_cost_x>p_new->r_cost_x ){
    p_new->next = 0;
    cur->stem = p_new;
    p_new = x;
  }

  /* Insert the new value */
  p_new->next = 0;
  x = p_new;
  for(i=0; i<=cur->mx_queue; i++){
    if( cur->queues[i] ){
      x = __fuzzer_merge_stems(x, cur->queues[i]);
      cur->queues[i] = 0;
    }else{
      cur->queues[i] = x;
      break;
    }
  }
  if( i>cur->mx_queue ){
    if( i<FUZZER_NQUEUE ){
      cur->mx_queue = i;
      cur->queues[i] = x;
    }else{
      assert( cur->mx_queue==FUZZER_NQUEUE-1 );
      x = __fuzzer_merge_stems(x, cur->queues[FUZZER_NQUEUE-1]);
      cur->queues[FUZZER_NQUEUE-1] = x;
    }
  }

  return __fuzzer_lowest_cost_stem(cur);
}

/*
** Allocate a new fuzzer_stem.  Add it to the hash table but do not
** link it into either the cur->stem or cur->done lists.
*/
static fuzzer_stem *__fuzzer_new_stem(
  fuzzer_cursor *cur,
  const char *word,
  fuzzer_cost r_base_cost
){
  fuzzer_stem *p_new;
  fuzzer_rule *rule;
  unsigned int h;

  p_new = dbsql_malloc64( sizeof(*p_new) + strlen(word) + 1 );
  if( p_new==0 ) return 0;
  memset(p_new, 0, sizeof(*p_new));
  p_new->basis = (char*)&p_new[1];
  p_new->n_basis = (fuzzer_len)strlen(word);
  memcpy(p_new->basis, word, p_new->n_basis+1);
  rule = cur->vtab->rule;
  while( __fuzzer_skip_rule(rule, p_new, cur->i_ruleset) ){
    rule = rule->next;
  }
  p_new->rule = rule;
  p_new->n = -1;
  p_new->r_base_cost = p_new->r_cost_x = r_base_cost;
  h = __fuzzer_hash(p_new->basis);
  p_new->hash = cur->hashes[h];
  cur->hashes[h] = p_new;
  cur->n_stem++;
  return p_new;
}


/*
** Advance a cursor to its next row of output
*/
static int __fuzzer_next(dbsql_vtab_cursor_t *cur){
  fuzzer_cursor *p_cur = (fuzzer_cursor*)cur;
  int rc;
  fuzzer_stem *stem, *p_new;

  p_cur->i_rowid++;

  /* Use the element the cursor is currently point to to create
  ** a new stem and insert the new stem into the priority queue.
  */
  stem = p_cur->stem;
  if( stem->r_cost_x>0 ){
    rc = __fuzzer_render(stem, &p_cur->buf, &p_cur->n_buf);
    if( rc==DBSQL_NOMEM ) return DBSQL_NOMEM;
    p_new = __fuzzer_new_stem(p_cur, p_cur->buf, stem->r_cost_x);
    if( p_new ){
      if( __fuzzer_advance(p_cur, p_new)==0 ){
        p_new->next = p_cur->done;
        p_cur->done = p_new;
      }else{
        if( __fuzzer_insert(p_cur, p_new)==p_new ){
          return DBSQL_OK;
        }
      }
    }else{
      return DBSQL_NOMEM;
    }
  }

  /* Adjust the priority queue so that the first element of the
  ** stem list is the next lowest cost word.
  */
  while( (stem = p_cur->stem)!=0 ){
    int res = __fuzzer_advance(p_cur, stem);
    if( res<0 ){
      return DBSQL_NOMEM;
    }else if( res>0 ){
      p_cur->stem = 0;
      stem = __fuzzer_insert(p_cur, stem);
      if( (rc = __fuzzer_seen(p_cur, stem))!=0 ){
        if( rc<0 ) return DBSQL_NOMEM;
        continue;
      }
      return DBSQL_OK;  /* New word found */
    }
    p_cur->stem = 0;
    stem->next = p_cur->done;
    p_cur->done = stem;
    if( __fuzzer_lowest_cost_stem(p_cur) ){
      rc = __fuzzer_seen(p_cur, p_cur->stem);
      if( rc<0 ) return DBSQL_NOMEM;
      if( rc==0 ){
        return DBSQL_OK;
      }
    }
  }

  /* Reach this point only if queue has been exhausted and there is
  ** nothing left to be output. */
  p_cur->r_limit = (fuzzer_cost)0;
  return DBSQL_OK;
}

/*
** Called to "rewind" a cursor back to the beginning so that
** it starts its output over again.  Always called at least once
** prior to any __fuzzer_column, __fuzzer_rowid, or __fuzzer_eof call.
*/
static int __fuzzer_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  fuzzer_cursor *cur = (fuzzer_cursor *)vtab_cursor;
  const char *word = "";
  fuzzer_stem *stem;
  int idx;

  __fuzzer_clear_cursor(cur, 1);
  cur->r_limit = 2147483647;
  idx = 0;
  if( idx_num & 1 ){
    word = (const char*)dbsql_value_text(argv[0]);
    if( word==0 ) word = "";
    idx++;
  }
  if( idx_num & 2 ){
    cur->r_limit = (fuzzer_cost)dbsql_value_int(argv[idx]);
    idx++;
  }
  if( idx_num & 4 ){
    cur->i_ruleset = (fuzzer_cost)dbsql_value_int(argv[idx]);
    idx++;
  }
  cur->null_rule.next = cur->vtab->rule;
  cur->null_rule.r_cost = 0;
  cur->null_rule.n_from = 0;
  cur->null_rule.n_to = 0;
  cur->null_rule.from = "";
  cur->i_rowid = 1;
  assert( cur->stem==0 );

  /* If the query term is longer than FUZZER_MX_OUTPUT_LENGTH bytes, this
  ** query will return zero rows.  */
  if( (int)strlen(word)<FUZZER_MX_OUTPUT_LENGTH ){
    cur->stem = stem = __fuzzer_new_stem(cur, word, (fuzzer_cost)0);
    if( stem==0 ) return DBSQL_NOMEM;
    stem->rule = &cur->null_rule;
    stem->n = stem->n_basis;
  }else{
    cur->r_limit = 0;
  }

  return DBSQL_OK;
}

/*
** Only the word and distance columns have values.  All other columns
** return NULL
*/
static int __fuzzer_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  fuzzer_cursor *p_cur = (fuzzer_cursor*)cur;
  if( i==0 ){
    /* the "word" column */
    if( __fuzzer_render(p_cur->stem, &p_cur->buf, &p_cur->n_buf)==DBSQL_NOMEM ){
      return DBSQL_NOMEM;
    }
    dbsql_result_text(ctx, p_cur->buf, -1, DBSQL_TRANSIENT);
  }else if( i==1 ){
    /* the "distance" column */
    dbsql_result_int(ctx, p_cur->stem->r_cost_x);
  }else{
    /* All other columns are NULL */
    dbsql_result_null(ctx);
  }
  return DBSQL_OK;
}

/*
** The rowid.
*/
static int __fuzzer_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  fuzzer_cursor *p_cur = (fuzzer_cursor*)cur;
  *rowid = p_cur->i_rowid;
  return DBSQL_OK;
}

/*
** When the fuzzer_cursor.r_limit value is 0 or less, that is a signal
** that the cursor has nothing more to output.
*/
static int __fuzzer_eof(dbsql_vtab_cursor_t *cur){
  fuzzer_cursor *p_cur = (fuzzer_cursor*)cur;
  return p_cur->r_limit<=(fuzzer_cost)0;
}

/*
** Search for terms of these forms:
**
**   (A)    word MATCH $str
**   (B1)   distance < $value
**   (B2)   distance <= $value
**   (C)    ruleid == $ruleid
**
** The distance< and distance<= are both treated as distance<=.
** The query plan number is a bit vector:
**
**   bit 1:   Term of the form (A) found
**   bit 2:   Term like (B1) or (B2) found
**   bit 3:   Term like (C) found
**
** If bit-1 is set, $str is always in filter.argv[0].  If bit-2 is set
** then $value is in filter.argv[0] if bit-1 is clear and is in 
** filter.argv[1] if bit-1 is set.  If bit-3 is set, then $ruleid is
** in filter.argv[0] if bit-1 and bit-2 are both zero, is in
** filter.argv[1] if exactly one of bit-1 and bit-2 are set, and is in
** filter.argv[2] if both bit-1 and bit-2 are set.
*/
static int __fuzzer_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  int i_plan = 0;
  int i_dist_term = -1;
  int i_ruleset_term = -1;
  int i;
  int seen_match = 0;
  const struct dbsql_index_constraint_t *constraint;
  double r_cost = 1e12;

  constraint = idx_info->constraints;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    if( constraint->i_column==0
     && constraint->op==DBSQL_INDEX_CONSTRAINT_MATCH ){
      seen_match = 1;
    }
    if( constraint->usable==0 ) continue;
    if( (i_plan & 1)==0 
     && constraint->i_column==0
     && constraint->op==DBSQL_INDEX_CONSTRAINT_MATCH
    ){
      i_plan |= 1;
      idx_info->constraint_usages[i].argv_index = 1;
      idx_info->constraint_usages[i].omit = 1;
      r_cost /= 1e6;
    }
    if( (i_plan & 2)==0
     && constraint->i_column==1
     && (constraint->op==DBSQL_INDEX_CONSTRAINT_LT
           || constraint->op==DBSQL_INDEX_CONSTRAINT_LE)
    ){
      i_plan |= 2;
      i_dist_term = i;
      r_cost /= 10.0;
    }
    if( (i_plan & 4)==0
     && constraint->i_column==2
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= 4;
      idx_info->constraint_usages[i].omit = 1;
      i_ruleset_term = i;
      r_cost /= 10.0;
    }
  }
  if( i_plan & 2 ){
    idx_info->constraint_usages[i_dist_term].argv_index = 1+((i_plan&1)!=0);
  }
  if( i_plan & 4 ){
    int idx = 1;
    if( i_plan & 1 ) idx++;
    if( i_plan & 2 ) idx++;
    idx_info->constraint_usages[i_ruleset_term].argv_index = idx;
  }
  idx_info->idx_num = i_plan;
  if( idx_info->n_order_by==1
   && idx_info->order_bies[0].i_column==1
   && idx_info->order_bies[0].desc==0
  ){
    idx_info->order_by_consumed = 1;
  }
  if( seen_match && (i_plan&1)==0 ) r_cost = 1e99;
  idx_info->estimated_cost = r_cost;
   
  return DBSQL_OK;
}

/*
** A virtual table module that implements the "fuzzer".
*/
static dbsql_module_t __fuzzer_module = {
  0,                           /* i_version */
  __fuzzer_connect,
  __fuzzer_connect,
  __fuzzer_best_index,
  __fuzzer_disconnect, 
  __fuzzer_disconnect,
  __fuzzer_open,                  /* x_open - open a cursor */
  __fuzzer_close,                 /* x_close - close a cursor */
  __fuzzer_filter,                /* filter - configure scan constraints */
  __fuzzer_next,                  /* next - advance a cursor */
  __fuzzer_eof,                   /* eof - check for end of scan */
  __fuzzer_column,                /* column - read data */
  __fuzzer_rowid,                 /* rowid - read data */
  0,                           /* update */
  0,                           /* begin */
  0,                           /* x_sync */
  0,                           /* commit */
  0,                           /* rollback */
  0,                           /* find_method */
  0,                           /* rename */
  0,                           /* savepoint */
  0,                           /* release */
  0,                           /* rollback_to */
  0,                           /* shadow_name */
  0                            /* integrity */
};

#endif /* DBSQL_OMIT_VIRTUALTABLE */


#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_fuzzer_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  rc = dbsql_create_module(db, "fuzzer", &__fuzzer_module, 0);
#endif
  return rc;
}
