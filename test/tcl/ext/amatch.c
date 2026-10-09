/*
** 2013-03-14
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
** This file contains code for a demonstration virtual table that finds
** "approximate matches" - strings from a finite set that are nearly the
** same as a single input string.  The virtual table is called "amatch".
**
** A amatch virtual table is created like this:
**
**     CREATE VIRTUAL TABLE f USING approximate_match(
**        vocabulary_table=<tablename>,      -- V
**        vocabulary_word=<columnname>,      -- W
**        vocabulary_language=<columnname>,  -- L
**        edit_distances=<edit-cost-table>
**     );
**
** When it is created, the new amatch table must be supplied with the
** the name of a table V and columns V.W and V.L such that 
**
**     SELECT W FROM V WHERE L=$language
**
** returns the allowed vocabulary for the match.  If the "vocabulary_language"
** or L columnname is left unspecified or is an empty string, then no
** filtering of the vocabulary by language is performed. 
**
** For efficiency, it is essential that the vocabulary table be indexed:
**
**     CREATE vocab_index ON V(W)
**
** A separate edit-cost-table provides scoring information that defines 
** what it means for one string to be "close" to another.
**
** The edit-cost-table must contain exactly four columns (more precisely,
** the statement "SELECT * FROM <edit-cost-table>" must return records
** that consist of four columns). It does not matter what the columns are
** named. 
**
** Each row in the edit-cost-table represents a single character
** transformation going from user input to the vocabulary. The leftmost 
** column of the row (column 0) contains an integer identifier of the
** language to which the transformation rule belongs (see "MULTIPLE LANGUAGES"
** below). The second column of the row (column 1) contains the input
** character or characters - the characters of user input. The third 
** column contains characters as they appear in the vocabulary table.
** And the fourth column contains the integer cost of making the
** transformation. For example:
**
**    CREATE TABLE f_data(i_lang, c_from, c_to, Cost);
**    INSERT INTO f_data(i_lang, c_from, c_to, Cost) VALUES(0, '', 'a', 100);
**    INSERT INTO f_data(i_lang, c_from, c_to, Cost) VALUES(0, 'b', '', 87);
**    INSERT INTO f_data(i_lang, c_from, c_to, Cost) VALUES(0, 'o', 'oe', 38);
**    INSERT INTO f_data(i_lang, c_from, c_to, Cost) VALUES(0, 'oe', 'o', 40);
**
** The first row inserted into the edit-cost-table by the SQL script
** above indicates that the cost of having an extra 'a' in the vocabulary
** table that is missing in the user input 100.  (All costs are integers.
** Overall cost must not exceed 16777216.)  The second INSERT statement 
** creates a rule saying that the cost of having a single letter 'b' in
** user input which is missing in the vocabulary table is 87.  The third
** INSERT statement mean that the cost of matching an 'o' in user input 
** against an 'oe' in the vocabulary table is 38.  And so forth.
**
** The following rules are special:
**
**    INSERT INTO f_data(i_lang, c_from, c_to, Cost) VALUES(0, '?', '', 97);
**    INSERT INTO f_data(i_lang, c_from, c_to, Cost) VALUES(0, '', '?', 98);
**    INSERT INTO f_data(i_lang, c_from, c_to, Cost) VALUES(0, '?', '?', 99);
**
** The '?' to '' rule is the cost of having any single character in the input
** that is not found in the vocabular.  The '' to '?' rule is the cost of
** having a character in the vocabulary table that is missing from input.
** And the '?' to '?' rule is the cost of doing an arbitrary character
** substitution.  These three generic rules apply across all languages.
** In other words, the i_lang field is ignored for the generic substitution
** rules.  If more than one cost is given for a generic substitution rule,
** then the lowest cost is used.
**
** Once it has been created, the amatch virtual table can be queried
** as follows:
**
**    SELECT word, distance FROM f
**     WHERE word MATCH 'abcdefg'
**       AND distance<200;
**
** This query outputs the strings contained in the T(F) field that
** are close to "abcdefg" and in order of increasing distance.  No string
** is output more than once.  If there are multiple ways to transform the
** target string ("abcdefg") into a string in the vocabulary table then
** the lowest cost transform is the one that is returned.  In this example,
** the search is limited to strings with a total distance of less than 200.
**
** For efficiency, it is important to put tight bounds on the distance.
** The time and memory space needed to perform this query is exponential
** in the maximum distance.  A good rule of thumb is to limit the distance
** to no more than 1.5 or 2 times the maximum cost of any rule in the
** edit-cost-table.
**
** The amatch is a read-only table.  Any attempt to DELETE, INSERT, or
** UPDATE on a amatch table will throw an error.
**
** It is important to put some kind of a limit on the amatch output.  This
** can be either in the form of a LIMIT clause at the end of the query,
** or better, a "distance<NNN" constraint where NNN is some number.  The
** running time and memory requirement is exponential in the value of NNN 
** so you want to make sure that NNN is not too big.  A value of NNN that
** is about twice the average transformation cost seems to give good results.
**
** The amatch table can be useful for tasks such as spelling correction.
** Suppose all allowed words are in table vocabulary(w).  Then one would create
** an amatch virtual table like this:
**
**   CREATE VIRTUAL TABLE ex1 USING amatch(
**       vocabtable=vocabulary,
**       vocabcolumn=w,
**       edit_distances=ec1
**   );
**
** Then given an input word $word, look up close spellings this way:
**
**   SELECT word, distance FROM ex1
**    WHERE word MATCH $word AND distance<200;
**
** MULTIPLE LANGUAGES
**
** Normally, the "i_lang" value associated with all character transformations
** in the edit-cost-table is zero. However, if required, the amatch 
** virtual table allows multiple languages to be defined. Each query uses 
** only a single i_lang value.   This allows, for example, a single 
** amatch table to support multiple languages.
**
** By default, only the rules with i_lang=0 are used. To specify an 
** alternative language, a "language = ?" expression must be added to the
** WHERE clause of a SELECT, where ? is the integer identifier of the desired 
** language. For example:
**
**   SELECT word, distance FROM ex1
**    WHERE word MATCH $word
**      AND distance<=200
**      AND language=1 -- Specify use language 1 instead of 0
**
** If no "language = ?" constraint is specified in the WHERE clause, language
** 0 is used.
**
** LIMITS
**
** The maximum language number is 2147483647.  The maximum length of either
** of the strings in the second or third column of the amatch data table
** is 50 bytes.  The maximum cost on a rule is 1000.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <ctype.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** Forward declaration of objects used by this implementation
*/
typedef struct amatch_vtab amatch_vtab;
typedef struct amatch_cursor amatch_cursor;
typedef struct amatch_rule amatch_rule;
typedef struct amatch_word amatch_word;
typedef struct amatch_avl amatch_avl;


