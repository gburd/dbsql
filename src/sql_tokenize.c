/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * An tokenizer for SQL
 *
 * This file contains C code that splits an SQL input string up into
 * individual tokens and sends those tokens one-by-one over to the
 * parser for analysis.
 *
 * Updated to support SQLite 3.52.0 SQL keywords including window
 * functions, CTEs, UPSERT, generated columns, RETURNING, and
 * JSON pointer operators.
 */

#include "inc/dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#include <stdlib.h>
#endif

#include "dbsql_int.h"

/*
 * All the keywords of the SQL language are stored as in a hash
 * table composed of instances of the following structure.
 */
typedef struct keyword keyword_t;
struct keyword {
	char *name;          /* The keyword name */
	u_int16_t token_type; /* token value for this keyword */
	u_int8_t len;        /* Length of this keyword */
	u_int16_t inext;     /* Index in sql_tokens_table[] of next
				with same hash */
};

/*
 * These are the keywords.  Updated to match SQLite 3.52.0 keyword
 * set including window functions (OVER, PARTITION, RANGE, ROWS,
 * GROUPS, etc.), CTEs (WITH, RECURSIVE), UPSERT (DO, NOTHING),
 * generated columns (GENERATED, ALWAYS, STORED, VIRTUAL),
 * RETURNING, and other modern SQL features.
 */
