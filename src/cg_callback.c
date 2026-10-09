/*
** 2005 May 23 
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
** This file contains functions used to access the internal hash tables
** of user defined functions and collation sequences.
*/

#include "dbsql_int.h"

/*
** Invoke the 'collation needed' callback to request a collation sequence
** in the encoding enc of name name, length n_name.
*/
static void __call_coll_needed(DBSQL *db, int enc, const char *name){
  assert( !db->coll_needed || !db->coll_needed16 );
  if( db->coll_needed ){
    char *external = __db_str_dup(db, name);
    if( !external ) return;
    db->coll_needed(db->coll_needed_arg, db, enc, external);
    __dbsql_db_free(db, external);
  }
#ifndef DBSQL_OMIT_UTF16
  if( db->coll_needed16 ){
    char const *external;
    dbsql_value_t *tmp = __value_new(db);
    __value_set_str(tmp, -1, name, DBSQL_UTF8, DBSQL_STATIC);
    external = __value_text(tmp, DBSQL_UTF16NATIVE);
    if( external ){
      db->coll_needed16(db->coll_needed_arg, db, (int)ENC(db), external);
    }
    __value_free(tmp);
  }
#endif
}

/*
** This routine is called if the collation factory fails to deliver a
** collation function in the best encoding but there may be other versions
** of this collation function (for other text encodings) available. Use one
** of these instead if they exist. Avoid a UTF-8 <-> UTF-16 conversion if
** possible.
*/
static int __synth_coll_seq(DBSQL *db, coll_seq_t *coll){
  coll_seq_t *coll2;
  char *z = coll->name;
  int i;
  static const u_int8_t encs[] = { DBSQL_UTF16BE, DBSQL_UTF16LE, DBSQL_UTF8 };
  for(i=0; i<3; i++){
    coll2 = __find_coll_seq(db, encs[i], z, 0);
    if( coll2->cmp!=0 ){
      memcpy(coll, coll2, sizeof(coll_seq_t));
      coll->x_del = 0;         /* Do not copy the destructor */
      return DBSQL_OK;
    }
  }
  return DBSQL_ERROR;
}

/*
** This routine is called on a collation sequence before it is used to
** check that it is defined. An undefined collation sequence exists when
** a database is loaded that contains references to collation sequences
** that have not been defined by dbsql_create_collation() etc.
**
** If required, this routine calls the 'collation needed' callback to
** request a definition of the collating sequence. If this doesn't work, 
** an equivalent collating sequence that uses a text encoding different
** from the main database is substituted, if one is available.
*/
int __check_coll_seq(parser_t *parser, coll_seq_t *coll){
  if( coll && coll->cmp==0 ){
    const char *name = coll->name;
    DBSQL *db = parser->db;
    coll_seq_t *p = __get_coll_seq(parser, ENC(db), coll, name);
    if( !p ){
      return DBSQL_ERROR;
    }
    assert( p==coll );
  }
  return DBSQL_OK;
}