/*****************************************************************************
** AVL Tree implementation
*/
/*
** Objects that want to be members of the AVL tree should embedded an
** instance of this structure.
*/
struct amatch_avl {
  amatch_word *word;   /* Points to the object being stored in the tree */
  char *key;           /* Key.  zero-terminated string.  Must be unique */
  amatch_avl *before;  /* Other elements less than key */
  amatch_avl *after;   /* Other elements greater than key */
  amatch_avl *up;      /* Parent element */
  int height;           /* Height of this node.  Leaf==1 */
  int imbalance;        /* Height difference between before and after */
};

/* Recompute the amatch_avl.height and amatch_avl.imbalance fields for p.
** Assume that the children of p have correct heights.
*/
static void __amatch_avl_recompute_height(amatch_avl *p){
  int h_before = p->before ? p->before->height : 0;
  int h_after = p->after ? p->after->height : 0;
  p->imbalance = h_before - h_after;  /* -: after higher.  +: before higher */
  p->height = (h_before>h_after ? h_before : h_after)+1;
}

/*
**     P                B
**    / \              / \
**   B   Z    ==>     X   P
**  / \                  / \
** X   Y                Y   Z
**
*/
static amatch_avl *__amatch_avl_rotate_before(amatch_avl *p){
  amatch_avl *b = p->before;
  amatch_avl *y = b->after;
  b->up = p->up;
  b->after = p;
  p->up = b;
  p->before = y;
  if( y ) y->up = p;
  __amatch_avl_recompute_height(p);
  __amatch_avl_recompute_height(b);
  return b;
}

/*
**     P                A
**    / \              / \
**   X   A    ==>     P   Z
**      / \          / \
**     Y   Z        X   Y
**
*/
static amatch_avl *__amatch_avl_rotate_after(amatch_avl *p){
  amatch_avl *a = p->after;
  amatch_avl *y = a->before;
  a->up = p->up;
  a->before = p;
  p->up = a;
  p->after = y;
  if( y ) y->up = p;
  __amatch_avl_recompute_height(p);
  __amatch_avl_recompute_height(a);
  return a;
}

/*
** Return a pointer to the before or after pointer in the parent
** of p that points to p.  Or if p is the root node, return pp.
*/
static amatch_avl **__amatch_avl_from_ptr(amatch_avl *p, amatch_avl **pp){
  amatch_avl *up = p->up;
  if( up==0 ) return pp;
  if( up->after==p ) return &up->after;
  return &up->before;
}

/*
** Rebalance all nodes starting with p and working up to the root.
** Return the new root.
*/
static amatch_avl *__amatch_avl_balance(amatch_avl *p){
  amatch_avl *top = p;
  amatch_avl **pp;
  while( p ){
    __amatch_avl_recompute_height(p);
    if( p->imbalance>=2 ){
      amatch_avl *b = p->before;
      if( b->imbalance<0 ) p->before = __amatch_avl_rotate_after(b);
      pp = __amatch_avl_from_ptr(p,&p);
      p = *pp = __amatch_avl_rotate_before(p);
    }else if( p->imbalance<=(-2) ){
      amatch_avl *a = p->after;
      if( a->imbalance>0 ) p->after = __amatch_avl_rotate_before(a);
      pp = __amatch_avl_from_ptr(p,&p);
      p = *pp = __amatch_avl_rotate_after(p);
    }
    top = p;
    p = p->up;
  }
  return top;
}

/* Search the tree rooted at p for an entry with key.  Return a pointer
** to the entry or return NULL.
*/
static amatch_avl *__amatch_avl_search(amatch_avl *p, const char *key){
  int c;
  while( p && (c = strcmp(key, p->key))!=0 ){
    p = (c<0) ? p->before : p->after;
  }
  return p;
}

/* Find the first node (the one with the smallest key).
*/
static amatch_avl *__amatch_avl_first(amatch_avl *p){
  if( p ) while( p->before ) p = p->before;
  return p;
}

#if 0 /* NOT USED */
/* Return the node with the next larger key after p.
*/
static amatch_avl *__amatch_avl_next(amatch_avl *p){
  amatch_avl *prev = 0;
  while( p && p->after==prev ){
    prev = p;
    p = p->up;
  }
  if( p && prev==0 ){
    p = __amatch_avl_first(p->after);
  }
  return p;
}
#endif

#if 0 /* NOT USED */
/* Verify AVL tree integrity
*/
static int __amatch_avl_integrity(amatch_avl *head){
  amatch_avl *p;
  if( head==0 ) return 1;
  if( (p = head->before)!=0 ){
    assert( p->up==head );
    assert( __amatch_avl_integrity(p) );
    assert( strcmp(p->key, head->key)<0 );
    while( p->after ) p = p->after;
    assert( strcmp(p->key, head->key)<0 );
  }
  if( (p = head->after)!=0 ){
    assert( p->up==head );
    assert( __amatch_avl_integrity(p) );
    assert( strcmp(p->key, head->key)>0 );
    p = __amatch_avl_first(p);
    assert( strcmp(p->key, head->key)>0 );
  }
  return 1;
}
static int __amatch_avl_integrity2(amatch_avl *head){
  amatch_avl *p, *next;
  for(p=__amatch_avl_first(head); p; p=next){
    next = __amatch_avl_next(p);
    if( next==0 ) break;
    assert( strcmp(p->key, next->key)<0 );
  }
  return 1;
}
#endif