static keyword_t sql_tokens_table[] = {
  { "ABORT",             TK_ABORT,        },
  { "ACTION",            TK_ACTION,       },
  { "ADD",               TK_ADD,          },
  { "AFTER",             TK_AFTER,        },
  { "ALL",               TK_ALL,          },
  { "ALTER",             TK_ALTER,        },
  { "ALWAYS",            TK_ALWAYS,       },
  { "ANALYZE",           TK_ANALYZE,      },
  { "AND",               TK_AND,          },
  { "AS",                TK_AS,           },
  { "ASC",               TK_ASC,          },
  { "ATTACH",            TK_ATTACH,       },
  { "AUTOINCREMENT",     TK_AUTOINCR,     },
  { "BEFORE",            TK_BEFORE,       },
  { "BEGIN",             TK_BEGIN,        },
  { "BETWEEN",           TK_BETWEEN,      },
  { "BY",                TK_BY,           },
  { "CASCADE",           TK_CASCADE,      },
  { "CASE",              TK_CASE,         },
  { "CAST",              TK_CAST,         },
  { "CHECK",             TK_CHECK,        },
  { "CLUSTER",           TK_CLUSTER,      },
  { "COLLATE",           TK_COLLATE,      },
  { "COLUMN",            TK_COLUMNKW,     },
  { "COMMIT",            TK_COMMIT,       },
  { "CONFLICT",          TK_CONFLICT,     },
  { "CONSTRAINT",        TK_CONSTRAINT,   },
  { "COPY",              TK_COPY,         },
  { "CREATE",            TK_CREATE,       },
  { "CROSS",             TK_JOIN_KW,      },
  { "CURRENT",           TK_CURRENT,      },
  { "CURRENT_DATE",      TK_CTIME_KW,     },
  { "CURRENT_TIME",      TK_CTIME_KW,     },
  { "CURRENT_TIMESTAMP", TK_CTIME_KW,     },
  { "DATABASE",          TK_DATABASE,     },
  { "DEFAULT",           TK_DEFAULT,      },
  { "DEFERRED",          TK_DEFERRED,     },
  { "DEFERRABLE",        TK_DEFERRABLE,   },
  { "DELETE",            TK_DELETE,       },
  { "DELIMITERS",        TK_DELIMITERS,   },
  { "DESC",              TK_DESC,         },
  { "DETACH",            TK_DETACH,       },
  { "DISTINCT",          TK_DISTINCT,     },
  { "DO",                TK_DO,           },
  { "DROP",              TK_DROP,         },
  { "EACH",              TK_EACH,         },
  { "ELSE",              TK_ELSE,         },
  { "END",               TK_END,          },
  { "ESCAPE",            TK_ESCAPE,       },
  { "EXCEPT",            TK_EXCEPT,       },
  { "EXCLUDE",           TK_EXCLUDE,      },
  { "EXCLUSIVE",         TK_EXCLUSIVE,    },
  { "EXISTS",            TK_EXISTS,       },
  { "EXPLAIN",           TK_EXPLAIN,      },
  { "FAIL",              TK_FAIL,         },
  { "FILTER",            TK_FILTER,       },
  { "FIRST",             TK_FIRST,        },
  { "FOLLOWING",         TK_FOLLOWING,    },
  { "FOR",               TK_FOR,          },
  { "FOREIGN",           TK_FOREIGN,      },
  { "FROM",              TK_FROM,         },
  { "FULL",              TK_JOIN_KW,      },
  { "GENERATED",         TK_GENERATED,    },
  { "GLOB",              TK_LIKE_KW,      },
  { "GROUP",             TK_GROUP,        },
  { "GROUPS",            TK_GROUPS,       },
  { "HAVING",            TK_HAVING,       },
  { "IF",                TK_IF,           },
  { "IGNORE",            TK_IGNORE,       },
  { "IMMEDIATE",         TK_IMMEDIATE,    },
  { "IN",                TK_IN,           },
  { "INDEX",             TK_INDEX,        },
  { "INDEXED",           TK_INDEXED,      },
  { "INITIALLY",         TK_INITIALLY,    },
  { "INNER",             TK_JOIN_KW,      },
  { "INSERT",            TK_INSERT,       },
  { "INSTEAD",           TK_INSTEAD,      },
  { "INTERSECT",         TK_INTERSECT,    },
  { "INTO",              TK_INTO,         },
  { "IS",                TK_IS,           },
  { "ISNULL",            TK_ISNULL,       },
  { "JOIN",              TK_JOIN,         },
  { "KEY",               TK_KEY,          },
  { "LAST",              TK_LAST,         },
  { "LEFT",              TK_JOIN_KW,      },
  { "LIKE",              TK_LIKE_KW,      },
  { "LIMIT",             TK_LIMIT,        },
  { "MATCH",             TK_MATCH,        },
  { "MATERIALIZED",      TK_MATERIALIZED, },
  { "NATURAL",           TK_JOIN_KW,      },
  { "NO",                TK_NO,           },
  { "NOT",               TK_NOT,          },
  { "NOTHING",           TK_NOTHING,      },
  { "NOTNULL",           TK_NOTNULL,      },
  { "NULL",              TK_NULL,         },
  { "NULLS",             TK_NULLS,        },
  { "OF",                TK_OF,           },
  { "OFFSET",            TK_OFFSET,       },
  { "ON",                TK_ON,           },
  { "OR",                TK_OR,           },
  { "ORDER",             TK_ORDER,        },
  { "OTHERS",            TK_OTHERS,       },
  { "OUTER",             TK_JOIN_KW,      },
  { "OVER",              TK_OVER,         },
  { "PARTITION",         TK_PARTITION,    },
  { "PLAN",              TK_PLAN,         },
  { "PRAGMA",            TK_PRAGMA,       },
  { "PRECEDING",         TK_PRECEDING,    },
  { "PRIMARY",           TK_PRIMARY,      },
  { "QUERY",             TK_QUERY,        },
  { "RAISE",             TK_RAISE,        },
  { "RANGE",             TK_RANGE,        },
  { "RECURSIVE",         TK_RECURSIVE,    },
  { "REFERENCES",        TK_REFERENCES,   },
  { "REGEXP",            TK_LIKE_KW,      },
  { "REINDEX",           TK_REINDEX,      },
  { "RELEASE",           TK_RELEASE,      },
  { "RENAME",            TK_RENAME,       },
  { "REPLACE",           TK_REPLACE,      },
  { "RESTRICT",          TK_RESTRICT,     },
  { "RETURNING",         TK_RETURNING,    },
  { "RIGHT",             TK_JOIN_KW,      },
  { "ROLLBACK",          TK_ROLLBACK,     },
  { "ROW",               TK_ROW,          },
  { "ROWS",              TK_ROWS,         },
  { "SAVEPOINT",         TK_SAVEPOINT,    },
  { "SELECT",            TK_SELECT,       },
  { "SET",               TK_SET,          },
  { "STATEMENT",         TK_STATEMENT,    },
  { "TABLE",             TK_TABLE,        },
  { "TEMP",              TK_TEMP,         },
  { "TEMPORARY",         TK_TEMP,         },
  { "THEN",              TK_THEN,         },
  { "TIES",              TK_TIES,         },
  { "TO",                TK_TO,           },
  { "TRANSACTION",       TK_TRANSACTION,  },
  { "TRIGGER",           TK_TRIGGER,      },
  { "UNBOUNDED",         TK_UNBOUNDED,    },
  { "UNION",             TK_UNION,        },
  { "UNIQUE",            TK_UNIQUE,       },
  { "UPDATE",            TK_UPDATE,       },
  { "USING",             TK_USING,        },
  { "VACUUM",            TK_VACUUM,       },
  { "VALUES",            TK_VALUES,       },
  { "VIEW",              TK_VIEW,         },
  { "VIRTUAL",           TK_VIRTUAL,      },
  { "WHEN",              TK_WHEN,         },
  { "WHERE",             TK_WHERE,        },
  { "WINDOW",            TK_WINDOW,       },
  { "WITH",              TK_WITH,         },
  { "WITHOUT",           TK_WITHOUT,      },
};