/*
** Locate and return an entry from the db.coll_seqs hash table. If the entry
** specified by name and n_name is not found and parameter 'create' is
** true, then create a new entry. Otherwise return NULL.
**
** Each pointer stored in the DBSQL.coll_seqs hash table contains an
** array of three coll_seq_t structures. The first is the collation sequence
** preferred for UTF-8, the second UTF-16le, and the third UTF-16be.
**
** Stored immediately after the three collation sequences is a copy of
** the collation sequence name. A pointer to this string is stored in
** each collation sequence structure.
*/
static coll_seq_t *__find_coll_seq_entry(
  DBSQL *db,          /* Database connection */
  const char *name,    /* Name of the collating sequence */
  int create            /* Create a new entry if true */
){
  coll_seq_t *coll;
  coll = __hash_find(&db->coll_seqs, name);

  if( 0==coll && create ){
    int n_name = __strlen30(name) + 1;
    coll = __db_malloc_zero(db, 3*sizeof(*coll) + n_name);
    if( coll ){
      coll_seq_t *del = 0;
      coll[0].name = (char*)&coll[3];
      coll[0].enc = DBSQL_UTF8;
      coll[1].name = (char*)&coll[3];
      coll[1].enc = DBSQL_UTF16LE;
      coll[2].name = (char*)&coll[3];
      coll[2].enc = DBSQL_UTF16BE;
      memcpy(coll[0].name, name, n_name);
      del = __hash_insert(&db->coll_seqs, coll[0].name, coll);

      /* If a malloc() failure occurred in __hash_insert(), it will 
      ** return the coll pointer to be deleted (because it wasn't added
      ** to the hash table).
      */
      assert( del==0 || del==coll );
      if( del!=0 ){
        __oom_fault(db);
        __dbsql_db_free(db, del);
        coll = 0;
      }
    }
  }
  return coll;
}

/*
** Parameter name points to a UTF-8 encoded string n_name bytes long.
** Return the coll_seq_t* pointer for the collation sequence named name
** for the encoding 'enc' from the database 'db'.
**
** If the entry specified is not found and 'create' is true, then create a
** new entry.  Otherwise return NULL.
**
** A separate function __locate_coll_seq() is a wrapper around
** this routine.  __locate_coll_seq() invokes the collation factory
** if necessary and generates an error message if the collating sequence
** cannot be found.
**
** See also: __locate_coll_seq(), __get_coll_seq()
*/
coll_seq_t *__find_coll_seq(
  DBSQL *db,          /* Database connection to search */
  u_int8_t enc,               /* Desired text encoding */
  const char *name,    /* Name of the collating sequence.  Might be NULL */
  int create            /* True to create coll_seq_t if doesn't already exist */
){
  coll_seq_t *coll;
  assert( DBSQL_UTF8==1 && DBSQL_UTF16LE==2 && DBSQL_UTF16BE==3 );
  assert( enc>=DBSQL_UTF8 && enc<=DBSQL_UTF16BE );
  if( name ){
    coll = __find_coll_seq_entry(db, name, create);
    if( coll ) coll += enc-1;
  }else{
    coll = db->dflt_coll;
  }
  return coll;
}

/*
** Change the text encoding for a database connection. This means that
** the dflt_coll must change as well.
*/
void __set_text_encoding(DBSQL *db, u_int8_t enc){
  assert( enc==DBSQL_UTF8 || enc==DBSQL_UTF16LE || enc==DBSQL_UTF16BE );
  db->enc = enc;
  /* EVIDENCE-OF: R-08308-17224 The default collating function for all
  ** strings is BINARY. 
  */
  db->dflt_coll = __find_coll_seq(db, enc, __str_binary, 0);
  __expire_prepared_statements(db, 1);
}

/*
** This function is responsible for invoking the collation factory callback
** or substituting a collation sequence of a different encoding when the
** requested collation sequence is not available in the desired encoding.
** 
** If it is not NULL, then coll must point to the database native encoding 
** collation sequence with name name, length n_name.
**
** The return value is either the collation sequence to be used in database
** db for collation type name name, length n_name, or NULL, if no collation
** sequence can be found.  If no collation is found, leave an error message.
**
** See also: __locate_coll_seq(), __find_coll_seq()
*/
coll_seq_t *__get_coll_seq(
  parser_t *parser,        /* Parsing context */
  u_int8_t enc,               /* The desired encoding for the collating sequence */
  coll_seq_t *coll,       /* Collating sequence with native encoding, or NULL */
  const char *name     /* Collating sequence name */
){
  coll_seq_t *p;
  DBSQL *db = parser->db;

  p = coll;
  if( !p ){
    p = __find_coll_seq(db, enc, name, 0);
  }
  if( !p || !p->cmp ){
    /* No collation sequence of this type for this encoding is registered.
    ** Call the collation factory to see if it can supply us with one.
    */
    __call_coll_needed(db, enc, name);
    p = __find_coll_seq(db, enc, name, 0);
  }
  if( p && !p->cmp && __synth_coll_seq(db, p) ){
    p = 0;
  }
  assert( !p || p->cmp );
  if( p==0 ){
    __error_msg(parser, "no such collation sequence: %s", name);
    parser->rc = DBSQL_ERROR_MISSING_COLLSEQ;
  }
  return p;
}