/* Insert a new node p_new.  Return NULL on success.  If the key is not
** unique, then do not perform the insert but instead leave p_new unchanged
** and return a pointer to an existing node with the same key.
*/
static amatch_avl *__amatch_avl_insert(amatch_avl **head, amatch_avl *p_new){
  int c;
  amatch_avl *p = *head;
  if( p==0 ){
    p = p_new;
    p_new->up = 0;
  }else{
    while( p ){
      c = strcmp(p_new->key, p->key);
      if( c<0 ){
        if( p->before ){
          p = p->before;
        }else{
          p->before = p_new;
          p_new->up = p;
          break;
        }
      }else if( c>0 ){
        if( p->after ){
          p = p->after;
        }else{
          p->after = p_new;
          p_new->up = p;
          break;
        }
      }else{
        return p;
      }
    }
  }
  p_new->before = 0;
  p_new->after = 0;
  p_new->height = 1;
  p_new->imbalance = 0;
  *head = __amatch_avl_balance(p);
  /* assert( __amatch_avl_integrity(*head) ); */
  /* assert( __amatch_avl_integrity2(*head) ); */
  return 0;
}

/* Remove node old from the tree.  old must be an element of the tree or
** the AVL tree will become corrupt.
*/
static void __amatch_avl_remove(amatch_avl **head, amatch_avl *old){
  amatch_avl **parent;
  amatch_avl *balance = 0;
  /* assert( __amatch_avl_search(*head, old->key)==old ); */
  parent = __amatch_avl_from_ptr(old, head);
  if( old->before==0 && old->after==0 ){
    *parent = 0;
    balance = old->up;
  }else if( old->before && old->after ){
    amatch_avl *x, *y;
    x = __amatch_avl_first(old->after);
    *__amatch_avl_from_ptr(x, 0) = x->after;
    if( x->after ) x->after->up = x->up;
    balance = x->up;
    x->after = old->after;
    if( x->after ){
      x->after->up = x;
    }else{
      assert( balance==old );
      balance = x;
    }
    x->before = y = old->before;
    if( y ) y->up = x;
    x->up = old->up;
    *parent = x;
  }else if( old->before==0 ){
    *parent = balance = old->after;
    balance->up = old->up;
  }else if( old->after==0 ){
    *parent = balance = old->before;
    balance->up = old->up;
  }
  *head = __amatch_avl_balance(balance);
  old->up = 0;
  old->before = 0;
  old->after = 0;
  /* assert( __amatch_avl_integrity(*head) ); */
  /* assert( __amatch_avl_integrity2(*head) ); */
}
/*
** End of the AVL Tree implementation
******************************************************************************/


/*
** Various types.
**
** amatch_cost is the "cost" of an edit operation.
**
** amatch_len is the length of a matching string.  
**
** amatch_langid is an ruleset identifier.
*/
typedef int amatch_cost;
typedef signed char amatch_len;
typedef int amatch_langid;

/*
** Limits
*/
#define AMATCH_MX_LENGTH          50  /* Maximum length of a rule string */
#define AMATCH_MX_LANGID  2147483647  /* Maximum rule ID */
#define AMATCH_MX_COST          1000  /* Maximum single-rule cost */
#define AMATCH_MX_WORD          1000  /* Maximum length of a word */


/*
** A match or partial match
*/
struct amatch_word {
  amatch_word *next;   /* Next on a list of all amatch_words */
  amatch_avl s_cost;     /* Linkage of this node into the cost tree */
  amatch_avl s_word;     /* Linkage of this node into the word tree */
  amatch_cost r_cost;    /* Cost of the match so far */
  int i_seq;             /* Sequence number */
  int n_match;           /* Input characters matched */
  char cost[10];       /* Cost key (text rendering of r_cost) */
  char word[4];        /* Text of the word.  Extra space appended as needed */
};

/*
** Each transformation rule is stored as an instance of this object.
** All rules are kept on a linked list sorted by r_cost.
*/
struct amatch_rule {
  amatch_rule *next;      /* Next rule in order of increasing r_cost */
  char *from;             /* Transform from (a string from user input) */
  amatch_cost r_cost;       /* Cost of this transformation */
  amatch_langid i_lang;     /* The language to which this rule belongs */
  amatch_len n_from, n_to;   /* Length of the from and to strings */
  char to[4];             /* Transform to V.W value (extra space appended) */
};

/* 
** A amatch virtual-table object 
*/
struct amatch_vtab {
  dbsql_vtab_t base;         /* Base class - must be first */
  char *class_name;          /* Name of this class.  Default: "amatch" */
  char *z_db;                 /* Name of database.  (ex: "main") */
  char *self;               /* Name of this virtual table */
  char *cost_tab;            /* Name of edit-cost-table */
  char *vocab_tab;           /* Name of vocabulary table */
  char *vocab_word;          /* Name of vocabulary table word column */
  char *vocab_lang;          /* Name of vocabulary table language column */
  amatch_rule *rule;        /* All active rules in this amatch */
  amatch_cost r_ins;          /* Generic insertion cost  '' -> ? */
  amatch_cost r_del;          /* Generic deletion cost  ? -> '' */
  amatch_cost r_sub;          /* Generic substitution cost ? -> ? */
  DBSQL *db;               /* The database connection */
  dbsql_stmt_t *v_check;     /* Query to check vocab_tab */
  int n_cursor;               /* Number of active cursors */
};

/* A amatch cursor object */
struct amatch_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  int64_t i_rowid;      /* The rowid of the current word */
  amatch_langid i_lang;       /* Use this language ID */
  amatch_cost r_limit;        /* Maximum cost of any term */
  int64_t n_buf;        /* Space allocated for buf */
  int oom_err;                /* True following an OOM error */
  int n_word;                 /* Number of amatch_word objects */
  char *buf;                /* Temp-use buffer space */
  char *input;              /* Input word to match against */
  amatch_vtab *vtab;        /* The virtual table this cursor belongs to */
  amatch_word *all_words;    /* List of all amatch_word objects */
  amatch_word *current;     /* Most recent solution */
  amatch_avl *cost;         /* amatch_word objects keyed by i_cost */
  amatch_avl *word;         /* amatch_word objects keyed by word */
};