/*
 * This is the hash table.  Sized to accommodate the expanded
 * keyword set with reasonable collision rates.
 */
#define KEY_HASH_SIZE 199
static u_int16_t ai_table[KEY_HASH_SIZE];


/*
 * __get_keyword_code --
 *	This function looks up an identifier to determine if it is a
 *	keyword.  If it is a keyword, the token code of that keyword is
 *	returned.  If the input is not a keyword, TK_ID is returned.
 *
 * PUBLIC: int __get_keyword_code(const char *, int);
 */
int
__get_keyword_code(const char *z, int n)
{
	int h, i;
	int nk;
	keyword_t *p;
	static char need_init = 1;
	if (need_init) {
		/* Initialize the keyword hash table */
		need_init = 0;
		nk = sizeof(sql_tokens_table) /
			sizeof(sql_tokens_table[0]);
		for (i = 0; i < nk; i++) {
			sql_tokens_table[i].len =
				strlen(sql_tokens_table[i].name);
			h = __hash_ignore_case(
				sql_tokens_table[i].name,
				sql_tokens_table[i].len);
			h %= KEY_HASH_SIZE;
			sql_tokens_table[i].inext = ai_table[h];
			ai_table[h] = i + 1;
		}
	}
	h = __hash_ignore_case(z, n) % KEY_HASH_SIZE;
	for (i = ai_table[h]; i; i = p->inext) {
		p = &sql_tokens_table[i - 1];
		if (p->len == n &&
		    strncasecmp(p->name, z, n) == 0) {
			return p->token_type;
		}
	}
	return TK_ID;
}


/*
 * If X is a character that can be used in an identifier and
 * X&0x80==0 then id_char_p[X] will be 1.  If X&0x80==0x80 then
 * X is always an identifier character.  (Hence all UTF-8
 * characters can be part of an identifier).  id_char_p[X] will
 * be 0 for every character in the lower 128 ASCII characters
 * that cannot be used as part of an identifier.
 *
 * In this implementation, an identifier can be a string of
 * alphabetic characters, digits, and "_" plus any character
 * with the high-order bit set.  The latter rule means that
 * any sequence of UTF-8 characters or characters taken from
 * an extended ISO8859 character set can form an identifier.
 */
static const char id_char_p[] = {
/* x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 1x */
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 2x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  /* 3x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 4x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,  /* 5x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 6x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,  /* 7x */
};


/* Forward declarations */
static int __get_token(const unsigned char *, int *);

#ifndef DBSQL_OMIT_WINDOWFUNC
/*
 * __skip_token --
 *	Return the id of the next token in string (*pz).  Before
 *	returning, set (*pz) to point to the byte following the
 *	parsed token.  Skips whitespace and comment tokens.
 *
 * STATIC: static int __skip_token(const unsigned char **);
 */
static int
__skip_token(const unsigned char **pz)
{
	const unsigned char *z = *pz;
	int t;
	do {
		z += __get_token(z, &t);
	} while (t == TK_SPACE || t == TK_COMMENT);
	if (t == TK_ID
	    || t == TK_STRING
	    || t == TK_JOIN_KW
	    || t == TK_WINDOW
	    || t == TK_OVER) {
		/* TODO: Add fallback token check: __sql_parser_fallback(t) == TK_ID */
		t = TK_ID;
	}
	*pz = z;
	return t;
}