/*
** This function returns the collation sequence for database native text
** encoding identified by the string name.
**
** If the requested collation sequence is not available, or not available
** in the database native encoding, the collation factory is invoked to
** request it. If the collation factory does not supply such a sequence,
** and the sequence is available in another text encoding, then that is
** returned instead.
**
** If no versions of the requested collations sequence are available, or
** another error occurs, NULL is returned and an error message written into
** parser.
**
** This routine is a wrapper around __find_coll_seq().  This routine
** invokes the collation factory if the named collation cannot be found
** and generates an error message.
**
** See also: __find_coll_seq(), __get_coll_seq()
*/
coll_seq_t *__locate_coll_seq(parser_t *parser, const char *name){
  DBSQL *db = parser->db;
  u_int8_t enc = ENC(db);
  u_int8_t initbusy = db->init.busy;
  coll_seq_t *coll;

  coll = __find_coll_seq(db, enc, name, initbusy);
  if( !initbusy && (!coll || !coll->cmp) ){
    coll = __get_coll_seq(parser, enc, coll, name);
  }

  return coll;
}

/* During the search for the best function definition, this procedure
** is called to test how well the function passed as the first argument
** matches the request for a function with n_arg arguments in a system
** that uses encoding enc. The value returned indicates how well the
** request is matched. A higher value indicates a better match.
**
** If n_arg is -1 that means to only return a match (non-zero) if p->n_arg
** is also -1.  In other words, we are searching for a function that
** takes a variable number of arguments.
**
** If n_arg is -2 that means that we are searching for any function 
** regardless of the number of arguments it uses, so return a positive
** match score for any
**
** The returned value is always between 0 and 6, as follows:
**
** 0: Not a match.
** 1: UTF8/16 conversion required and function takes any number of arguments.
** 2: UTF16 byte order change required and function takes any number of args.
** 3: encoding matches and function takes any number of arguments
** 4: UTF8/16 conversion required - argument count matches exactly
** 5: UTF16 byte order conversion required - argument count matches exactly
** 6: Perfect match:  encoding and argument count match exactly.
**
** If n_arg==(-2) then any function with a non-null s_func is
** a perfect match and any function with s_func NULL is
** a non-match.
*/
#define FUNC_PERFECT_MATCH 6  /* The score for a perfect match */
static int __match_quality(
  func_def_t *p,     /* The function we are evaluating for match quality */
  int n_arg,       /* Desired number of arguments.  (-1)==any */
  u_int8_t enc          /* Desired text encoding */
){
  int match;
  assert( p->n_arg>=(-4) && p->n_arg!=(-2) );
  assert( n_arg>=(-2) );

  /* Wrong number of arguments means "no match" */
  if( p->n_arg!=n_arg ){
    if( n_arg==(-2) ) return p->s_func==0 ? 0 : FUNC_PERFECT_MATCH;
    if( p->n_arg>=0 ) return 0;
    /* Special p->n_arg values available to built-in functions only:
    **    -3     1 or more arguments required
    **    -4     2 or more arguments required
    */
    if( p->n_arg<(-2) && n_arg<(-2-p->n_arg) ) return 0;
  }

  /* Give a better score to a function with a specific number of arguments
  ** than to function that accepts any number of arguments. */
  if( p->n_arg==n_arg ){
    match = 4;
  }else{
    match = 1;
  }

  /* Bonus points if the text encoding matches */
  if( enc==(p->func_flags & DBSQL_FUNC_ENCMASK) ){
    match += 2;  /* Exact encoding match */
  }else if( (enc & p->func_flags & 2)!=0 ){
    match += 1;  /* Both are UTF16, but with different byte orders */
  }

  return match;
}