/*
** The two input rule lists are both sorted in order of increasing
** cost.  Merge them together into a single list, sorted by cost, and
** return a pointer to the head of that list.
*/
static amatch_rule *__amatch_merge_rules(amatch_rule *a, amatch_rule *b){
  amatch_rule head;
  amatch_rule *tail;

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
** Statement stmt currently points to a row in the amatch data table. This
** function allocates and populates a amatch_rule structure according to
** the content of the row.
**
** If successful, *rule is set to point to the new object and DBSQL_OK
** is returned. Otherwise, *rule is zeroed, *err_p may be set to point
** to an error message and an dbsql error code returned.
*/
static int __amatch_load_one_rule(
  amatch_vtab *p,                 /* Fuzzer virtual table handle */
  dbsql_stmt_t *stmt,            /* Base rule on statements current row */
  amatch_rule **pp_rule,           /* OUT: New rule object */
  char **err_p                    /* OUT: Error message */
){
  int64_t i_lang = dbsql_column_int64(stmt, 0);
  const char *from = (const char *)dbsql_column_text(stmt, 1);
  const char *to = (const char *)dbsql_column_text(stmt, 2);
  amatch_cost r_cost = dbsql_column_int(stmt, 3);

  int rc = DBSQL_OK;             /* Return code */
  int n_from;                      /* Size of string from, in bytes */
  int n_to;                        /* Size of string to, in bytes */
  amatch_rule *rule = 0;         /* New rule object to return */

  if( from==0 ) from = "";
  if( to==0 ) to = "";
  n_from = (int)strlen(from);
  n_to = (int)strlen(to);

  /* Silently ignore null transformations */
  if( strcmp(from, to)==0 ){
    if( from[0]=='?' && from[1]==0 ){
      if( p->r_sub==0 || p->r_sub>r_cost ) p->r_sub = r_cost;
    }
    *pp_rule = 0;
    return DBSQL_OK;
  }

  if( r_cost<=0 || r_cost>AMATCH_MX_COST ){
    *err_p = dbsql_mprintf("%s: cost must be between 1 and %d", 
        p->class_name, AMATCH_MX_COST
    );
    rc = DBSQL_ERROR;
  }else
  if( n_from>AMATCH_MX_LENGTH || n_to>AMATCH_MX_LENGTH ){
    *err_p = dbsql_mprintf("%s: maximum string length is %d", 
        p->class_name, AMATCH_MX_LENGTH
    );
    rc = DBSQL_ERROR;    
  }else
  if( i_lang<0 || i_lang>AMATCH_MX_LANGID ){
    *err_p = dbsql_mprintf("%s: iLang must be between 0 and %d", 
        p->class_name, AMATCH_MX_LANGID
    );
    rc = DBSQL_ERROR;    
  }else
  if( strcmp(from,"")==0 && strcmp(to,"?")==0 ){
    if( p->r_ins==0 || p->r_ins>r_cost ) p->r_ins = r_cost;
  }else
  if( strcmp(from,"?")==0 && strcmp(to,"")==0 ){
    if( p->r_del==0 || p->r_del>r_cost ) p->r_del = r_cost;
  }else
  {
    rule = dbsql_malloc64( sizeof(*rule) + n_from + n_to );
    if( rule==0 ){
      rc = DBSQL_NOMEM;
    }else{
      memset(rule, 0, sizeof(*rule));
      rule->from = &rule->to[n_to+1];
      rule->n_from = (amatch_len)n_from;
      memcpy(rule->from, from, n_from+1);
      memcpy(rule->to, to, n_to+1);
      rule->n_to = (amatch_len)n_to;
      rule->r_cost = r_cost;
      rule->i_lang = (int)i_lang;
    }
  }

  *pp_rule = rule;
  return rc;
}

/*
** Free all the content in the edit-cost-table
*/
static void __amatch_free_rules(amatch_vtab *p){
  while( p->rule ){
    amatch_rule *rule = p->rule;
    p->rule = rule->next;
    dbsql_free(rule);
  }
  p->rule = 0;
}

/*
** Load the content of the amatch data table into memory.
*/
static int __amatch_load_rules(
  DBSQL *db,                    /* Database handle */
  amatch_vtab *p,                 /* Virtual amatch table to configure */
  char **err_p                    /* OUT: Error message */
){
  int rc = DBSQL_OK;             /* Return code */
  char *sql;                     /* SELECT used to read from rules table */
  amatch_rule *head = 0;

  sql = dbsql_mprintf("SELECT * FROM %Q.%Q", p->z_db, p->cost_tab);
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
          p->class_name, p->cost_tab, dbsql_column_count(stmt)
      );
      rc = DBSQL_ERROR;
    }else{
      while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(stmt) ){
        amatch_rule *rule = 0;
        rc = __amatch_load_one_rule(p, stmt, &rule, err_p);
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
  ** block sorts them by cost and then sets amatch_vtab.rule to point to 
  ** point to the head of the sorted list.
  */
  if( rc==DBSQL_OK ){
    unsigned int i;
    amatch_rule *x;
    amatch_rule *a[15];
    for(i=0; i<sizeof(a)/sizeof(a[0]); i++) a[i] = 0;
    while( (x = head)!=0 ){
      head = x->next;
      x->next = 0;
      for(i=0; a[i] && i<sizeof(a)/sizeof(a[0])-1; i++){
        x = __amatch_merge_rules(a[i], x);
        a[i] = 0;
      }
      a[i] = __amatch_merge_rules(a[i], x);
    }
    for(x=a[0], i=1; i<sizeof(a)/sizeof(a[0]); i++){
      x = __amatch_merge_rules(a[i], x);
    }
    p->rule = __amatch_merge_rules(p->rule, x);
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
static char *__amatch_dequote(const char *in){
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
    }
    assert( (int)strlen(out)<=n_in );
  }
  return out;
}

