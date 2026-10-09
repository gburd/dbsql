/*
** 2014 May 31
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** Interfaces to extend FTS5. Using the interfaces defined in this file, 
** FTS5 may be extended with:
**
**     * custom tokenizers, and
**     * custom auxiliary functions.
*/


#ifndef _FTS5_H
#define _FTS5_H

#include "dbsql.h"

#ifdef __cplusplus
extern "C" {
#endif

/*************************************************************************
** CUSTOM AUXILIARY FUNCTIONS
**
** Virtual table implementations may overload SQL functions by implementing
** the dbsql_module_t.find_function() method.
*/

typedef struct fts5_extension_api_t fts5_extension_api_t;
typedef struct fts5_context_t fts5_context_t;
typedef struct fts5_phrase_iter_t fts5_phrase_iter_t;

typedef void (*fts5_extension_function)(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  dbsql_func_t *ctx,          /* Context for returning result/error */
  int n_val,                       /* Number of values in vals[] array */
  dbsql_value_t **vals           /* Array of trailing arguments */
);

struct fts5_phrase_iter_t {
  const unsigned char *a;
  const unsigned char *b;
};

/*
** EXTENSION API FUNCTIONS
**
** user_data(fts):
**   Return a copy of the user_data pointer passed to the create_function()
**   API when the extension function was registered.
**
** column_total_size(fts, i_col, pn_token):
**   If parameter i_col is less than zero, set output variable *pn_token
**   to the total number of tokens in the FTS5 table. Or, if i_col is
**   non-negative but less than the number of columns in the table, return
**   the total number of tokens in column i_col, considering all rows in 
**   the FTS5 table.
**
**   If parameter i_col is greater than or equal to the number of columns
**   in the table, DBSQL_RANGE is returned. Or, if an error occurs (e.g.
**   an OOM condition or IO error), an appropriate dbsql error code is 
**   returned.
**
** column_count(fts):
**   Return the number of columns in the table.
**
** column_size(fts, i_col, pn_token):
**   If parameter i_col is less than zero, set output variable *pn_token
**   to the total number of tokens in the current row. Or, if i_col is
**   non-negative but less than the number of columns in the table, set
**   *pn_token to the number of tokens in column i_col of the current row.
**
**   If parameter i_col is greater than or equal to the number of columns
**   in the table, DBSQL_RANGE is returned. Or, if an error occurs (e.g.
**   an OOM condition or IO error), an appropriate dbsql error code is 
**   returned.
**
**   This function may be quite inefficient if used with an FTS5 table
**   created with the "columnsize=0" option.
**
** column_text:
**   If parameter i_col is less than zero, or greater than or equal to the
**   number of columns in the table, DBSQL_RANGE is returned. 
**
**   Otherwise, this function attempts to retrieve the text of column i_col of
**   the current document. If successful, (*pz) is set to point to a buffer
**   containing the text in utf-8 encoding, (*pn) is set to the size in bytes
**   (not characters) of the buffer and DBSQL_OK is returned. Otherwise,
**   if an error occurs, an dbsql error code is returned and the final values
**   of (*pz) and (*pn) are undefined.
**
** phrase_count:
**   Returns the number of phrases in the current query expression.
**
** phrase_size:
**   If parameter i_col is less than zero, or greater than or equal to the
**   number of phrases in the current query, as returned by phrase_count, 
**   0 is returned. Otherwise, this function returns the number of tokens in
**   phrase i_phrase of the query. Phrases are numbered starting from zero.
**
** inst_count:
**   Set *pn_inst to the total number of occurrences of all phrases within
**   the query within the current row. Return DBSQL_OK if successful, or
**   an error code (i.e. DBSQL_NOMEM) if an error occurs.
**
**   This API can be quite slow if used with an FTS5 table created with the
**   "detail=none" or "detail=column" option. If the FTS5 table is created 
**   with either "detail=none" or "detail=column" and "content=" option 
**   (i.e. if it is a contentless table), then this API always returns 0.
**
** inst:
**   Query for the details of phrase match i_idx within the current row.
**   Phrase matches are numbered starting from zero, so the i_idx argument
**   should be greater than or equal to zero and smaller than the value
**   output by inst_count(). If i_idx is less than zero or greater than
**   or equal to the value returned by inst_count(), DBSQL_RANGE is returned.
**
**   Otherwise, output parameter *pi_phrase is set to the phrase number, *pi_col
**   to the column in which it occurs and *pi_off the token offset of the
**   first token of the phrase. DBSQL_OK is returned if successful, or an
**   error code (i.e. DBSQL_NOMEM) if an error occurs.
**
**   This API can be quite slow if used with an FTS5 table created with the
**   "detail=none" or "detail=column" option. 
**
** rowid:
**   Returns the rowid of the current row.
**
** tokenize:
**   Tokenize text using the tokenizer belonging to the FTS5 table.
**
** query_phrase(fts5, i_phrase, user_data, callback):
**   This API function is used to query the FTS table for phrase i_phrase
**   of the current query. Specifically, a query equivalent to:
**
**       ... FROM ftstable WHERE ftstable MATCH $p ORDER BY rowid
**
**   with $p set to a phrase equivalent to the phrase i_phrase of the
**   current query is executed. Any column filter that applies to
**   phrase i_phrase of the current query is included in $p. For each 
**   row visited, the callback function passed as the fourth argument 
**   is invoked. The context and API objects passed to the callback 
**   function may be used to access the properties of each matched row.
**   Invoking Api.user_data() returns a copy of the pointer passed as 
**   the third argument to user_data.
**
**   If parameter i_phrase is less than zero, or greater than or equal to
**   the number of phrases in the query, as returned by phrase_count(),
**   this function returns DBSQL_RANGE.
**
**   If the callback function returns any value other than DBSQL_OK, the
**   query is abandoned and the query_phrase function returns immediately.
**   If the returned value is DBSQL_DONE, query_phrase returns DBSQL_OK.
**   Otherwise, the error code is propagated upwards.
**
**   If the query runs to completion without incident, DBSQL_OK is returned.
**   Or, if some error occurs before the query completes or is aborted by
**   the callback, an dbsql error code is returned.
**
**
** set_auxdata(fts5, aux, x_delete)
**
**   Save the pointer passed as the second argument as the extension function's 
**   "auxiliary data". The pointer may then be retrieved by the current or any
**   future invocation of the same fts5 extension function made as part of
**   the same MATCH query using the get_auxdata() API.
**
**   Each extension function is allocated a single auxiliary data slot for
**   each FTS query (MATCH expression). If the extension function is invoked 
**   more than once for a single FTS query, then all invocations share a 
**   single auxiliary data context.
**
**   If there is already an auxiliary data pointer when this function is
**   invoked, then it is replaced by the new pointer. If an x_delete callback
**   was specified along with the original pointer, it is invoked at this
**   point.
**
**   The x_delete callback, if one is specified, is also invoked on the
**   auxiliary data pointer after the FTS5 query has finished.
**
**   If an error (e.g. an OOM condition) occurs within this function,
**   the auxiliary data is set to NULL and an error code returned. If the
**   x_delete parameter was not NULL, it is invoked on the auxiliary data
**   pointer before returning.
**
**
** get_auxdata(fts5, clear)
**
**   Returns the current auxiliary data pointer for the fts5 extension 
**   function. See the set_auxdata() method for details.
**
**   If the clear argument is non-zero, then the auxiliary data is cleared
**   (set to NULL) before this function returns. In this case the x_delete,
**   if any, is not invoked.
**
**
** row_count(fts5, pn_row)
**
**   This function is used to retrieve the total number of rows in the table.
**   In other words, the same value that would be returned by:
**
**        SELECT count(*) FROM ftstable;
**
** phrase_first()
**   This function is used, along with type fts5_phrase_iter_t and the phrase_next
**   method, to iterate through all instances of a single query phrase within
**   the current row. This is the same information as is accessible via the
**   inst_count/inst APIs. While the inst_count/inst APIs are more convenient
**   to use, this API may be faster under some circumstances. To iterate 
**   through instances of phrase i_phrase, use the following code:
**
**       fts5_phrase_iter_t iter;
**       int i_col, i_off;
**       for(api->phrase_first(fts, i_phrase, &iter, &i_col, &i_off);
**           i_col>=0;
**           api->phrase_next(fts, &iter, &i_col, &i_off)
**       ){
**         // An instance of phrase i_phrase at offset i_off of column i_col
**       }
**
**   The fts5_phrase_iter_t structure is defined above. Applications should not
**   modify this structure directly - it should only be used as shown above
**   with the phrase_first() and phrase_next() API methods (and by
**   phrase_first_column() and phrase_next_column() as illustrated below).
**
**   This API can be quite slow if used with an FTS5 table created with the
**   "detail=none" or "detail=column" option. If the FTS5 table is created 
**   with either "detail=none" or "detail=column" and "content=" option 
**   (i.e. if it is a contentless table), then this API always iterates
**   through an empty set (all calls to phrase_first() set i_col to -1).
**
**   In all cases, matches are visited in (column ASC, offset ASC) order.
**   i.e. all those in column 0, sorted by offset, followed by those in 
**   column 1, etc.
**
** phrase_next()
**   See phrase_first above.
**
** phrase_first_column()
**   This function and phrase_next_column() are similar to the phrase_first()
**   and phrase_next() APIs described above. The difference is that instead
**   of iterating through all instances of a phrase in the current row, these
**   APIs are used to iterate through the set of columns in the current row
**   that contain one or more instances of a specified phrase. For example:
**
**       fts5_phrase_iter_t iter;
**       int i_col;
**       for(api->phrase_first_column(fts, i_phrase, &iter, &i_col);
**           i_col>=0;
**           api->phrase_next_column(fts, &iter, &i_col)
**       ){
**         // column_t i_col contains at least one instance of phrase i_phrase
**       }
**
**   This API can be quite slow if used with an FTS5 table created with the
**   "detail=none" option. If the FTS5 table is created with either 
**   "detail=none" "content=" option (i.e. if it is a contentless table), 
**   then this API always iterates through an empty set (all calls to 
**   phrase_first_column() set i_col to -1).
**
**   The information accessed using this API and its companion
**   phrase_first_column() may also be obtained using phrase_first/phrase_next
**   (or inst/inst_count). The chief advantage of this API is that it is
**   significantly more efficient than those alternatives when used with
**   "detail=column" tables.  
**
** phrase_next_column()
**   See phrase_first_column above.
**
** query_token(fts5, i_phrase, i_token, token, pn_token)
**   This is used to access token i_token of phrase i_phrase of the current
**   query. Before returning, output parameter *token is set to point
**   to a buffer containing the requested token, and *pn_token to the
**   size of this buffer in bytes.
**
**   If i_phrase or i_token are less than zero, or if i_phrase is greater than
**   or equal to the number of phrases in the query as reported by 
**   phrase_count(), or if i_token is equal to or greater than the number of
**   tokens in the phrase, DBSQL_RANGE is returned and *token and *pn_token
     are both zeroed.
**
**   The output text is not a copy of the query text that specified the
**   token. It is the output of the tokenizer module. For tokendata=1
**   tables, this includes any embedded 0x00 and trailing data.
**
** inst_token(fts5, i_idx, i_token, token, pn_token)
**   This is used to access token i_token of phrase hit i_idx within the
**   current row. If i_idx is less than zero or greater than or equal to the
**   value returned by inst_count(), DBSQL_RANGE is returned.  Otherwise,
**   output variable (*token) is set to point to a buffer containing the
**   matching document token, and (*pn_token) to the size of that buffer in 
**   bytes. 
**
**   The output text is not a copy of the document text that was tokenized.
**   It is the output of the tokenizer module. For tokendata=1 tables, this 
**   includes any embedded 0x00 and trailing data.
**
**   This API may be slow in some cases if the token identified by parameters 
**   i_idx and i_token matched a prefix token in the query. In most cases, the
**   first call to this API for each prefix token in the query is forced
**   to scan the portion of the full-text index that matches the prefix
**   token to collect the extra data required by this API. If the prefix
**   token matches a large number of token instances in the document set,
**   this may be a performance problem. 
**
**   If the user knows in advance that a query may use this API for a
**   prefix token, FTS5 may be configured to collect all required data as part
**   of the initial querying of the full-text index, avoiding the second scan
**   entirely. This also causes prefix queries that do not use this API to 
**   run more slowly and use more memory. FTS5 may be configured in this way
**   either on a per-table basis using the [FTS5 insttoken | 'insttoken'] 
**   option, or on a per-query basis using the 
**   [fts5_insttoken | fts5_insttoken()] user function.
**
**   This API can be quite slow if used with an FTS5 table created with the
**   "detail=none" or "detail=column" option. 
**
** column_locale(fts5, i_idx, locale, pn_locale)
**   If parameter i_col is less than zero, or greater than or equal to the
**   number of columns in the table, DBSQL_RANGE is returned.
**
**   Otherwise, this function attempts to retrieve the locale associated
**   with column i_col of the current row. Usually, there is no associated
**   locale, and output parameters (*locale) and (*pn_locale) are set
**   to NULL and 0, respectively. However, if the fts5_locale() function
**   was used to associate a locale with the value when it was inserted
**   into the fts5 table, then (*locale) is set to point to a nul-terminated
**   buffer containing the name of the locale in utf-8 encoding. (*pn_locale) 
**   is set to the size in bytes of the buffer, not including the 
**   nul-terminator.
**
**   If successful, DBSQL_OK is returned. Or, if an error occurs, an
**   dbsql error code is returned. The final value of the output parameters
**   is undefined in this case.
**
** tokenize_v2:
**   Tokenize text using the tokenizer belonging to the FTS5 table. This
**   API is the same as the tokenize() API, except that it allows a tokenizer
**   locale to be specified.
*/
struct fts5_extension_api_t {
  int i_version;                   /* Currently always set to 4 */

  void *(*x_user_data)(fts5_context_t*);

  int (*column_count)(fts5_context_t*);
  int (*row_count)(fts5_context_t*, int64_t *pn_row);
  int (*column_total_size)(fts5_context_t*, int i_col, int64_t *pn_token);

  int (*tokenize)(fts5_context_t*, 
    const char *text, int n_text, /* Text to tokenize */
    void *ctx,                   /* Context passed to token() */
    int (*x_token)(void*, int, const char*, int, int, int)       /* Callback */
  );

  int (*phrase_count)(fts5_context_t*);
  int (*phrase_size)(fts5_context_t*, int i_phrase);

  int (*inst_count)(fts5_context_t*, int *pn_inst);
  int (*inst)(fts5_context_t*, int i_idx, int *pi_phrase, int *pi_col, int *pi_off);

  int64_t (*rowid)(fts5_context_t*);
  int (*column_text)(fts5_context_t*, int i_col, const char **pz, int *pn);
  int (*column_size)(fts5_context_t*, int i_col, int *pn_token);

  int (*query_phrase)(fts5_context_t*, int i_phrase, void *user_data,
    int(*)(const fts5_extension_api_t*,fts5_context_t*,void*)
  );
  int (*set_auxdata)(fts5_context_t*, void *aux, void(*x_delete)(void*));
  void *(*get_auxdata)(fts5_context_t*, int clear);

  int (*phrase_first)(fts5_context_t*, int i_phrase, fts5_phrase_iter_t*, int*, int*);
  void (*phrase_next)(fts5_context_t*, fts5_phrase_iter_t*, int *pi_col, int *pi_off);

  int (*phrase_first_column)(fts5_context_t*, int i_phrase, fts5_phrase_iter_t*, int*);
  void (*phrase_next_column)(fts5_context_t*, fts5_phrase_iter_t*, int *pi_col);

  /* Below this point are i_version>=3 only */
  int (*query_token)(fts5_context_t*, 
      int i_phrase, int i_token, 
      const char **pp_token, int *pn_token
  );
  int (*inst_token)(fts5_context_t*, int i_idx, int i_token, const char**, int*);

  /* Below this point are i_version>=4 only */
  int (*column_locale)(fts5_context_t*, int i_col, const char **pz, int *pn);
  int (*tokenize_v2)(fts5_context_t*,
    const char *text, int n_text,      /* Text to tokenize */
    const char *locale, int n_locale,  /* Locale to pass to tokenizer */
    void *ctx,                        /* Context passed to token() */
    int (*x_token)(void*, int, const char*, int, int, int)       /* Callback */
  );
};

/* 
** CUSTOM AUXILIARY FUNCTIONS
*************************************************************************/

/*************************************************************************
** CUSTOM TOKENIZERS
**
** Applications may also register custom tokenizer types. A tokenizer 
** is registered by providing fts5 with a populated instance of the 
** following structure. All structure methods must be defined, setting
** any member of the fts5_tokenizer struct to NULL leads to undefined
** behaviour. The structure methods are expected to function as follows:
**
** create:
**   This function is used to allocate and initialize a tokenizer instance.
**   A tokenizer instance is required to actually tokenize text.
**
**   The first argument passed to this function is a copy of the (void*)
**   pointer provided by the application when the fts5_tokenizer_v2 object
**   was registered with FTS5 (the third argument to create_tokenizer()). 
**   The second and third arguments are an array of nul-terminated strings
**   containing the tokenizer arguments, if any, specified following the
**   tokenizer name as part of the CREATE VIRTUAL TABLE statement used
**   to create the FTS5 table.
**
**   The final argument is an output variable. If successful, (*out) 
**   should be set to point to the new tokenizer handle and DBSQL_OK
**   returned. If an error occurs, some value other than DBSQL_OK should
**   be returned. In this case, fts5 assumes that the final value of *out 
**   is undefined.
**
** x_delete:
**   This function is invoked to delete a tokenizer handle previously
**   allocated using create(). Fts5 guarantees that this function will
**   be invoked exactly once for each successful call to create().
**
** tokenize:
**   This function is expected to tokenize the n_text byte string indicated 
**   by argument text. text may or may not be nul-terminated. The first
**   argument passed to this function is a pointer to an fts5_tokenizer_t object
**   returned by an earlier call to create().
**
**   The third argument indicates the reason that FTS5 is requesting
**   tokenization of the supplied text. This is always one of the following
**   four values:
**
**   <ul><li> <b>FTS5_TOKENIZE_DOCUMENT</b> - A document is being inserted into
**            or removed from the FTS table. The tokenizer is being invoked to
**            determine the set of tokens to add to (or delete from) the
**            FTS index.
**
**       <li> <b>FTS5_TOKENIZE_QUERY</b> - A MATCH query is being executed 
**            against the FTS index. The tokenizer is being called to tokenize 
**            a bareword or quoted string specified as part of the query.
**
**       <li> <b>(FTS5_TOKENIZE_QUERY | FTS5_TOKENIZE_PREFIX)</b> - Same as
**            FTS5_TOKENIZE_QUERY, except that the bareword or quoted string is
**            followed by a "*" character, indicating that the last token
**            returned by the tokenizer will be treated as a token prefix.
**
**       <li> <b>FTS5_TOKENIZE_AUX</b> - The tokenizer is being invoked to 
**            satisfy an fts5_api.tokenize() request made by an auxiliary
**            function. Or an fts5_api.column_size() request made by the same
**            on a columnsize=0 database.  
**   </ul>
**
**   The sixth and seventh arguments passed to tokenize() - locale and
**   n_locale - are a pointer to a buffer containing the locale to use for
**   tokenization (e.g. "en_US") and its size in bytes, respectively. The
**   locale buffer is not nul-terminated. locale may be passed NULL (in
**   which case n_locale is always 0) to indicate that the tokenizer should
**   use its default locale.
**
**   For each token in the input string, the supplied callback token() must
**   be invoked. The first argument to it should be a copy of the pointer
**   passed as the second argument to tokenize(). The third and fourth
**   arguments are a pointer to a buffer containing the token text, and the
**   size of the token in bytes. The 4th and 5th arguments are the byte offsets
**   of the first byte of and first byte immediately following the text from
**   which the token is derived within the input.
**
**   The second argument passed to the token() callback ("tflags") should
**   normally be set to 0. The exception is if the tokenizer supports 
**   synonyms. In this case see the discussion below for details.
**
**   FTS5 assumes the token() callback is invoked for each token in the 
**   order that they occur within the input text.
**
**   If an token() callback returns any value other than DBSQL_OK, then
**   the tokenization should be abandoned and the tokenize() method should
**   immediately return a copy of the token() return value. Or, if the
**   input buffer is exhausted, tokenize() should return DBSQL_OK. Finally,
**   if an error occurs with the tokenize() implementation itself, it
**   may abandon the tokenization and return any error code other than
**   DBSQL_OK or DBSQL_DONE.
**
**   If the tokenizer is registered using an fts5_tokenizer_v2 object,
**   then the tokenize() method has two additional arguments - locale
**   and n_locale. These specify the locale that the tokenizer should use
**   for the current request. If locale and n_locale are both 0, then the
**   tokenizer should use its default locale. Otherwise, locale points to
**   an n_locale byte buffer containing the name of the locale to use as utf-8 
**   text. locale is not nul-terminated.
**
** FTS5_TOKENIZER
**
** There is also an fts5_tokenizer object. This is an older, deprecated,
** version of fts5_tokenizer_v2. It is similar except that:
**
**  <ul>
**    <li> There is no "i_version" field, and
**    <li> The tokenize() method does not take a locale argument.
**  </ul>
**
** Legacy fts5_tokenizer tokenizers must be registered using the
** legacy create_tokenizer() function, instead of create_tokenizer_v2().
**
** Tokenizer implementations registered using either API may be retrieved
** using both find_tokenizer() and find_tokenizer_v2().
**
** SYNONYM SUPPORT
**
**   Custom tokenizers may also support synonyms. Consider a case in which a
**   user wishes to query for a phrase such as "first place". Using the 
**   built-in tokenizers, the FTS5 query 'first + place' will match instances
**   of "first place" within the document set, but not alternative forms
**   such as "1st place". In some applications, it would be better to match
**   all instances of "first place" or "1st place" regardless of which form
**   the user specified in the MATCH query text.
**
**   There are several ways to approach this in FTS5:
**
**   <ol><li> By mapping all synonyms to a single token. In this case, using
**            the above example, this means that the tokenizer returns the
**            same token for inputs "first" and "1st". Say that token is in
**            fact "first", so that when the user inserts the document "I won
**            1st place" entries are added to the index for tokens "i", "won",
**            "first" and "place". If the user then queries for '1st + place',
**            the tokenizer substitutes "first" for "1st" and the query works
**            as expected.
**
**       <li> By querying the index for all synonyms of each query term
**            separately. In this case, when tokenizing query text, the
**            tokenizer may provide multiple synonyms for a single term 
**            within the document. FTS5 then queries the index for each 
**            synonym individually. For example, faced with the query:
**
**   <codeblock>
**     ... MATCH 'first place'</codeblock>
**
**            the tokenizer offers both "1st" and "first" as synonyms for the
**            first token in the MATCH query and FTS5 effectively runs a query 
**            similar to:
**
**   <codeblock>
**     ... MATCH '(first OR 1st) place'</codeblock>
**
**            except that, for the purposes of auxiliary functions, the query
**            still appears to contain just two phrases - "(first OR 1st)" 
**            being treated as a single phrase.
**
**       <li> By adding multiple synonyms for a single term to the FTS index.
**            Using this method, when tokenizing document text, the tokenizer
**            provides multiple synonyms for each token. So that when a 
**            document such as "I won first place" is tokenized, entries are
**            added to the FTS index for "i", "won", "first", "1st" and
**            "place".
**
**            This way, even if the tokenizer does not provide synonyms
**            when tokenizing query text (it should not - to do so would be
**            inefficient), it doesn't matter if the user queries for 
**            'first + place' or '1st + place', as there are entries in the
**            FTS index corresponding to both forms of the first token.
**   </ol>
**
**   Whether it is parsing document or query text, any call to token that
**   specifies a <i>tflags</i> argument with the FTS5_TOKEN_COLOCATED bit
**   is considered to supply a synonym for the previous token. For example,
**   when parsing the document "I won first place", a tokenizer that supports
**   synonyms would call token() 5 times, as follows:
**
**   <codeblock>
**       token(ctx, 0, "i",                      1,  0,  1);
**       token(ctx, 0, "won",                    3,  2,  5);
**       token(ctx, 0, "first",                  5,  6, 11);
**       token(ctx, FTS5_TOKEN_COLOCATED, "1st", 3,  6, 11);
**       token(ctx, 0, "place",                  5, 12, 17);
**</codeblock>
**
**   It is an error to specify the FTS5_TOKEN_COLOCATED flag the first time
**   token() is called. Multiple synonyms may be specified for a single token
**   by making multiple calls to token(FTS5_TOKEN_COLOCATED) in sequence. 
**   There is no limit to the number of synonyms that may be provided for a
**   single token.
**
**   In many cases, method (1) above is the best approach. It does not add 
**   extra data to the FTS index or require FTS5 to query for multiple terms,
**   so it is efficient in terms of disk space and query speed. However, it
**   does not support prefix queries very well. If, as suggested above, the
**   token "first" is substituted for "1st" by the tokenizer, then the query:
**
**   <codeblock>
**     ... MATCH '1s*'</codeblock>
**
**   will not match documents that contain the token "1st" (as the tokenizer
**   will probably not map "1s" to any prefix of "first").
**
**   For full prefix support, method (3) may be preferred. In this case, 
**   because the index contains entries for both "first" and "1st", prefix
**   queries such as 'fi*' or '1s*' will match correctly. However, because
**   extra entries are added to the FTS index, this method uses more space
**   within the database.
**
**   Method (2) offers a midpoint between (1) and (3). Using this method,
**   a query such as '1s*' will match documents that contain the literal 
**   token "1st", but not "first" (assuming the tokenizer is not able to
**   provide synonyms for prefixes). However, a non-prefix query like '1st'
**   will match against "1st" and "first". This method does not require
**   extra disk space, as no extra entries are added to the FTS index. 
**   On the other hand, it may require more CPU cycles to run MATCH queries,
**   as separate queries of the FTS index are required for each synonym.
**
**   When using methods (2) or (3), it is important that the tokenizer only
**   provide synonyms when tokenizing document text (method (3)) or query
**   text (method (2)), not both. Doing so will not cause any errors, but is
**   inefficient.
*/
typedef struct fts5_tokenizer_t fts5_tokenizer_t;
typedef struct fts5_tokenizer_v2 fts5_tokenizer_v2;
struct fts5_tokenizer_v2 {
  int i_version;             /* Currently always 2 */

  int (*create)(void*, const char **args, int n_arg, fts5_tokenizer_t **out);
  void (*x_delete)(fts5_tokenizer_t*);
  int (*tokenize)(fts5_tokenizer_t*, 
      void *ctx,
      int flags,            /* Mask of FTS5_TOKENIZE_* flags */
      const char *text, int n_text, 
      const char *locale, int n_locale,
      int (*x_token)(
        void *ctx,         /* Copy of 2nd argument to tokenize() */
        int tflags,         /* Mask of FTS5_TOKEN_* flags */
        const char *token, /* Pointer to buffer containing token */
        int n_token,         /* Size of token in bytes */
        int i_start,         /* Byte offset of token within input text */
        int i_end            /* Byte offset of end of token within input text */
      )
  );
};

/*
** New code should use the fts5_tokenizer_v2 type to define tokenizer
** implementations. The following type is included for legacy applications
** that still use it.
*/
typedef struct fts5_tokenizer fts5_tokenizer;
struct fts5_tokenizer {
  int (*create)(void*, const char **args, int n_arg, fts5_tokenizer_t **out);
  void (*x_delete)(fts5_tokenizer_t*);
  int (*tokenize)(fts5_tokenizer_t*, 
      void *ctx,
      int flags,            /* Mask of FTS5_TOKENIZE_* flags */
      const char *text, int n_text, 
      int (*x_token)(
        void *ctx,         /* Copy of 2nd argument to tokenize() */
        int tflags,         /* Mask of FTS5_TOKEN_* flags */
        const char *token, /* Pointer to buffer containing token */
        int n_token,         /* Size of token in bytes */
        int i_start,         /* Byte offset of token within input text */
        int i_end            /* Byte offset of end of token within input text */
      )
  );
};


/* Flags that may be passed as the third argument to tokenize() */
#define FTS5_TOKENIZE_QUERY     0x0001
#define FTS5_TOKENIZE_PREFIX    0x0002
#define FTS5_TOKENIZE_DOCUMENT  0x0004
#define FTS5_TOKENIZE_AUX       0x0008

/* Flags that may be passed by the tokenizer implementation back to FTS5
** as the third argument to the supplied token callback. */
#define FTS5_TOKEN_COLOCATED    0x0001      /* Same position as prev. token */

/*
** END OF CUSTOM TOKENIZERS
*************************************************************************/

/*************************************************************************
** FTS5 EXTENSION REGISTRATION API
*/
typedef struct fts5_api fts5_api;
struct fts5_api {
  int i_version;                   /* Currently always set to 3 */

  /* Create a new tokenizer */
  int (*create_tokenizer)(
    fts5_api *api,
    const char *name,
    void *user_data,
    fts5_tokenizer *tokenizer,
    void (*destroy)(void*)
  );

  /* Find an existing tokenizer */
  int (*find_tokenizer)(
    fts5_api *api,
    const char *name,
    void **pp_user_data,
    fts5_tokenizer *tokenizer
  );

  /* Create a new auxiliary function */
  int (*create_function)(
    fts5_api *api,
    const char *name,
    void *user_data,
    fts5_extension_function function,
    void (*destroy)(void*)
  );

  /* APIs below this point are only available if i_version>=3 */

  /* Create a new tokenizer */
  int (*create_tokenizer_v2)(
    fts5_api *api,
    const char *name,
    void *user_data,
    fts5_tokenizer_v2 *tokenizer,
    void (*destroy)(void*)
  );

  /* Find an existing tokenizer */
  int (*find_tokenizer_v2)(
    fts5_api *api,
    const char *name,
    void **pp_user_data,
    fts5_tokenizer_v2 **pp_tokenizer
  );
};

/*
** END OF REGISTRATION API
*************************************************************************/

#ifdef __cplusplus
}  /* end of the 'extern "C"' block */
#endif

#endif /* _FTS5_H */
