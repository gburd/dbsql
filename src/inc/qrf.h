/*
** 2025-10-20
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Header file for the Query Result-Format or "qrf" utility library for
** dbsql.  See the README.md documentation for additional information.
*/
#ifndef DBSQL_QRF_H
#define DBSQL_QRF_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdlib.h>
#include "dbsql.h"

/*
** Specification used by clients to define the output format they want
*/
typedef struct dbsql_qrf_spec dbsql_qrf_spec;
struct dbsql_qrf_spec {
  unsigned char i_version;     /* Version number of this structure */
  unsigned char style;       /* Formatting style.  "box", "csv", etc... */
  unsigned char esc;         /* How to escape control characters in text */
  unsigned char text;        /* Quoting style for text */
  unsigned char title;       /* Quating style for the text of column names */
  unsigned char blob;        /* Quoting style for BLOBs */
  unsigned char titles;      /* True to show column names */
  unsigned char word_wrap;    /* Try to wrap on word boundaries */
  unsigned char text_jsonb;   /* Render JSONB blobs as JSON text */
  unsigned char dflt_align;   /* Default alignment, no covered by alignments */
  unsigned char title_align;  /* Alignment for column headers */
  unsigned char split_column; /* Wrap single-column output into many columns */
  unsigned char border;      /* Show outer border in Box and table_t styles */
  short int n_wrap;            /* Wrap columns wider than this */
  short int n_screen_width;     /* Maximum overall table width */
  short int n_line_limit;       /* Maximum number of lines for any row */
  short int n_title_limit;      /* Maximum number of characters in a title */
  unsigned int n_multi_insert;  /* Add rows to one INSERT until size exceeds */
  int n_char_limit;             /* Maximum number of characters in a cell */
  int n_width;                 /* Number of entries in widths[] */
  int n_align;                 /* Number of entries in alignments[] */
  short int *widths;          /* column_t widths */
  unsigned char *aligns;      /* column_t alignments */
  char *column_sep;           /* Alternative column separator */
  char *row_sep;              /* Alternative row separator */
  char *table_name;           /* Output table name */
  char *null;                /* Rendering of NULL */
  char *(*render)(void*,dbsql_value_t*);           /* Render a value */
  int (*x_write)(void*,const char*,int64_t);   /* Write output */
  void *render_arg;           /* First argument to the render callback */
  void *write_arg;            /* First argument to the x_write callback */
  char **output;            /* Storage location for output string */
  /* Additional fields may be added in the future */
};

/*
** Interfaces
*/
int dbsql_format_query_result(
  dbsql_stmt_t *stmt,             /* SQL statement to run */
  const dbsql_qrf_spec *spec,   /* Result format specification */
  char **err_p                     /* OUT: Write error message here */
);

/*
** Range of values for dbsql_qrf_spec.widths[] entries and for
** dbsql_qrf_spec.mx_col_width and .n_screen_width
*/
#define QRF_MAX_WIDTH    10000
#define QRF_MIN_WIDTH    0

/*
** Output styles:
*/
#define QRF_STYLE_Auto      0 /* Choose a style automatically */
#define QRF_STYLE_Box       1 /* Unicode box-drawing characters */
#define QRF_STYLE_Column    2 /* One record per line in neat columns */
#define QRF_STYLE_Count     3 /* Output only a count of the rows of output */
#define QRF_STYLE_Csv       4 /* Comma-separated-value */
#define QRF_STYLE_Eqp       5 /* Format EXPLAIN QUERY PLAN output */
#define QRF_STYLE_Explain   6 /* EXPLAIN output */
#define QRF_STYLE_Html      7 /* Generate an XHTML table */
#define QRF_STYLE_Insert    8 /* Generate SQL "insert" statements */
#define QRF_STYLE_Json      9 /* Output is a list of JSON objects */
#define QRF_STYLE_JObject  10 /* Independent JSON objects for each row */
#define QRF_STYLE_Line     11 /* One column per line. */
#define QRF_STYLE_List     12 /* One record per line with a separator */
#define QRF_STYLE_Markdown 13 /* Markdown formatting */
#define QRF_STYLE_Off      14 /* No query output shown */
#define QRF_STYLE_Quote    15 /* SQL-quoted, comma-separated */
#define QRF_STYLE_Stats    16 /* EQP-like output but with performance stats */
#define QRF_STYLE_StatsEst 17 /* EQP-like output with planner estimates */
#define QRF_STYLE_StatsVm  18 /* EXPLAIN-like output with performance stats */
#define QRF_STYLE_Table    19 /* __my_sql-style table formatting */