/*
 * __analyze_window_keyword --
 *	Determine if the WINDOW token should be treated as a keyword
 *	or an SQL identifier.  WINDOW is treated as a keyword if the
 *	following token is an identifier and the token after that is
 *	AS.
 *
 * STATIC: static int __analyze_window_keyword(const unsigned char *);
 */
static int
__analyze_window_keyword(const unsigned char *z)
{
	int t;
	t = __skip_token(&z);
	if (t != TK_ID)
		return TK_ID;
	t = __skip_token(&z);
	if (t != TK_AS)
		return TK_ID;
	return TK_WINDOW;
}

/*
 * __analyze_over_keyword --
 *	Determine if the OVER token should be treated as a keyword
 *	or an SQL identifier.  OVER is a keyword if the previous
 *	token was TK_RP and the next token is either TK_LP or an
 *	identifier.
 *
 * STATIC: static int __analyze_over_keyword(
 *             const unsigned char *, int);
 */
static int
__analyze_over_keyword(const unsigned char *z, int last_token)
{
	if (last_token == TK_RP) {
		int t = __skip_token(&z);
		if (t == TK_LP || t == TK_ID)
			return TK_OVER;
	}
	return TK_ID;
}

/*
 * __analyze_filter_keyword --
 *	Determine if the FILTER token should be treated as a keyword
 *	or an SQL identifier.  FILTER is a keyword if the previous
 *	token was TK_RP and the next token is TK_LP.
 *
 * STATIC: static int __analyze_filter_keyword(
 *             const unsigned char *, int);
 */
static int
__analyze_filter_keyword(const unsigned char *z, int last_token)
{
	if (last_token == TK_RP && __skip_token(&z) == TK_LP) {
		return TK_FILTER;
	}
	return TK_ID;
}
#endif /* DBSQL_OMIT_WINDOWFUNC */


/*
 * __get_token --
 *	Return the length of the token that begins at z[0].
 *	Store the token type in *token_type before returning.
 *
 *	Updated to handle hex integer literals (0x...), blob
 *	literals (x'...'), backtick-quoted identifiers, named
 *	SQL variables ($name, @name, :name, #name), and the
 *	JSON pointer operator (-> and ->>).
 *
 * STATIC: static int __get_token(const unsigned char *, int *);
 */