/*
** Search a func_def_hash_t for a function with the given name.  Return
** a pointer to the matching func_def_t if found, or 0 if there is no match.
*/
func_def_t *__function_search(
  int h,               /* hash_t of the name */
  const char *func    /* Name of function */
){
  func_def_t *p;
  for(p=__builtin_functions.a[h]; p; p=p->u.hash){
    assert( p->func_flags & DBSQL_FUNC_BUILTIN );
    if( __str_icmp(p->name, func)==0 ){
      return p;
    }
  }
  return 0;
}

/*
** Insert a new func_def_t into a func_def_hash_t hash table.
*/
void __insert_builtin_funcs(
  func_def_t *defs,      /* List of global functions to be inserted */
  int n_def            /* Length of the defs[] list */
){
  int i;
  for(i=0; i<n_def; i++){
    func_def_t *other;
    const char *name = defs[i].name;
    int n_name = __strlen30(name);
    int h = DBSQL_FUNC_HASH(name[0], n_name);
    assert( defs[i].func_flags & DBSQL_FUNC_BUILTIN );
    other = __function_search(h, name);
    if( other ){
      assert( other!=&defs[i] && other->next!=&defs[i] );
      defs[i].next = other->next;
      other->next = &defs[i];
    }else{
      defs[i].next = 0;
      defs[i].u.hash = __builtin_functions.a[h];
      __builtin_functions.a[h] = &defs[i];
    }
  }
}
  
  

/*
** Locate a user function given a name, a number of arguments and a flag
** indicating whether the function prefers UTF-16 over UTF-8.  Return a
** pointer to the func_def_t structure that defines that function, or return
** NULL if the function does not exist.
**
** If the create_flag argument is true, then a new (blank) func_def_t
** structure is created and liked into the "db" structure if a
** no matching function previously existed.
**
** If n_arg is -2, then the first valid function found is returned.  A
** function is valid if s_func is non-zero.  The n_arg==(-2)
** case is used to see if name is a valid function name for some number
** of arguments.  If n_arg is -2, then create_flag must be 0.
**
** If create_flag is false, then a function with the required name and
** number of arguments may be returned even if the text_rep flag does not
** match that requested.
*/
func_def_t *__find_function(
  DBSQL *db,       /* An open database */
  const char *name, /* Name of the function.  zero-terminated */
  int n_arg,          /* Number of arguments.  -1 means any number */
  u_int8_t enc,            /* Preferred text encoding */
  u_int8_t create_flag      /* Create new entry if true and does not otherwise exist */
){
  func_def_t *p;         /* Iterator variable */
  func_def_t *best = 0; /* Best match found so far */
  int best_score = 0;  /* Score of best match */
  int h;              /* hash_t value */
  int n_name;          /* Length of the name */

  assert( n_arg>=(-2) );
  assert( n_arg>=(-1) || create_flag==0 );
  n_name = __strlen30(name);

  /* First search for a match amongst the application-defined functions.
  */
  p = (func_def_t*)__hash_find(&db->funcs, name);
  while( p ){
    int score = __match_quality(p, n_arg, enc);
    if( score>best_score ){
      best = p;
      best_score = score;
    }
    p = p->next;
  }

  /* If no match is found, search the built-in functions.
  **
  ** If the DBFLAG_PreferBuiltin flag is set, then search the built-in
  ** functions even if a prior app-defined function was found.  And give
  ** priority to built-in functions.
  **
  ** Except, if create_flag is true, that means that we are trying to
  ** install a new function.  Whatever func_def_t structure is returned it will
  ** have fields overwritten with new information appropriate for the
  ** new function.  But the __func_defs for built-in functions are read-only.
  ** So we must not search for built-ins when creating a new function.
  */ 
  if( !create_flag && (best==0 || (db->m_db_flags & DBFLAG_PreferBuiltin)!=0) ){
    best_score = 0;
    h = DBSQL_FUNC_HASH(__upper_to_lower[(u_int8_t)name[0]], n_name);
    p = __function_search(h, name);
    while( p ){
      int score = __match_quality(p, n_arg, enc);
      if( score>best_score ){
        best = p;
        best_score = score;
      }
      p = p->next;
    }
  }

  /* If the create_flag parameter is true and the search did not reveal an
  ** exact match for the name, number of arguments and encoding, then add a
  ** new entry to the hash table and return it.
  */
  if( create_flag && best_score<FUNC_PERFECT_MATCH && 
      (best = __db_malloc_zero(db, sizeof(*best)+n_name+1))!=0 ){
    func_def_t *other;
    u_int8_t *z;
    best->name = (const char*)&best[1];
    best->n_arg = (u_int16_t)n_arg;
    best->func_flags = enc;
    memcpy((char*)&best[1], name, n_name+1);
    for(z=(u_int8_t*)best->name; *z; z++) *z = __upper_to_lower[*z];
    other = (func_def_t*)__hash_insert(&db->funcs, best->name, best);
    if( other==best ){
      __dbsql_db_free(db, best);
      __oom_fault(db);
      return 0;
    }else{
      best->next = other;
    }
  }

  if( best && (best->s_func || create_flag) ){
    return best;
  }
  return 0;
}