/*
** Deallocate the v_check prepared statement.
*/
static void __amatch_v_check_clear(amatch_vtab *p){
  if( p->v_check ){
    dbsql_finalize(p->v_check);
    p->v_check = 0;
  }
}

/*
** Deallocate an amatch_vtab object
*/
static void __amatch_free(amatch_vtab *p){
  if( p ){
    __amatch_free_rules(p);
    __amatch_v_check_clear(p);
    dbsql_free(p->class_name);
    dbsql_free(p->z_db);
    dbsql_free(p->cost_tab);
    dbsql_free(p->vocab_tab);
    dbsql_free(p->vocab_word);
    dbsql_free(p->vocab_lang);
    dbsql_free(p->self);
    memset(p, 0, sizeof(*p));
    dbsql_free(p);
  }
}

/*
** disconnect/destroy method for the amatch module.
*/
static int __amatch_disconnect(dbsql_vtab_t *vtab){
  amatch_vtab *p = (amatch_vtab*)vtab;
  assert( p->n_cursor==0 );
  __amatch_free(p);
  return DBSQL_OK;
}

/*
** Check to see if the argument is of the form:
**
**       KEY = VALUE
**
** If it is, return a pointer to the first character of VALUE.
** If not, return NULL.  Spaces around the = are ignored.
*/
static const char *__amatch_value_of_key(const char *key, const char *str){
  int n_key = (int)strlen(key);
  int n_str = (int)strlen(str);
  int i;
  if( n_str<n_key+1 ) return 0;
  if( memcmp(str, key, n_key)!=0 ) return 0;
  for(i=n_key; isspace((unsigned char)str[i]); i++){}
  if( str[i]!='=' ) return 0;
  i++;
  while( isspace((unsigned char)str[i]) ){ i++; }
  return str+i;
}

/*
** connect/create method for the amatch module. Arguments are:
**
**   argv[0]    -> module name  ("approximate_match")
**   argv[1]    -> database name
**   argv[2]    -> table name
**   argv[3...] -> arguments
*/
static int __amatch_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  int rc = DBSQL_OK;             /* Return code */
  amatch_vtab *p_new = 0;          /* New virtual table */
  const char *module = argv[0];
  const char *z_db = argv[1];
  const char *val;
  int i;

  (void)aux;
  *vtab = 0;
  p_new = dbsql_malloc64( sizeof(*p_new) );
  if( p_new==0 ) return DBSQL_NOMEM;
  rc = DBSQL_NOMEM;
  memset(p_new, 0, sizeof(*p_new));
  p_new->db = db;
  p_new->class_name = dbsql_mprintf("%s", module);
  if( p_new->class_name==0 ) goto amatch_connect_error;
  p_new->z_db = dbsql_mprintf("%s", z_db);
  if( p_new->z_db==0 ) goto amatch_connect_error;
  p_new->self = dbsql_mprintf("%s", argv[2]);
  if( p_new->self==0 ) goto amatch_connect_error;
  for(i=3; i<argc; i++){
    val = __amatch_value_of_key("vocabulary_table", argv[i]);
    if( val ){
      dbsql_free(p_new->vocab_tab);
      p_new->vocab_tab = __amatch_dequote(val);
      if( p_new->vocab_tab==0 ) goto amatch_connect_error;
      continue;
    }
    val = __amatch_value_of_key("vocabulary_word", argv[i]);
    if( val ){
      dbsql_free(p_new->vocab_word);
      p_new->vocab_word = __amatch_dequote(val);
      if( p_new->vocab_word==0 ) goto amatch_connect_error;
      continue;
    }
    val = __amatch_value_of_key("vocabulary_language", argv[i]);
    if( val ){
      dbsql_free(p_new->vocab_lang);
      p_new->vocab_lang = __amatch_dequote(val);
      if( p_new->vocab_lang==0 ) goto amatch_connect_error;
      continue;
    }
    val = __amatch_value_of_key("edit_distances", argv[i]);
    if( val ){
      dbsql_free(p_new->cost_tab);
      p_new->cost_tab = __amatch_dequote(val);
      if( p_new->cost_tab==0 ) goto amatch_connect_error;
      continue;
    }
    *err_p = dbsql_mprintf("unrecognized argument: [%s]\n", argv[i]);
    __amatch_free(p_new);
    *vtab = 0;
    return DBSQL_ERROR;
  }
  rc = DBSQL_OK;
  if( p_new->cost_tab==0 ){
    *err_p = dbsql_mprintf("no edit_distances table specified");
    rc = DBSQL_ERROR;
  }else{
    rc = __amatch_load_rules(db, p_new, err_p);
  }
  if( rc==DBSQL_OK ){
    dbsql_vtab_config(db, DBSQL_VTAB_INNOCUOUS);
    rc = dbsql_declare_vtab(db,
           "CREATE TABLE x(word,distance,language,"
           "command HIDDEN,nword HIDDEN)"
         );
#define AMATCH_COL_WORD       0
#define AMATCH_COL_DISTANCE   1
#define AMATCH_COL_LANGUAGE   2
#define AMATCH_COL_COMMAND    3
#define AMATCH_COL_NWORD      4
  }
  if( rc!=DBSQL_OK ){
    __amatch_free(p_new);
  }
  *vtab = &p_new->base;
  return rc;

amatch_connect_error:
  __amatch_free(p_new);
  return rc;
}

/*
** Open a new amatch cursor.
*/
static int __amatch_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  amatch_vtab *p = (amatch_vtab*)v_tab;
  amatch_cursor *cur;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  cur->vtab = p;
  *cursor = &cur->base;
  p->n_cursor++;
  return DBSQL_OK;
}