static int
__get_token(const unsigned char *z, int *token_type)
{
	int i, c, delim;

	switch (*z) {
	case ' ':  /* FALLTHROUGH */
	case '\t': /* FALLTHROUGH */
	case '\n': /* FALLTHROUGH */
	case '\f': /* FALLTHROUGH */
	case '\r':
		i = 1;
		while (isspace(z[i])) {
			i++;
		}
		*token_type = TK_SPACE;
		return i;
	case '-':
		if (z[1] == '-') {
			i = 2;
			while (z[i] && z[i] != '\n') {
				i++;
			}
			*token_type = TK_COMMENT;
			return i;
		} else if (z[1] == '>') {
			*token_type = TK_PTR;
			return 2 + (z[2] == '>');
		}
		*token_type = TK_MINUS;
		return 1;
	case '(':
		*token_type = TK_LP;
		return 1;
	case ')':
		*token_type = TK_RP;
		return 1;
	case ';':
		*token_type = TK_SEMI;
		return 1;
	case '+':
		*token_type = TK_PLUS;
		return 1;
	case '*':
		*token_type = TK_STAR;
		return 1;
	case '/':
		if (z[1] != '*' || z[2] == 0) {
			*token_type = TK_SLASH;
			return 1;
		}
		i = 3;
		c = z[2];
		while ((c != '*' || z[i] != '/') && (c = z[i]) != 0) {
			i++;
		}
		if (c)
			i++;
		*token_type = TK_COMMENT;
		return i;
	case '%':
		*token_type = TK_REM;
		return 1;
	case '=':
		*token_type = TK_EQ;
		return 1 + (z[1] == '=');
	case '<':
		if (z[1] == '=') {
			*token_type = TK_LE;
			return 2;
		} else if (z[1] == '>') {
			*token_type = TK_NE;
			return 2;
		} else if (z[1] == '<') {
			*token_type = TK_LSHIFT;
			return 2;
		} else {
			*token_type = TK_LT;
			return 1;
		}
	case '>':
		if (z[1] == '=') {
			*token_type = TK_GE;
			return 2;
		} else if (z[1] == '>') {
			*token_type = TK_RSHIFT;
			return 2;
		} else {
			*token_type = TK_GT;
			return 1;
		}
	case '!':
		if (z[1] != '=') {
			*token_type = TK_ILLEGAL;
			return 1;
		} else {
			*token_type = TK_NE;
			return 2;
		}
	case '|':
		if (z[1] != '|') {
			*token_type = TK_BITOR;
			return 1;
		} else {
			*token_type = TK_CONCAT;
			return 2;
		}
	case ',':
		*token_type = TK_COMMA;
		return 1;
	case '&':
		*token_type = TK_BITAND;
		return 1;
	case '~':
		*token_type = TK_BITNOT;
		return 1;
	case '`':  /* FALLTHROUGH */
	case '\'': /* FALLTHROUGH */
	case '"':
		delim = z[0];
		for (i = 1; (c = z[i]) != 0; i++) {
			if (c == delim) {
				if (z[i + 1] == delim) {
					i++;
				} else {
					break;
				}
			}
		}
		if (c == '\'') {
			*token_type = TK_STRING;
			return i + 1;
		} else if (c != 0) {
			*token_type = TK_ID;
			return i + 1;
		} else {
			*token_type = TK_ILLEGAL;
			return i;
		}
	case '.':
		if (!isdigit(z[1])) {
			*token_type = TK_DOT;
			return 1;
		}
		/*
		 * If the next character is a digit, this is a
		 * floating point number that begins with ".".
		 * Fall through into the next case.
		 */
		/* FALLTHROUGH */
	case '0':
	case '1': /* FALLTHROUGH */
	case '2': /* FALLTHROUGH */
	case '3': /* FALLTHROUGH */
	case '4': /* FALLTHROUGH */
	case '5': /* FALLTHROUGH */
	case '6': /* FALLTHROUGH */
	case '7': /* FALLTHROUGH */
	case '8': /* FALLTHROUGH */
	case '9':
		*token_type = TK_INTEGER;
		if (z[0] == '0' && (z[1] == 'x' || z[1] == 'X')
		    && isxdigit(z[2])) {
			for (i = 3; isxdigit(z[i]); i++) {}
		} else {
			for (i = 0; isdigit(z[i]); i++) {}
			if (z[i] == '.' && isdigit(z[i + 1])) {
				i += 2;
				while (isdigit(z[i])) {
					i++;
				}
				*token_type = TK_FLOAT;
			}
			if ((z[i] == 'e' || z[i] == 'E') &&
			    (isdigit(z[i + 1]) ||
			     ((z[i + 1] == '+' || z[i + 1] == '-')
			      && isdigit(z[i + 2])))) {
				i += 2;
				while (isdigit(z[i])) {
					i++;
				}
				*token_type = TK_FLOAT;
			}
		}
		while (((z[i] & 0x80) != 0) || id_char_p[z[i]]) {
			*token_type = TK_ILLEGAL;
			i++;
		}
		return i;
	case '[':
		for (i = 1, c = z[0]; c != ']' && (c = z[i]) != 0; i++) {}
		*token_type = (c == ']') ? TK_ID : TK_ILLEGAL;
		return i;
	case '?':
		*token_type = TK_VARIABLE;
		for (i = 1; isdigit(z[i]); i++) {}
		return i;
	case '$':
	case '@':
	case ':':
	case '#': {
		int64_t n = 0;
		*token_type = TK_VARIABLE;
		for (i = 1; (c = z[i]) != 0; i++) {
			if (((c & 0x80) != 0) || id_char_p[c]) {
				n++;
			} else {
				break;
			}
		}
		if (n == 0)
			*token_type = TK_ILLEGAL;
		return i;
	}
	default:
		if (*z == 'x' || *z == 'X') {
			if (z[1] == '\'') {
				*token_type = TK_BLOB;
				for (i = 2; isxdigit(z[i]); i++) {}
				if (z[i] != '\'' || i % 2) {
					*token_type = TK_ILLEGAL;
					while (z[i] && z[i] != '\'') {
						i++;
					}
				}
				if (z[i])
					i++;
				return i;
			}
		}
		if ((*z & 0x80) == 0 && !id_char_p[*z]) {
			break;
		}
		i = 1;
		while ((z[i] & 0x80) != 0 || id_char_p[z[i]]) {
			i++;
		}
		*token_type = __get_keyword_code((char *)z, i);
		return i;
	}
	*token_type = TK_ILLEGAL;
	return 1;
}