/*
** Free all resources held by the schema structure. The void* argument points
** at a schema_t struct. This function does not call __dbsql_db_free(db, ) on the 
** pointer itself, it just cleans up subsidiary resources (i.e. the contents
** of the schema hash tables).
**
** The schema_t.cache_size variable is not cleared.
*/
void __schema_clear(void *p){
  hash_t temp1;
  hash_t temp2;
  hash_elem_t *elem;
  schema_t *schema = (schema_t *)p;
  DBSQL xdb;

  memset(&xdb, 0, sizeof(xdb));
  temp1 = schema->tbl_hash;
  temp2 = schema->trig_hash;
  __hash_init(&schema->trig_hash);
  __hash_clear(&schema->idx_hash);
  for(elem=dbsql_hash_first(&temp2); elem; elem=dbsql_hash_next(elem)){
    __delete_trigger(&xdb, (trigger_t*)dbsql_hash_data(elem));
  }

  __hash_clear(&temp2);
  __hash_init(&schema->tbl_hash);
  for(elem=dbsql_hash_first(&temp1); elem; elem=dbsql_hash_next(elem)){
    table_t *tab = dbsql_hash_data(elem);
    __delete_table(&xdb, tab);
  }
  __hash_clear(&temp1);
  __hash_clear(&schema->fkey_hash);
  schema->seq_tab = 0;
  if( schema->schema_flags & DB_SchemaLoaded ){
    schema->i_generation++;
  }
  schema->schema_flags &= ~(DB_SchemaLoaded|DB_ResetWanted);
}

/*
** Find and return the schema associated with a BTree.  Create
** a new one if necessary.
*/
schema_t *__schema_get(DBSQL *db, sm_t *bt){
  schema_t * p;
  if( bt ){
    p = (schema_t *)__sm_schema(bt, sizeof(schema_t), __schema_clear);
  }else{
    p = (schema_t *)__db_malloc_zero(0, sizeof(schema_t));
  }
  if( !p ){
    __oom_fault(db);
  }else if ( 0==p->file_format ){
    __hash_init(&p->tbl_hash);
    __hash_init(&p->idx_hash);
    __hash_init(&p->trig_hash);
    __hash_init(&p->fkey_hash);
    p->enc = DBSQL_UTF8;
  }
  return p;
}