/*
** Free up all the memory allocated by a cursor.  Set it r_limit to 0
** to indicate that it is at EOF.
*/
static void __amatch_clear_cursor(amatch_cursor *cur){
  amatch_word *word, *next_word;
  for(word=cur->all_words; word; word=next_word){
    next_word = word->next;
    dbsql_free(word);
  }
  cur->all_words = 0;
  dbsql_free(cur->input);
  cur->input = 0;
  dbsql_free(cur->buf);
  cur->buf = 0;
  cur->n_buf = 0;
  cur->cost = 0;
  cur->word = 0;
  cur->current = 0;
  cur->r_limit = 1000000;
  cur->i_lang = 0;
  cur->n_word = 0;
}

/*
** Close a amatch cursor.
*/
static int __amatch_close(dbsql_vtab_cursor_t *cur){
  amatch_cursor *p_cur = (amatch_cursor *)cur;
  __amatch_clear_cursor(p_cur);
  p_cur->vtab->n_cursor--;
  dbsql_free(p_cur);
  return DBSQL_OK;
}

/*
** Render a 24-bit unsigned integer as a 4-byte base-64 number.
*/
static void __amatch_encode_int(int x, char *z){
  static const char a[] = 
    "0123456789"
    "ABCDEFGHIJ"
    "KLMNOPQRST"
    "UVWXYZ^abc"
    "defghijklm"
    "nopqrstuvw"
    "xyz~";
  z[0] = a[(x>>18)&0x3f];
  z[1] = a[(x>>12)&0x3f];
  z[2] = a[(x>>6)&0x3f];
  z[3] = a[x&0x3f];
}

/*
** Write the cost[] field for a amatch_word object
*/
static void __amatch_write_cost(amatch_word *word){
  __amatch_encode_int(word->r_cost, word->cost);
  __amatch_encode_int(word->i_seq, word->cost+4);
  word->cost[8] = 0;
}

/* Circumvent compiler warnings about the use of strcpy() by supplying
** our own implementation.
*/
static void __amatch_strcpy(char *dest, const char *src){
  while( (*(dest++) = *(src++))!=0 ){}
}
static void __amatch_strcat(char *dest, const char *src){
  while( *dest ) dest++;
  __amatch_strcpy(dest, src);
}

/*
** Add a new amatch_word object to the queue.
**
** If a prior amatch_word object with the same word, and n_match
** already exists, update its r_cost (if the new r_cost is less) but
** otherwise leave it unchanged.  Do not add a duplicate.
**
** Do nothing if the cost exceeds threshold.
*/
static void __amatch_add_word(
  amatch_cursor *cur,
  amatch_cost r_cost,
  int n_match,
  const char *word_base,
  const char *word_tail
){
  amatch_word *word;
  amatch_avl *node;
  amatch_avl *other;
  int n_base, n_tail;
  char buf[4];
  
  if( r_cost>cur->r_limit ){
    return;
  }
  n_base = (int)strlen(word_base);
  n_tail = (int)strlen(word_tail);
  if( n_base+n_tail+3>cur->n_buf ){
    cur->n_buf = n_base+n_tail+100;
    cur->buf = dbsql_realloc64(cur->buf, cur->n_buf);
    if( cur->buf==0 ){
      cur->n_buf = 0;
      return;
    }
  }
  __amatch_encode_int(n_match, buf);
  memcpy(cur->buf, buf+2, 2);
  memcpy(cur->buf+2, word_base, n_base);
  memcpy(cur->buf+2+n_base, word_tail, n_tail+1);
  node = __amatch_avl_search(cur->word, cur->buf);
  if( node ){
    word = node->word;
    if( word->r_cost>r_cost ){
#ifdef AMATCH_TRACE_1
      printf("UPDATE [%s][%.*s^%s] %d (\"%s\" \"%s\")\n",
             word->word+2, word->n_match, cur->input, cur->input,
             word->r_cost, word->word, word->cost);
#endif
      __amatch_avl_remove(&cur->cost, &word->s_cost);
      word->r_cost = r_cost;
      __amatch_write_cost(word);
#ifdef AMATCH_TRACE_1
      printf("  ---> %d (\"%s\" \"%s\")\n",
             word->r_cost, word->word, word->cost);
#endif
      other = __amatch_avl_insert(&cur->cost, &word->s_cost);
      assert( other==0 ); (void)other;
    }
    return;
  }
  word = dbsql_malloc64( sizeof(*word) + n_base + n_tail - 1 );
  if( word==0 ) return;
  memset(word, 0, sizeof(*word));
  word->r_cost = r_cost;
  word->i_seq = cur->n_word++;
  __amatch_write_cost(word);
  word->n_match = n_match;
  word->next = cur->all_words;
  cur->all_words = word;
  word->s_cost.key = word->cost;
  word->s_cost.word = word;
  other = __amatch_avl_insert(&cur->cost, &word->s_cost);
  assert( other==0 ); (void)other;
  word->s_word.key = word->word;
  word->s_word.word = word;
  __amatch_strcpy(word->word, cur->buf);
  other = __amatch_avl_insert(&cur->word, &word->s_word);
  assert( other==0 ); (void)other;
#ifdef AMATCH_TRACE_1
  printf("INSERT [%s][%.*s^%s] %d (\"%s\" \"%s\")\n", word->word+2,
       word->n_match, cur->input, cur->input+word->n_match, r_cost,
       word->word, word->cost);
#endif
}