/*
** Quoting styles for text.
** Allowed values for dbsql_qrf_spec.text
*/
#define QRF_TEXT_Auto    0 /* Choose text encoding automatically */
#define QRF_TEXT_Plain   1 /* Literal text */
#define QRF_TEXT_Sql     2 /* Quote as an SQL literal */
#define QRF_TEXT_Csv     3 /* CSV-style quoting */
#define QRF_TEXT_Html    4 /* HTML-style quoting */
#define QRF_TEXT_Tcl     5 /* C/Tcl quoting */
#define QRF_TEXT_Json    6 /* JSON quoting */
#define QRF_TEXT_Relaxed 7 /* Relaxed SQL quoting */

/*
** Quoting styles for BLOBs
** Allowed values for dbsql_qrf_spec.blob
*/
#define QRF_BLOB_Auto    0 /* Determine BLOB quoting using text */
#define QRF_BLOB_Text    1 /* Display content exactly as it is */
#define QRF_BLOB_Sql     2 /* Quote as an SQL literal */
#define QRF_BLOB_Hex     3 /* Hexadecimal representation */
#define QRF_BLOB_Tcl     4 /* "\000" notation */
#define QRF_BLOB_Json    5 /* A JSON string */
#define QRF_BLOB_Size    6 /* Display the blob size only */

/*
** Control-character escape modes.
** Allowed values for dbsql_qrf_spec.esc
*/
#define QRF_ESC_Auto    0 /* Choose the ctrl-char escape automatically */
#define QRF_ESC_Off     1 /* Do not escape control characters */
#define QRF_ESC_Ascii   2 /* Unix-style escapes.  Ex: U+0007 shows ^G */
#define QRF_ESC_Symbol  3 /* Unicode escapes. Ex: U+0007 shows U+2407 */

/*
** Allowed values for "boolean" fields, such as "column_names", "word_wrap",
** and "text_jsonb".  There is an extra "auto" variants so these are actually
** tri-state settings, not booleans.
*/
#define QRF_SW_Auto     0 /* Let QRF choose the best value */
#define QRF_SW_Off      1 /* This setting is forced off */
#define QRF_SW_On       2 /* This setting is forced on */
#define QRF_Auto        0 /* Alternate spelling for QRF_*_Auto */
#define QRF_No          1 /* Alternate spelling for QRF_SW_Off */
#define QRF_Yes         2 /* Alternate spelling for QRF_SW_On */

/*
** Possible alignment values alignment settings
**
**                             Horizontal   Vertial
**                             ----------   --------  */
#define QRF_ALIGN_Auto    0 /*   auto        auto     */
#define QRF_ALIGN_Left    1 /*   left        auto     */
#define QRF_ALIGN_Center  2 /*   center      auto     */
#define QRF_ALIGN_Right   3 /*   right       auto     */
#define QRF_ALIGN_Top     4 /*   auto        top      */
#define QRF_ALIGN_NW      5 /*   left        top      */
#define QRF_ALIGN_N       6 /*   center      top      */
#define QRF_ALIGN_NE      7 /*   right       top      */
#define QRF_ALIGN_Middle  8 /*   auto        middle   */
#define QRF_ALIGN_W       9 /*   left        middle   */
#define QRF_ALIGN_C      10 /*   center      middle   */
#define QRF_ALIGN_E      11 /*   right       middle   */
#define QRF_ALIGN_Bottom 12 /*   auto        bottom   */
#define QRF_ALIGN_SW     13 /*   left        bottom   */
#define QRF_ALIGN_S      14 /*   center      bottom   */
#define QRF_ALIGN_SE     15 /*   right       bottom   */
#define QRF_ALIGN_HMASK   3 /* Horizontal alignment mask */
#define QRF_ALIGN_VMASK  12 /* Vertical alignment mask */

/*
** Auxiliary routines contined within this module that might be useful
** in other contexts, and which are therefore exported.
*/
/*
** Return an estimate of the width, in columns, for the single Unicode
** character c.  For normal characters, the answer is always 1.  But the
** estimate might be 0 or 2 for zero-width and double-width characters.
**
** Different devices display unicode using different widths.  So
** it is impossible to know that true display width with 100% accuracy.
** Inaccuracies in the width estimates might cause columns to be misaligned.
** Unfortunately, there is nothing we can do about that.
*/
int dbsql_qrf_wcwidth(int c);

/*
** Return an estimate of the number of display columns used by the
** string in the argument.  The width of individual characters is
** determined as for dbsql_qrf_wcwidth().  VT100 escape code sequences
** are assigned a width of zero.
*/
size_t dbsql_qrf_wcswidth(const char*);


#ifdef __cplusplus
}
#endif
#endif /* !defined(DBSQL_QRF_H) */