/*
 * __parser_malloc_wrapper --
 *	Wrapper for __dbsql_calloc that matches the parser allocator signature.
 *	The Lemon parser expects malloc-style allocation but we need calloc
 *	to zero the parser stack, preventing invalid pointer frees during
 *	cleanup of uninitialized stack entries.
 */
static int
__parser_malloc_wrapper(DBSQL *dbp, size_t size, void *storep)
{
	/* Use calloc (1, size) to zero-initialize the memory */
	return __dbsql_calloc(dbp, 1, size, storep);
}

/*
 * __run_sql_parser --
 *	Run the parser on the given SQL string.  The parser structure
 *	is passed in.  A DBSQL_ status code is returned.
 *
 *	Updated to handle window function keyword disambiguation
 *	(WINDOW, OVER, FILTER) via context-sensitive analysis.
 *
 * PUBLIC: int __run_sql_parser(parser_t *, const char *, char **);
 */
int
__run_sql_parser(parser_t *parser, const char *sql, char **err_msgs)
{
	int nerr = 0;
	int i;
	void *engine;
	int token_type;
	int last_token_parsed = -1;
	DBSQL *dbp = parser->db;
	extern void *__sql_parser_alloc(
		DBSQL *, int(*)(DBSQL *, size_t, void *));
	extern void __sql_parser_free(
		DBSQL *, void *, void(*)(DBSQL *, void *));
	extern int __sql_parser(
		void *, int, token_t, parser_t *);

	dbp->flags &= ~DBSQL_Interrupt;
	parser->rc = DBSQL_SUCCESS;
	i = 0;
	/* Use wrapper that zeros memory to prevent invalid frees of uninitialized pointers */
	engine = __sql_parser_alloc(dbp, __parser_malloc_wrapper);
	if (engine == 0) {
		__str_append(err_msgs, "out of memory", (char *)0);
		return 1;
	}
	parser->sLastToken.dyn = 0;
	parser->zTail = sql;
	while (parser->rc == DBSQL_SUCCESS && sql[i] != 0) {
		DBSQL_ASSERT(i >= 0);
		parser->sLastToken.z = &sql[i];
		DBSQL_ASSERT(parser->sLastToken.dyn == 0);
		parser->sLastToken.n = __get_token(
			(unsigned char *)&sql[i], &token_type);
		i += parser->sLastToken.n;
		switch (token_type) {
		case TK_SPACE: /* FALLTHROUGH */
		case TK_COMMENT:
			if ((dbp->flags & DBSQL_Interrupt) != 0) {
				parser->rc = DBSQL_INTERRUPTED;
				__str_append(err_msgs, "interrupt",
					     (char *)0);
				goto abort_parse;
			}
			break;
#ifndef DBSQL_OMIT_WINDOWFUNC
		case TK_WINDOW: {
			int n = parser->sLastToken.n;
			DBSQL_ASSERT(n == 6);
			token_type = __analyze_window_keyword(
				(const unsigned char *)&sql[i]);
			if (token_type == TK_ID)
				goto default_token;
			break;
		}
		case TK_OVER: {
			int n = parser->sLastToken.n;
			DBSQL_ASSERT(n == 4);
			token_type = __analyze_over_keyword(
				(const unsigned char *)&sql[i],
				last_token_parsed);
			if (token_type == TK_ID)
				goto default_token;
			break;
		}
		case TK_FILTER: {
			int n = parser->sLastToken.n;
			DBSQL_ASSERT(n == 6);
			token_type = __analyze_filter_keyword(
				(const unsigned char *)&sql[i],
				last_token_parsed);
			if (token_type == TK_ID)
				goto default_token;
			break;
		}
#endif /* DBSQL_OMIT_WINDOWFUNC */
		case TK_ILLEGAL:
			__str_nappend(err_msgs,
				"unrecognized token: \"",
				-1, parser->sLastToken.z,
				parser->sLastToken.n,
				"\"", 1, NULL);
			nerr++;
			goto abort_parse;
		case TK_SEMI:
			parser->zTail = &sql[i];
			/* FALLTHROUGH */
		default:
		default_token:
			__sql_parser(engine, token_type,
				parser->sLastToken, parser);
			last_token_parsed = token_type;
			if (parser->rc != DBSQL_SUCCESS) {
				goto abort_parse;
			}
			break;
		}
	}
  abort_parse:
	if (sql[i] == 0 && nerr == 0
	    && parser->rc == DBSQL_SUCCESS) {
		if (last_token_parsed != TK_SEMI) {
			__sql_parser(engine, TK_SEMI,
				parser->sLastToken, parser);
			parser->zTail = &sql[i];
		}
		__sql_parser(engine, 0, parser->sLastToken,
			parser);
	}
	__sql_parser_free(dbp, engine, __dbsql_free);
	if (parser->rc != DBSQL_SUCCESS
	    && parser->rc != DBSQL_DONE
	    && parser->zErrMsg == 0) {
		__str_append(&parser->zErrMsg,
			     dbsql_strerror(parser->rc), (char *)0);
	}
	if (parser->zErrMsg) {
		if (err_msgs && *err_msgs == 0) {
			*err_msgs = parser->zErrMsg;
		} else {
			__dbsql_free(dbp, parser->zErrMsg);
		}
		parser->zErrMsg = 0;
		if (!nerr)
			nerr++;
	}
	if (parser->pVdbe
	    && (parser->useCallback || parser->n_errors > 0)) {
		__vdbe_delete(parser->pVdbe);
		parser->pVdbe = 0;
	}
	if (parser->pNewTable) {
		__vdbe_delete_table(parser->db,
			parser->pNewTable);
		parser->pNewTable = 0;
	}
	if (parser->pNewTrigger) {
		__vdbe_delete_trigger(parser->pNewTrigger);
		parser->pNewTrigger = 0;
	}
	if (nerr > 0 &&
	    (parser->rc == DBSQL_SUCCESS
	     || parser->rc == DBSQL_DONE)) {
		parser->rc = DBSQL_ERROR;
	}
	return nerr;
}