/*
** Advance a cursor to its next row of output
*/
static int __amatch_next(dbsql_vtab_cursor_t *cur){
  amatch_cursor *p_cur = (amatch_cursor*)cur;
  amatch_word *word = 0;
  amatch_avl *node;
  int isMatch = 0;
  amatch_vtab *p = p_cur->vtab;
  int64_t n_word;
  int rc;
  int i;
  const char *w;
  amatch_rule *rule;
  char *buf = 0;
  int64_t n_buf = 0;
  char next[8];
  char next_in[8];
  int n_next_in;

  if( p->v_check==0 ){
    char *sql;
    if( p->vocab_lang && p->vocab_lang[0] ){
      sql = dbsql_mprintf(
          "SELECT \"%w\" FROM \"%w\"",
          " WHERE \"%w\">=?1 AND \"%w\"=?2"
          " ORDER BY 1",
          p->vocab_word, p->vocab_tab,
          p->vocab_word, p->vocab_lang
      );
    }else{
      sql = dbsql_mprintf(
          "SELECT \"%w\" FROM \"%w\""
          " WHERE \"%w\">=?1"
          " ORDER BY 1",
          p->vocab_word, p->vocab_tab,
          p->vocab_word
      );
    }
    rc = dbsql_prepare_v2(p->db, sql, -1, &p->v_check, 0);
    dbsql_free(sql);
    if( rc ) return rc;
  }
  dbsql_bind_int(p->v_check, 2, p_cur->i_lang);

  do{
    node = __amatch_avl_first(p_cur->cost);
    if( node==0 ){
      word = 0;
      break;
    }
    word = node->word;
    __amatch_avl_remove(&p_cur->cost, &word->s_cost);

#ifdef AMATCH_TRACE_1
    printf("PROCESS [%s][%.*s^%s] %d (\"%s\" \"%s\")\n",
       word->word+2, word->n_match, p_cur->input, p_cur->input+word->n_match,
       word->r_cost, word->word, word->cost);
#endif
    n_word = (int)strlen(word->word+2);
    if( n_word+20>n_buf ){
      n_buf = (char)(n_word+100);
      buf = dbsql_realloc64(buf, n_buf);
      if( buf==0 ) return DBSQL_NOMEM;
    }
    __amatch_strcpy(buf, word->word+2);
    next[0] = 0;
    next_in[0] = p_cur->input[word->n_match];
    if( next_in[0] ){
      for(i=1; i<=4 && (p_cur->input[word->n_match+i]&0xc0)==0x80; i++){
        next_in[i] = p_cur->input[word->n_match+i];
      }
      next_in[i] = 0;
      n_next_in = i;
    }else{
      n_next_in = 0;
    }

    if( next_in[0] && next_in[0]!='*' ){
      dbsql_reset(p->v_check);
      __amatch_strcat(buf, next_in);
      dbsql_bind_text(p->v_check, 1, buf, n_word+n_next_in, DBSQL_STATIC);
      rc = dbsql_step(p->v_check);
      if( rc==DBSQL_ROW ){
        w = (const char*)dbsql_column_text(p->v_check, 0);
        if( strncmp(buf, w, n_word+n_next_in)==0 ){
          __amatch_add_word(p_cur, word->r_cost, word->n_match+n_next_in, buf, "");
        }
      }
      buf[n_word] = 0;
    }

    while( 1 ){
      __amatch_strcpy(buf+n_word, next);
      dbsql_reset(p->v_check);
      dbsql_bind_text(p->v_check, 1, buf, -1, DBSQL_TRANSIENT);
      rc = dbsql_step(p->v_check);
      if( rc!=DBSQL_ROW ) break;
      w = (const char*)dbsql_column_text(p->v_check, 0);
      __amatch_strcpy(buf+n_word, next);
      if( strncmp(w, buf, n_word)!=0 ) break;
      if( (next_in[0]=='*' && next_in[1]==0)
       || (next_in[0]==0 && w[n_word]==0)
      ){
        isMatch = 1;
        next_in[0] = 0;
        n_next_in = 0;
        break;
      }
      next[0] = w[n_word];
      for(i=1; i<=4 && (w[n_word+i]&0xc0)==0x80; i++){
        next[i] = w[n_word+i];
      }
      next[i] = 0;
      buf[n_word] = 0;
      if( p->r_ins>0 ){
        __amatch_add_word(p_cur, word->r_cost+p->r_ins, word->n_match, 
                      buf, next);
      }
      if( p->r_sub>0 ){
        __amatch_add_word(p_cur, word->r_cost+p->r_sub, word->n_match+n_next_in, 
                      buf, next);
      }
      if( p->r_ins<0 && p->r_sub<0 ) break;
      next[i-1]++;  /* FIX ME */
    }
    dbsql_reset(p->v_check);

    if( p->r_del>0 ){
      buf[n_word] = 0;
      __amatch_add_word(p_cur, word->r_cost+p->r_del, word->n_match+n_next_in,
                    buf, "");
    }

    for(rule=p->rule; rule; rule=rule->next){
      if( rule->i_lang!=p_cur->i_lang ) continue;
      if( strncmp(rule->from, p_cur->input+word->n_match, rule->n_from)==0 ){
        __amatch_add_word(p_cur, word->r_cost+rule->r_cost,
                      word->n_match+rule->n_from, word->word+2, rule->to);
      }
    }
  }while( !isMatch );
  p_cur->current = word;
  dbsql_free(buf);
  return DBSQL_OK;
}

/*
** Called to "rewind" a cursor back to the beginning so that
** it starts its output over again.  Always called at least once
** prior to any __amatch_column, __amatch_rowid, or __amatch_eof call.
*/
static int __amatch_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  amatch_cursor *cur = (amatch_cursor *)vtab_cursor;
  const char *word = "*";
  int idx;
  int rc = DBSQL_OK;

  __amatch_clear_cursor(cur);
  idx = 0;
  if( idx_num & 1 ){
    word = (const char*)dbsql_value_text(argv[0]);
    if( word==0 ) word = "";
    idx++;
  }
  if( idx_num & 2 ){
    cur->r_limit = (amatch_cost)dbsql_value_int(argv[idx]);
    idx++;
  }
  if( idx_num & 4 ){
    cur->i_lang = (amatch_cost)dbsql_value_int(argv[idx]);
    idx++;
  }
  cur->input = dbsql_mprintf("%s", word);
  if( cur->input==0 ) return DBSQL_NOMEM;
  if( strlen(cur->input)>AMATCH_MX_WORD ){
    cur->input[AMATCH_MX_WORD] = 0;
    rc = DBSQL_TOOBIG;
  }
  __amatch_add_word(cur, 0, 0, "", "");
  __amatch_next(vtab_cursor);
  return rc;
}

