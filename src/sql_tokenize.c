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
 */

/*
 * An tokenizer for SQL
 *
 * This file contains C code that splits an SQL input string up into
 * individual tokens and sends those tokens one-by-one over to the
 * parser for analysis.
 */

#include "dbsql_config.h"

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
	u_int8_t token_type; /* token_t value for this keyword */
	u_int8_t len;        /* Length of this keyword */
	u_int8_t inext;      /* Index in sql_tokens_table[] of next with
                                same hash */
};

/*
 * These are the keywords
 */
static keyword_t sql_tokens_table[] = {
  { "ABORT",             TK_ABORT,        },
  { "AFTER",             TK_AFTER,        },
  { "ALL",               TK_ALL,          },
  { "AND",               TK_AND,          },
  { "AS",                TK_AS,           },
  { "ASC",               TK_ASC,          },
  { "ATTACH",            TK_ATTACH,       },
  { "BEFORE",            TK_BEFORE,       },
  { "BEGIN",             TK_BEGIN,        },
  { "BETWEEN",           TK_BETWEEN,      },
  { "BY",                TK_BY,           },
  { "CASCADE",           TK_CASCADE,      },
  { "CASE",              TK_CASE,         },
  { "CHECK",             TK_CHECK,        },
  { "CLUSTER",           TK_CLUSTER,      },
  { "COLLATE",           TK_COLLATE,      },
  { "COMMIT",            TK_COMMIT,       },
  { "CONFLICT",          TK_CONFLICT,     },
  { "CONSTRAINT",        TK_CONSTRAINT,   },
  { "COPY",              TK_COPY,         },
  { "CREATE",            TK_CREATE,       },
  { "CROSS",             TK_JOIN_KW,      },
  { "DATABASE",          TK_DATABASE,     },
  { "DEFAULT",           TK_DEFAULT,      },
  { "DEFERRED",          TK_DEFERRED,     },
  { "DEFERRABLE",        TK_DEFERRABLE,   },
  { "DELETE",            TK_DELETE,       },
  { "DELIMITERS",        TK_DELIMITERS,   },
  { "DESC",              TK_DESC,         },
  { "DETACH",            TK_DETACH,       },
  { "DISTINCT",          TK_DISTINCT,     },
  { "DROP",              TK_DROP,         },
  { "END",               TK_END,          },
  { "EACH",              TK_EACH,         },
  { "ELSE",              TK_ELSE,         },
  { "EXCEPT",            TK_EXCEPT,       },
  { "EXPLAIN",           TK_EXPLAIN,      },
  { "FAIL",              TK_FAIL,         },
  { "FOR",               TK_FOR,          },
  { "FOREIGN",           TK_FOREIGN,      },
  { "FROM",              TK_FROM,         },
  { "FULL",              TK_JOIN_KW,      },
  { "GLOB",              TK_GLOB,         },
  { "GROUP",             TK_GROUP,        },
  { "HAVING",            TK_HAVING,       },
  { "IGNORE",            TK_IGNORE,       },
  { "IMMEDIATE",         TK_IMMEDIATE,    },
  { "IN",                TK_IN,           },
  { "INDEX",             TK_INDEX,        },
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
  { "LEFT",              TK_JOIN_KW,      },
  { "LIKE",              TK_LIKE,         },
  { "LIMIT",             TK_LIMIT,        },
  { "MATCH",             TK_MATCH,        },
  { "NATURAL",           TK_JOIN_KW,      },
  { "NOT",               TK_NOT,          },
  { "NOTNULL",           TK_NOTNULL,      },
  { "NULL",              TK_NULL,         },
  { "OF",                TK_OF,           },
  { "OFFSET",            TK_OFFSET,       },
  { "ON",                TK_ON,           },
  { "OR",                TK_OR,           },
  { "ORDER",             TK_ORDER,        },
  { "OUTER",             TK_JOIN_KW,      },
  { "PRAGMA",            TK_PRAGMA,       },
  { "PRIMARY",           TK_PRIMARY,      },
  { "RAISE",             TK_RAISE,        },
  { "REFERENCES",        TK_REFERENCES,   },
  { "REPLACE",           TK_REPLACE,      },
  { "RESTRICT",          TK_RESTRICT,     },
  { "RIGHT",             TK_JOIN_KW,      },
  { "ROLLBACK",          TK_ROLLBACK,     },
  { "ROW",               TK_ROW,          },
  { "SELECT",            TK_SELECT,       },
  { "SET",               TK_SET,          },
  { "STATEMENT",         TK_STATEMENT,    },
  { "TABLE",             TK_TABLE,        },
  { "TEMP",              TK_TEMP,         },
  { "TEMPORARY",         TK_TEMP,         },
  { "THEN",              TK_THEN,         },
  { "TRANSACTION",       TK_TRANSACTION,  },
  { "TRIGGER",           TK_TRIGGER,      },
  { "UNION",             TK_UNION,        },
  { "UNIQUE",            TK_UNIQUE,       },
  { "UPDATE",            TK_UPDATE,       },
  { "USING",             TK_USING,        },
  { "VACUUM",            TK_VACUUM,       },
  { "VALUES",            TK_VALUES,       },
  { "VIEW",              TK_VIEW,         },
  { "WHEN",              TK_WHEN,         },
  { "WHERE",             TK_WHERE,        },
};