/*
 * Token types used by the dbsql_complete_stmt() routine.  See the
 * header comments on that procedure for additional information.
 */
#define tkEXPLAIN 0
#define tkCREATE  1
#define tkTEMP    2
#define tkTRIGGER 3
#define tkEND     4
#define tkSEMI    5
#define tkWS      6
#define tkOTHER   7

/*
 * dbsql_complete_stmt --
 *
 *	Return TRUE if the given SQL string ends in a semicolon.
 *
 *	Special handling is require for CREATE TRIGGER statements.
 *	Whenever the CREATE TRIGGER keywords are seen, the statement
 *	must end with ";END;".
 *
 *	This implementation uses a state machine with 7 states:
 *
 *   (0) START     At the beginning or end of an SQL statement.
 *                 This routine returns 1 if it ends in the START
 *                 state and 0 if it ends in any other state.
 *
 *   (1) EXPLAIN   The keyword EXPLAIN has been seen at the
 *                 beginning of a statement.
 *
 *   (2) CREATE    The keyword CREATE has been seen at the
 *                 beginning of a statement, possibly preceded by
 *                 EXPLAIN and/or followed by TEMP or TEMPORARY
 *
 *   (3) NORMAL    We are in the middle of statement which ends
 *                 with a single semicolon.
 *
 *   (4) TRIGGER   We are in the middle of a trigger definition
 *                 that must be ended by a semicolon, the keyword
 *                 END, and another semicolon.
 *
 *   (5) SEMI      We've seen the first semicolon in the ";END;"
 *                 that occurs at the end of a trigger definition.
 *
 *   (6) END       We've seen the ";END" of the ";END;" that
 *                 occurs at the end of a trigger definition.
 *
 * Transitions between states above are determined by tokens
 * extracted from the input.  The following tokens are
 * significant:
 *
 *   (0) tkEXPLAIN   The "explain" keyword.
 *   (1) tkCREATE    The "create" keyword.
 *   (2) tkTEMP      The "temp" or "temporary" keyword.
 *   (3) tkTRIGGER   The "trigger" keyword.
 *   (4) tkEND       The "end" keyword.
 *   (5) tkSEMI      A semicolon.
 *   (6) tkWS        Whitespace
 *   (7) tkOTHER     Any other SQL token.
 *
 * Whitespace never causes a state transition and is always
 * ignored.
 *
 * EXTERN: int dbsql_complete_stmt(const char *);
 */