/*
** Only the word and distance columns have values.  All other columns
** return NULL
*/
static int __amatch_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  amatch_cursor *p_cur = (amatch_cursor*)cur;
  switch( i ){
    case AMATCH_COL_WORD: {
      dbsql_result_text(ctx, p_cur->current->word+2, -1, DBSQL_STATIC);
      break;
    }
    case AMATCH_COL_DISTANCE: {
      dbsql_result_int(ctx, p_cur->current->r_cost);
      break;
    }
    case AMATCH_COL_LANGUAGE: {
      dbsql_result_int(ctx, p_cur->i_lang);
      break;
    }
    case AMATCH_COL_NWORD: {
      dbsql_result_int(ctx, p_cur->n_word);
      break;
    }
    default: {
      dbsql_result_null(ctx);
      break;
    }
  }
  return DBSQL_OK;
}

/*
** The rowid.
*/
static int __amatch_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  amatch_cursor *p_cur = (amatch_cursor*)cur;
  *rowid = p_cur->i_rowid;
  return DBSQL_OK;
}

/*
** EOF indicator
*/
static int __amatch_eof(dbsql_vtab_cursor_t *cur){
  amatch_cursor *p_cur = (amatch_cursor*)cur;
  return p_cur->current==0;
}

/*
** Search for terms of these forms:
**
**   (A)    word MATCH $str
**   (B1)   distance < $value
**   (B2)   distance <= $value
**   (C)    language == $language
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
static int __amatch_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  int i_plan = 0;
  int i_dist_term = -1;
  int i_lang_term = -1;
  int i;
  const struct dbsql_index_constraint_t *constraint;

  (void)tab;
  constraint = idx_info->constraints;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    if( constraint->usable==0 ) continue;
    if( (i_plan & 1)==0 
     && constraint->i_column==0
     && constraint->op==DBSQL_INDEX_CONSTRAINT_MATCH
    ){
      i_plan |= 1;
      idx_info->constraint_usages[i].argv_index = 1;
      idx_info->constraint_usages[i].omit = 1;
    }
    if( (i_plan & 2)==0
     && constraint->i_column==1
     && (constraint->op==DBSQL_INDEX_CONSTRAINT_LT
           || constraint->op==DBSQL_INDEX_CONSTRAINT_LE)
    ){
      i_plan |= 2;
      i_dist_term = i;
    }
    if( (i_plan & 4)==0
     && constraint->i_column==2
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= 4;
      idx_info->constraint_usages[i].omit = 1;
      i_lang_term = i;
    }
  }
  if( i_plan & 2 ){
    idx_info->constraint_usages[i_dist_term].argv_index = 1+((i_plan&1)!=0);
  }
  if( i_plan & 4 ){
    int idx = 1;
    if( i_plan & 1 ) idx++;
    if( i_plan & 2 ) idx++;
    idx_info->constraint_usages[i_lang_term].argv_index = idx;
  }
  idx_info->idx_num = i_plan;
  if( idx_info->n_order_by==1
   && idx_info->order_bies[0].i_column==1
   && idx_info->order_bies[0].desc==0
  ){
    idx_info->order_by_consumed = 1;
  }
  idx_info->estimated_cost = (double)10000;
   
  return DBSQL_OK;
}

/*
** The update() method.  
**
** This implementation disallows DELETE and UPDATE.  The only thing
** allowed is INSERT into the "command" column.
*/
static int __amatch_update(
  dbsql_vtab_t *v_tab,
  int argc,
  dbsql_value_t **argv,
  int64_t *rowid
){
  amatch_vtab *p = (amatch_vtab*)v_tab;
  const unsigned char *cmd;
  (void)rowid;
  if( argc==1 ){
    v_tab->errmsg = dbsql_mprintf("DELETE from %s is not allowed", 
                                      p->self);
    return DBSQL_ERROR;
  }
  if( dbsql_value_type(argv[0])!=DBSQL_NULL ){
    v_tab->errmsg = dbsql_mprintf("UPDATE of %s is not allowed", 
                                      p->self);
    return DBSQL_ERROR;
  }
  if( dbsql_value_type(argv[2+AMATCH_COL_WORD])!=DBSQL_NULL
   || dbsql_value_type(argv[2+AMATCH_COL_DISTANCE])!=DBSQL_NULL
   || dbsql_value_type(argv[2+AMATCH_COL_LANGUAGE])!=DBSQL_NULL
  ){
    v_tab->errmsg = dbsql_mprintf(
            "INSERT INTO %s allowed for column [command] only", p->self);
    return DBSQL_ERROR;
  }
  cmd = dbsql_value_text(argv[2+AMATCH_COL_COMMAND]);
  if( cmd==0 ) return DBSQL_OK;
  
  return DBSQL_OK;
}

/*
** A virtual table module that implements the "approximate_match".
*/
static dbsql_module_t __amatch_module = {
  0,                      /* i_version */
  __amatch_connect,          /* create */
  __amatch_connect,          /* connect */
  __amatch_best_index,        /* best_index */
  __amatch_disconnect,       /* disconnect */
  __amatch_disconnect,       /* destroy */
  __amatch_open,             /* x_open - open a cursor */
  __amatch_close,            /* x_close - close a cursor */
  __amatch_filter,           /* filter - configure scan constraints */
  __amatch_next,             /* next - advance a cursor */
  __amatch_eof,              /* eof - check for end of scan */
  __amatch_column,           /* column - read data */
  __amatch_rowid,            /* rowid - read data */
  __amatch_update,           /* update */
  0,                      /* begin */
  0,                      /* x_sync */
  0,                      /* commit */
  0,                      /* rollback */
  0,                      /* find_method */
  0,                      /* rename */
  0,                      /* savepoint */
  0,                      /* release */
  0,                      /* rollback_to */
  0,                      /* shadow_name */
  0                       /* integrity */
};

#endif /* DBSQL_OMIT_VIRTUALTABLE */

/*
** Register the amatch virtual table
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_amatch_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Not used */
#ifndef DBSQL_OMIT_VIRTUALTABLE
  rc = dbsql_create_module(db, "approximate_match", &__amatch_module, 0);
#endif /* DBSQL_OMIT_VIRTUALTABLE */
  return rc;
}