/*
 * This is the hash table
 */
#define KEY_HASH_SIZE 101
static u_int8_t ai_table[KEY_HASH_SIZE];


/*
 * __get_keyword_code --
 *	This function looks up an identifier to determine if it is a
 *	keyword.  If it is a keyword, the token code of that keyword is 
 *	returned.  If the input is not a keyword, TK_ID is returned.
 *
 * PUBLIC: int get_keyword_code __P((const char *, int));
 */
int
__get_keyword_code(z, n)
	const char *z;
	int n;
{
	int h, i;
	int nk;
	keyword_t *p;
	static char need_init = 1;
	if (need_init) { /* TODO: beginning of what used to be mutex'ed */
		/* Initialize the keyword hash table */
		need_init = 0;
		nk = sizeof(sql_tokens_table) /
			sizeof(sql_tokens_table[0]);
		for (i = 0; i < nk; i++) {
			sql_tokens_table[i].len =
				strlen(sql_tokens_table[i].name);
			h = __hash_ignore_case(sql_tokens_table[i].name,
					       sql_tokens_table[i].len);
			h %= KEY_HASH_SIZE;
			sql_tokens_table[i].inext = ai_table[h];
			ai_table[h] = i+1;
		}
	} /* TODO: end of what used to be mutex'ed */
	h = __hash_ignore_case(z, n) % KEY_HASH_SIZE;
	for (i = ai_table[h]; i; i = p->inext) {
		p = &sql_tokens_table[i-1];
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
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 2x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  /* 3x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 4x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,  /* 5x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 6x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,  /* 7x */
};


/*
 * __get_token --
 *	Return the length of the token that begins at z[0]. 
 *	Store the token type in *token_type before returning.
 *
 * STATIC: static int __get_token __P((const unsigned char *, int *));
 */
static int
__get_token(z, token_type)
	const unsigned char *z;
	int *token_type;
{
	int i, delim;

	switch(*z) {
	case ' ':  /* FALLTHROUGH */
	case '\t': /* FALLTHROUGH */
	case '\n': /* FALLTHROUGH */
	case '\f': /* FALLTHROUGH */
	case '\r': /* FALLTHROUGH */
		i = 1;
		while (isspace(z[i])) {
			i++;
		}
		*token_type = TK_SPACE;
		return i;
		break;
	case '-':
		if (z[1] == '-') {
			i = 2;
			while (z[i] && z[i] != '\n') {
				i++;
			}
			*token_type = TK_COMMENT;
			return i;
		}
		*token_type = TK_MINUS;
		return 1;
		break;
	case '(':
		*token_type = TK_LP;
		return 1;
		break;
	case ')':
		*token_type = TK_RP;
		return 1;
		break;
	case ';':
		*token_type = TK_SEMI;
		return 1;
		break;
	case '+':
		*token_type = TK_PLUS;
		return 1;
		break;
	case '*':
		*token_type = TK_STAR;
		return 1;
		break;
	case '/':
		if (z[1] != '*' || z[2] == 0) {
			*token_type = TK_SLASH;
			return 1;
		}
		i = 3;
		while (z[i] && (z[i]!='/' || z[i-1]!='*')) {
			i++;
		}
		if (z[i])
			i++;
		*token_type = TK_COMMENT;
		return i;
		break;
	case '%':
		*token_type = TK_REM;
		return 1;
		break;
	case '=':
		*token_type = TK_EQ;
		return 1 + (z[1] == '=');
		break;
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
		break;
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
		break;
	case '!':
		if (z[1] != '=') {
			*token_type = TK_ILLEGAL;
			return 2;
		} else {
			*token_type = TK_NE;
			return 2;
		}
		break;
	case '|':
		if (z[1] != '|') {
			*token_type = TK_BITOR;
			return 1;
		} else {
			*token_type = TK_CONCAT;
			return 2;
		}
		break;
	case ',':
		*token_type = TK_COMMA;
		return 1;
		break;
	case '&':
		*token_type = TK_BITAND;
		return 1;
		break;
	case '~':
		*token_type = TK_BITNOT;
		return 1;
		break;
	case '\'': /* FALLTHROUGH */
	case '"':
		delim = z[0];
		for (i = 1; z[i]; i++) {
			if (z[i] == delim) {
				if (z[i+1] == delim) {
					i++;
				} else {
					break;
				}
			}
		}
		if (z[i])
			i++;
		*token_type = TK_STRING;
		return i;
	case '.':
		*token_type = TK_DOT;
		return 1;
		break;
	case '0': /* FALLTHROUGH */
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
		i = 1;
		while (isdigit(z[i])) {
			i++;
		}
		if (z[i] == '.' && isdigit(z[i+1])) {
			i += 2;
			while(isdigit(z[i])) {
				i++;
			}
			*token_type = TK_FLOAT;
		}
		if ((z[i] == 'e' || z[i] == 'E') &&
		    (isdigit(z[i+1]) ||
		     ((z[i+1] == '+' || z[i+1] == '-') && isdigit(z[i+2])))) {
			i += 2;
			while(isdigit(z[i])) {
				i++;
			}
			*token_type = TK_FLOAT;
		}
		return i;
		break;
	case '[':
		i = 1;
		while (z[i] && z[i-1] != ']') {
			i++;
		}
		*token_type = TK_ID;
		return i;
		break;
	case '?':
		*token_type = TK_VARIABLE;
		return 1;
		break;
	default:
		if ((*z & 0x80) == 0 && !id_char_p[*z]) {
			break;
		}
		i = 1;
		while((z[i] & 0x80)!=0 || id_char_p[z[i]]) {
			i++;
		}
		*token_type = __get_keyword_code((char*)z, i);
		return i;
		break;
	}
	*token_type = TK_ILLEGAL;
	return 1;
}

/*
 * __run_sql_parser --
 *	Run the parser on the given SQL string.  The parser structure is
 *	passed in.  A DBSQL_ status code is returned.
 *
 * PUBLIC: int __run_sql_parser __P((parser_t *, const char *, char **));
 */
/*TODO: REMOVE THIS  If an error occurs
 * and pzErrMsg!=NULL then an error message might be written into 
 * memory obtained from malloc() and *pzErrMsg made to point to that
 * error message.  Or maybe not.
 */
int
__run_sql_parser(parser, sql, err_msgs)
	parser_t *parser;
	const char *sql;
	char **err_msgs;
{
	int nerr = 0;
	int i;
	void *engine;
	int token_type;
	int last_token_parsed = -1;
	DBSQL *dbp = parser->db;
	extern void *__sql_parser_alloc(DBSQL *, int(*)(DBSQL*,size_t,void *));
	extern void __sql_parser_free(DBSQL *, void *, void(*)(DBSQL *,void*));
	extern int __sql_parser(void*, int, token_t, parser_t*);

	dbp->flags &= ~DBSQL_Interrupt;
	parser->rc = DBSQL_SUCCESS;
	i = 0;
	engine = __sql_parser_alloc(dbp, __dbsql_malloc);
	if (engine == 0) {
		__str_append(err_msgs, "out of memory", (char*)0);
		return 1;
	}
	parser->sLastToken.dyn = 0;
	parser->zTail = sql;
	while (parser->rc == DBSQL_SUCCESS && sql[i] != 0) {
		DBSQL_ASSERT(i >= 0);
		parser->sLastToken.z = &sql[i];
		DBSQL_ASSERT(parser->sLastToken.dyn == 0);
		parser->sLastToken.n = __get_token((unsigned char*)&sql[i],
						   &token_type);
		i += parser->sLastToken.n;
		switch (token_type) {
		case TK_SPACE: /* FALLTHROUGH */
		case TK_COMMENT:
			if ((dbp->flags & DBSQL_Interrupt) != 0) {
				parser->rc = DBSQL_INTERRUPTED;
				__str_append(err_msgs, "interrupt",
					     (char*)0);
				goto abort_parse;
			}
			break;
		case TK_ILLEGAL:
			__str_nappend(err_msgs, "unrecognized token: \"",
				      -1, parser->sLastToken.z,
				      parser->sLastToken.n, "\"", 1, NULL);
			nerr++;
			goto abort_parse;
			break;
		case TK_SEMI:
			parser->zTail = &sql[i];
			/* FALLTHROUGH */
		default:
			__sql_parser(engine, token_type, parser->sLastToken,
				     parser);
			last_token_parsed = token_type;
			if (parser->rc != DBSQL_SUCCESS) {
				goto abort_parse;
			}
			break;
		}
	}
  abort_parse:
	if (sql[i] == 0 && nerr == 0 && parser->rc == DBSQL_SUCCESS) {
		if (last_token_parsed != TK_SEMI) {
			__sql_parser(engine, TK_SEMI, parser->sLastToken,
				     parser);
			parser->zTail = &sql[i];
		}
		__sql_parser(engine, 0, parser->sLastToken, parser);
	}
	__sql_parser_free(dbp, engine, __dbsql_free);
	if (parser->rc != DBSQL_SUCCESS && parser->rc != DBSQL_DONE &&
	    parser->zErrMsg == 0) {
		__str_append(&parser->zErrMsg,
			     dbsql_strerror(parser->rc), (char*)0);
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
	if (parser->pVdbe && (parser->useCallback || parser->nErr > 0)) {
		__vdbe_delete(parser->pVdbe);
		parser->pVdbe = 0;
	}
	if (parser->pNewTable) {
		__vdbe_delete_table(parser->db, parser->pNewTable);
		parser->pNewTable = 0;
	}
	if (parser->pNewTrigger) {
		__vdbe_delete_trigger(parser->pNewTrigger);
		parser->pNewTrigger = 0;
	}
	if (nerr > 0 &&
	    (parser->rc == DBSQL_SUCCESS || parser->rc == DBSQL_DONE)) {
		parser->rc = DBSQL_ERROR;
	}
	return nerr;
}

/*
 * Token types used by the dbsql_complete_stmt() routine.  See the header
 * comments on that procedure for additional information.
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
 *   (0) START     At the beginning or end of an SQL statement.  This routine
 *                 returns 1 if it ends in the START state and 0 if it ends
 *                 in any other state.
 *
 *   (1) EXPLAIN   The keyword EXPLAIN has been seen at the beginning of 
 *                 a statement.
 *
 *   (2) CREATE    The keyword CREATE has been seen at the beginning of a
 *                 statement, possibly preceeded by EXPLAIN and/or followed by
 *                 TEMP or TEMPORARY
 *
 *   (3) NORMAL    We are in the middle of statement which ends with a single
 *                 semicolon.
 *
 *   (4) TRIGGER   We are in the middle of a trigger definition that must be
 *                 ended by a semicolon, the keyword END, and another
 *                 semicolon.
 *
 *   (5) SEMI      We've seen the first semicolon in the ";END;" that occurs at
 *                 the end of a trigger definition.
 *
 *   (6) END       We've seen the ";END" of the ";END;" that occurs at the end
 *                 of a trigger difinition.
 *
 * Transitions between states above are determined by tokens extracted
 * from the input.  The following tokens are significant:
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
 * Whitespace never causes a state transition and is always ignored.
 *
 * EXTERN: int dbsql_complete_stmt __P((const char *));
 *
 */
int
dbsql_complete_stmt(sql)
	const char *sql;
{
	u_int8_t state = 0; /* Current state, using values from comment */
	u_int8_t token;     /* Value of the next token */
	int c;

	/*
	 * The following matrix defines the transition from one state to
	 * another according to what token is seen.  trans[state][token]
	 * returns the next state.
	 */
	static const u_int8_t trans[7][8] = {
                    /* Token:                                                */
    /* State:       **  EXPLAIN  CREATE  TEMP  TRIGGER  END  SEMI  WS  OTHER */
    /* 0   START: */ {       1,      2,    3,       3,   3,    0,  0,     3, },
    /* 1 EXPLAIN: */ {       3,      2,    3,       3,   3,    0,  1,     3, },
    /* 2  CREATE: */ {       3,      3,    2,       4,   3,    0,  2,     3, },
    /* 3  NORMAL: */ {       3,      3,    3,       3,   3,    0,  3,     3, },
    /* 4 TRIGGER: */ {       4,      4,    4,       4,   4,    5,  4,     4, },
    /* 5    SEMI: */ {       4,      4,    4,       4,   6,    5,  5,     4, },
    /* 6     END: */ {       4,      4,    4,       4,   4,    0,  6,     4, },
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
			while (sql[0] && (sql[0] != '*' || sql[1] != '/')) {
				sql++;
			}
			if (sql[0] == 0)
				return 0;
			sql++;
			token = tkWS;
			break;
		case '-':
			/* SQL-style comments from "--" to end of line */
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
			while (*sql && *sql!=']') {
				sql++;
			}
			if (*sql == 0)
				return 0;
			token = tkOTHER;
			break;
		case '"':
			/* single- and double-quoted strings */
			/* FALLTHROUGH */
		case '\'':
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
								   "create",
								   6) == 0) {
						token = tkCREATE;
					} else {
						token = tkOTHER;
					}
					break;
				case 't': /* FALLTHROUGH */
				case 'T':
					if (nid == 7 &&
					    strncasecmp(sql,
						   "trigger", 7) == 0 ) {
						token = tkTRIGGER;
					} else if (nid == 4 &&
					    strncasecmp(sql,
						  "temp", 4) == 0) {
						token = tkTEMP;
					} else if (nid == 9 &&
					    strncasecmp(sql,
						  "temporary", 9) == 0) {
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
						  "explain", 7) == 0) {
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