int
dbsql_complete_stmt(const char *sql)
{
	u_int8_t state = 0;
	u_int8_t token;
	int c;

	/*
	 * The following matrix defines the transition from one
	 * state to another according to what token is seen.
	 * trans[state][token] returns the next state.
	 */
	static const u_int8_t trans[7][8] = {
		  /* Token:                                   */
  /* State:     ** EXPLAIN CREATE TEMP TRIGGER END SEMI WS OTHER */
  /* 0 START:  */ {     1,     2,   3,      3,  3,   0, 0,    3,},
  /* 1 EXPLAIN:*/ {     3,     2,   3,      3,  3,   0, 1,    3,},
  /* 2 CREATE: */ {     3,     3,   2,      4,  3,   0, 2,    3,},
  /* 3 NORMAL: */ {     3,     3,   3,      3,  3,   0, 3,    3,},
  /* 4 TRIGGER:*/ {     4,     4,   4,      4,  4,   5, 4,    4,},
  /* 5 SEMI:   */ {     4,     4,   4,      4,  6,   5, 5,    4,},
  /* 6 END:    */ {     4,     4,   4,      4,  4,   0, 6,    4,},
	};

	while (*sql) {
		switch (*sql) {
		case ';':
			token = tkSEMI;
			break;
		case ' ': /* FALLTHROUGH */
		case '\r': /* FALLTHROUGH */
		case '\t': /* FALLTHROUGH */
		case '\n': /* FALLTHROUGH */
		case '\f':
			/* White space is ignored */
			token = tkWS;
			break;
		case '/':
			/* C-style comments */
			if (sql[1] != '*') {
				token = tkOTHER;
				break;
			}
			sql += 2;
			while (sql[0]
			    && (sql[0] != '*' || sql[1] != '/')) {
				sql++;
			}
			if (sql[0] == 0)
				return 0;
			sql++;
			token = tkWS;
			break;
		case '-':
			/* SQL-style comments "--" to end of line */
			if (sql[1] != '-') {
				token = tkOTHER;
				break;
			}
			while (*sql && *sql != '\n') {
				sql++;
			}
			if (*sql == 0)
				return state == 0;
			token = tkWS;
			break;
		case '[':
			/* Microsoft-style identifiers in [...] */
			sql++;
			while (*sql && *sql != ']') {
				sql++;
			}
			if (*sql == 0)
				return 0;
			token = tkOTHER;
			break;
		case '`':
			/* Backtick-quoted identifiers */
			/* FALLTHROUGH */
		case '"':
			/* Double-quoted identifiers */
			/* FALLTHROUGH */
		case '\'':
			/* Single-quoted strings */
			c = *sql;
			sql++;
			while (*sql && *sql != c) {
				sql++;
			}
			if (*sql == 0)
				return 0;
			token = tkOTHER;
			break;
		default:
			if (id_char_p[(u_int8_t)*sql]) {
				/* Keywords and unquoted identifiers */
				int nid = 1;
				while (id_char_p[(u_int8_t)sql[nid]]) {
					nid++;
				}
				switch (*sql) {
				case 'c': /* FALLTHROUGH */
				case 'C':
					if (nid == 6 &&
					    strncasecmp(sql,
						"create", 6) == 0) {
						token = tkCREATE;
					} else {
						token = tkOTHER;
					}
					break;
				case 't': /* FALLTHROUGH */
				case 'T':
					if (nid == 7 &&
					    strncasecmp(sql,
						"trigger",
						7) == 0) {
						token = tkTRIGGER;
					} else if (nid == 4 &&
					    strncasecmp(sql,
						"temp", 4) == 0) {
						token = tkTEMP;
					} else if (nid == 9 &&
					    strncasecmp(sql,
						"temporary",
						9) == 0) {
						token = tkTEMP;
					} else {
						token = tkOTHER;
					}
					break;
				case 'e': /* FALLTHROUGH */
				case 'E':
					if (nid == 3 &&
					    strncasecmp(sql,
						"end", 3) == 0) {
						token = tkEND;
					} else if (nid == 7 &&
					    strncasecmp(sql,
						"explain",
						7) == 0) {
						token = tkEXPLAIN;
					} else {
						token = tkOTHER;
					}
					break;
				default:
					token = tkOTHER;
					break;
				}
				sql += nid - 1;
			} else {
				/* Operators and special symbols */
				token = tkOTHER;
			}
			break;
		}
		state = trans[state][token];
		sql++;
	}
	return state == 0;
}
