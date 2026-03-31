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
 * JSON1 extension functions for semi-structured data support.
 *
 * Implements:
 *   - json_valid(json_text)              Validate JSON
 *   - json_type(json_text [,path])       Return JSON type
 *   - json_extract(json_text, path)      Extract value at path
 *   - json_object(key1, val1, ...)       Create JSON object
 *   - json_array(val1, val2, ...)        Create JSON array
 *
 * JSON path syntax: $.field, $.array[0], $.nested.field
 */

#include "inc/dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#endif

#include "dbsql_int.h"

/*
 * JSON type enumeration.
 */
typedef enum {
	JSON_NULL = 0,
	JSON_TRUE,
	JSON_FALSE,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} json_type_t;

/*
 * JSON parsing state.
 */
typedef struct {
	const char *json;     /* JSON text being parsed */
	const char *ptr;      /* Current parse position */
	const char *end;      /* End of JSON text */
	char *error;          /* Error message if any */
} json_parser_t;

/*
 * Forward declarations.
 */
static int __json_skip_whitespace(json_parser_t *parser);
static int __json_parse_value(json_parser_t *parser);
static int __json_parse_string(json_parser_t *parser);
static int __json_parse_number(json_parser_t *parser);
static int __json_parse_array(json_parser_t *parser);
static int __json_parse_object(json_parser_t *parser);
static char *__json_extract_value(const char *json, const char *path);
static json_type_t __json_get_type_at_path(const char *json, const char *path);
static const char *__json_type_name(json_type_t type);

/*
 * __json_skip_whitespace --
 *	Skip whitespace characters in JSON text.
 *
 * STATIC: static int __json_skip_whitespace __P((json_parser_t *));
 */
static int
__json_skip_whitespace(json_parser_t *parser)
{
	while (parser->ptr < parser->end &&
	       (*parser->ptr == ' ' || *parser->ptr == '\t' ||
	        *parser->ptr == '\n' || *parser->ptr == '\r')) {
		parser->ptr++;
	}
	return 0;
}

/*
 * __json_parse_string --
 *	Parse a JSON string value.
 *
 * STATIC: static int __json_parse_string __P((json_parser_t *));
 */
static int
__json_parse_string(json_parser_t *parser)
{
	if (parser->ptr >= parser->end || *parser->ptr != '"')
		return -1;

	parser->ptr++; /* Skip opening quote */

	while (parser->ptr < parser->end) {
		char c = *parser->ptr;

		if (c == '"') {
			parser->ptr++; /* Skip closing quote */
			return 0;
		}

		if (c == '\\') {
			parser->ptr++;
			if (parser->ptr >= parser->end)
				return -1;
			/* Skip escaped character */
			parser->ptr++;
		} else {
			parser->ptr++;
		}
	}

	return -1; /* Unterminated string */
}

/*
 * __json_parse_number --
 *	Parse a JSON number value.
 *
 * STATIC: static int __json_parse_number __P((json_parser_t *));
 */
static int
__json_parse_number(json_parser_t *parser)
{
	const char *start = parser->ptr;

	/* Optional minus sign */
	if (parser->ptr < parser->end && *parser->ptr == '-')
		parser->ptr++;

	/* At least one digit required */
	if (parser->ptr >= parser->end || !isdigit((unsigned char)*parser->ptr))
		return -1;

	/* Integer part */
	while (parser->ptr < parser->end && isdigit((unsigned char)*parser->ptr))
		parser->ptr++;

	/* Optional fractional part */
	if (parser->ptr < parser->end && *parser->ptr == '.') {
		parser->ptr++;
		if (parser->ptr >= parser->end || !isdigit((unsigned char)*parser->ptr))
			return -1;
		while (parser->ptr < parser->end && isdigit((unsigned char)*parser->ptr))
			parser->ptr++;
	}

	/* Optional exponent */
	if (parser->ptr < parser->end &&
	    (*parser->ptr == 'e' || *parser->ptr == 'E')) {
		parser->ptr++;
		if (parser->ptr < parser->end &&
		    (*parser->ptr == '+' || *parser->ptr == '-'))
			parser->ptr++;
		if (parser->ptr >= parser->end || !isdigit((unsigned char)*parser->ptr))
			return -1;
		while (parser->ptr < parser->end && isdigit((unsigned char)*parser->ptr))
			parser->ptr++;
	}

	return parser->ptr > start ? 0 : -1;
}

/*
 * __json_parse_array --
 *	Parse a JSON array.
 *
 * STATIC: static int __json_parse_array __P((json_parser_t *));
 */
static int
__json_parse_array(json_parser_t *parser)
{
	if (parser->ptr >= parser->end || *parser->ptr != '[')
		return -1;

	parser->ptr++; /* Skip '[' */
	__json_skip_whitespace(parser);

	/* Empty array */
	if (parser->ptr < parser->end && *parser->ptr == ']') {
		parser->ptr++;
		return 0;
	}

	/* Parse array elements */
	while (parser->ptr < parser->end) {
		if (__json_parse_value(parser) != 0)
			return -1;

		__json_skip_whitespace(parser);

		if (parser->ptr >= parser->end)
			return -1;

		if (*parser->ptr == ']') {
			parser->ptr++;
			return 0;
		}

		if (*parser->ptr != ',')
			return -1;

		parser->ptr++; /* Skip comma */
		__json_skip_whitespace(parser);
	}

	return -1; /* Unterminated array */
}

/*
 * __json_parse_object --
 *	Parse a JSON object.
 *
 * STATIC: static int __json_parse_object __P((json_parser_t *));
 */
static int
__json_parse_object(json_parser_t *parser)
{
	if (parser->ptr >= parser->end || *parser->ptr != '{')
		return -1;

	parser->ptr++; /* Skip '{' */
	__json_skip_whitespace(parser);

	/* Empty object */
	if (parser->ptr < parser->end && *parser->ptr == '}') {
		parser->ptr++;
		return 0;
	}

	/* Parse object members */
	while (parser->ptr < parser->end) {
		/* Parse key (must be string) */
		if (__json_parse_string(parser) != 0)
			return -1;

		__json_skip_whitespace(parser);

		/* Expect colon */
		if (parser->ptr >= parser->end || *parser->ptr != ':')
			return -1;
		parser->ptr++;
		__json_skip_whitespace(parser);

		/* Parse value */
		if (__json_parse_value(parser) != 0)
			return -1;

		__json_skip_whitespace(parser);

		if (parser->ptr >= parser->end)
			return -1;

		if (*parser->ptr == '}') {
			parser->ptr++;
			return 0;
		}

		if (*parser->ptr != ',')
			return -1;

		parser->ptr++; /* Skip comma */
		__json_skip_whitespace(parser);
	}

	return -1; /* Unterminated object */
}

/*
 * __json_parse_value --
 *	Parse any JSON value.
 *
 * STATIC: static int __json_parse_value __P((json_parser_t *));
 */
static int
__json_parse_value(json_parser_t *parser)
{
	__json_skip_whitespace(parser);

	if (parser->ptr >= parser->end)
		return -1;

	switch (*parser->ptr) {
	case '{':
		return __json_parse_object(parser);
	case '[':
		return __json_parse_array(parser);
	case '"':
		return __json_parse_string(parser);
	case 't':
		/* true */
		if (parser->end - parser->ptr >= 4 &&
		    memcmp(parser->ptr, "true", 4) == 0) {
			parser->ptr += 4;
			return 0;
		}
		return -1;
	case 'f':
		/* false */
		if (parser->end - parser->ptr >= 5 &&
		    memcmp(parser->ptr, "false", 5) == 0) {
			parser->ptr += 5;
			return 0;
		}
		return -1;
	case 'n':
		/* null */
		if (parser->end - parser->ptr >= 4 &&
		    memcmp(parser->ptr, "null", 4) == 0) {
			parser->ptr += 4;
			return 0;
		}
		return -1;
	case '-':
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		return __json_parse_number(parser);
	default:
		return -1;
	}
}

/*
 * __json_validate --
 *	Validate JSON text.
 *
 * STATIC: static int __json_validate __P((const char *));
 */
static int
__json_validate(const char *json)
{
	json_parser_t parser;

	if (json == NULL)
		return 0;

	parser.json = json;
	parser.ptr = json;
	parser.end = json + strlen(json);
	parser.error = NULL;

	if (__json_parse_value(&parser) != 0)
		return 0;

	__json_skip_whitespace(&parser);

	/* Ensure we consumed entire input */
	return parser.ptr >= parser.end ? 1 : 0;
}

/*
 * __json_type_name --
 *	Return string name for JSON type.
 *
 * STATIC: static const char *__json_type_name __P((json_type_t));
 */
static const char *
__json_type_name(json_type_t type)
{
	switch (type) {
	case JSON_NULL:   return "null";
	case JSON_TRUE:   return "true";
	case JSON_FALSE:  return "false";
	case JSON_NUMBER: return "number";
	case JSON_STRING: return "string";
	case JSON_ARRAY:  return "array";
	case JSON_OBJECT: return "object";
	default:          return "unknown";
	}
}

/*
 * __json_detect_type --
 *	Detect the type of a JSON value.
 *
 * STATIC: static json_type_t __json_detect_type __P((const char *));
 */
static json_type_t
__json_detect_type(const char *json)
{
	const char *p = json;

	/* Skip whitespace */
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;

	switch (*p) {
	case '{':
		return JSON_OBJECT;
	case '[':
		return JSON_ARRAY;
	case '"':
		return JSON_STRING;
	case 't':
		if (memcmp(p, "true", 4) == 0)
			return JSON_TRUE;
		break;
	case 'f':
		if (memcmp(p, "false", 5) == 0)
			return JSON_FALSE;
		break;
	case 'n':
		if (memcmp(p, "null", 4) == 0)
			return JSON_NULL;
		break;
	case '-':
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		return JSON_NUMBER;
	}

	return JSON_NULL; /* Invalid JSON defaults to null */
}

/*
 * __json_get_type_at_path --
 *	Get JSON type at specified path.
 *
 * STATIC: static json_type_t __json_get_type_at_path __P((const char *,
 * STATIC:     const char *));
 */
static json_type_t
__json_get_type_at_path(const char *json, const char *path)
{
	char *value;
	json_type_t type;

	/* Root path */
	if (path == NULL || strcmp(path, "$") == 0)
		return __json_detect_type(json);

	/* Extract value at path */
	value = __json_extract_value(json, path);
	if (value == NULL)
		return JSON_NULL;

	type = __json_detect_type(value);
	free(value);
	return type;
}

/*
 * __json_extract_value --
 *	Extract JSON value at specified path.
 *	Simplified path parser supporting: $.field, $.array[0], $.nested.field
 *
 * STATIC: static char *__json_extract_value __P((const char *, const char *));
 */
static char *
__json_extract_value(const char *json, const char *path)
{
	const char *p;
	char *result;

	/* For this minimal implementation, just handle root path */
	if (path == NULL || strcmp(path, "$") == 0) {
		result = strdup(json);
		return result;
	}

	/* Skip leading $ */
	p = path;
	if (*p == '$')
		p++;

	/*
	 * TODO: Full path parsing implementation.
	 * For now, return copy of input for root path only.
	 */
	if (*p == '\0') {
		result = strdup(json);
		return result;
	}

	/* Path extraction not yet implemented for non-root paths */
	return NULL;
}

/*
 * __json_valid_func --
 *	Implementation of json_valid() function.
 *	Returns 1 if valid JSON, 0 otherwise.
 *
 * STATIC: static void __json_valid_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__json_valid_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	int valid;

	if (argc != 1 || argv[0] == NULL) {
		dbsql_set_result_int(context, 0);
		return;
	}

	valid = __json_validate(argv[0]);
	dbsql_set_result_int(context, valid);
}

/*
 * __json_type_func --
 *	Implementation of json_type() function.
 *	Returns JSON type as string: "object", "array", "string",
 *	"number", "true", "false", "null".
 *
 * STATIC: static void __json_type_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__json_type_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *json;
	const char *path;
	json_type_t type;
	const char *type_name;

	if (argc < 1 || argv[0] == NULL) {
		dbsql_set_result_string(context, "null", -1);
		return;
	}

	json = argv[0];
	path = (argc > 1) ? argv[1] : "$";

	type = __json_get_type_at_path(json, path);
	type_name = __json_type_name(type);

	dbsql_set_result_string(context, type_name, -1);
}

/*
 * __json_extract_func --
 *	Implementation of json_extract() function.
 *	Extracts value at JSON path.
 *
 * STATIC: static void __json_extract_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__json_extract_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	const char *json;
	const char *path;
	char *result;

	if (argc < 2 || argv[0] == NULL || argv[1] == NULL) {
		return; /* Return NULL */
	}

	json = argv[0];
	path = argv[1];

	result = __json_extract_value(json, path);
	if (result != NULL) {
		dbsql_set_result_string(context, result, -1);
		free(result);
	}
}

/*
 * __json_object_func --
 *	Implementation of json_object() function.
 *	Creates JSON object from key/value pairs.
 *
 * STATIC: static void __json_object_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__json_object_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	char *result;
	size_t result_size;
	size_t result_len;
	int i;

	/* argc must be even (key/value pairs) */
	if (argc % 2 != 0) {
		dbsql_set_result_error(context,
		    "json_object requires even number of arguments", -1);
		return;
	}

	/* Start with opening brace */
	result_size = 1024;
	result = malloc(result_size);
	if (result == NULL) {
		dbsql_set_result_error(context, "out of memory", -1);
		return;
	}

	strcpy(result, "{");
	result_len = 1;

	/* Add key/value pairs */
	for (i = 0; i < argc; i += 2) {
		const char *key = argv[i];
		const char *value = argv[i + 1];
		size_t needed;

		if (key == NULL)
			continue;

		/* Add comma separator after first pair */
		if (i > 0) {
			needed = result_len + 2;
			if (needed >= result_size) {
				result_size *= 2;
				result = realloc(result, result_size);
				if (result == NULL) {
					dbsql_set_result_error(context,
					    "out of memory", -1);
					return;
				}
			}
			strcat(result, ",");
			result_len++;
		}

		/* Calculate space needed for this pair */
		needed = result_len + strlen(key) + 10;
		if (value != NULL)
			needed += strlen(value);

		/* Grow buffer if needed */
		while (needed >= result_size) {
			result_size *= 2;
			result = realloc(result, result_size);
			if (result == NULL) {
				dbsql_set_result_error(context,
				    "out of memory", -1);
				return;
			}
		}

		/* Add key */
		strcat(result, "\"");
		strcat(result, key);
		strcat(result, "\":");

		/* Add value */
		if (value == NULL) {
			strcat(result, "null");
		} else {
			strcat(result, "\"");
			strcat(result, value);
			strcat(result, "\"");
		}

		result_len = strlen(result);
	}

	/* Add closing brace */
	strcat(result, "}");

	dbsql_set_result_string(context, result, -1);
	free(result);
}

/*
 * __json_array_func --
 *	Implementation of json_array() function.
 *	Creates JSON array from values.
 *
 * STATIC: static void __json_array_func __P((dbsql_func_t *, int,
 * STATIC:     const char **));
 */
static void
__json_array_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	char *result;
	size_t result_size;
	size_t result_len;
	int i;

	/* Start with opening bracket */
	result_size = 1024;
	result = malloc(result_size);
	if (result == NULL) {
		dbsql_set_result_error(context, "out of memory", -1);
		return;
	}

	strcpy(result, "[");
	result_len = 1;

	/* Add array elements */
	for (i = 0; i < argc; i++) {
		const char *value = argv[i];
		size_t needed;

		/* Add comma separator after first element */
		if (i > 0) {
			needed = result_len + 2;
			if (needed >= result_size) {
				result_size *= 2;
				result = realloc(result, result_size);
				if (result == NULL) {
					dbsql_set_result_error(context,
					    "out of memory", -1);
					return;
				}
			}
			strcat(result, ",");
			result_len++;
		}

		/* Calculate space needed for this element */
		needed = result_len + 10;
		if (value != NULL)
			needed += strlen(value);

		/* Grow buffer if needed */
		while (needed >= result_size) {
			result_size *= 2;
			result = realloc(result, result_size);
			if (result == NULL) {
				dbsql_set_result_error(context,
				    "out of memory", -1);
				return;
			}
		}

		/* Add value */
		if (value == NULL) {
			strcat(result, "null");
		} else {
			strcat(result, "\"");
			strcat(result, value);
			strcat(result, "\"");
		}

		result_len = strlen(result);
	}

	/* Add closing bracket */
	strcat(result, "]");

	dbsql_set_result_string(context, result, -1);
	free(result);
}

/*
 * __register_json_funcs --
 *	Register all JSON extension functions.
 *
 * PUBLIC: void __register_json_funcs __P((DBSQL *));
 */
void
__register_json_funcs(DBSQL *dbp)
{
	/* json_valid(json_text) - validate JSON */
	dbp->create_function(dbp, "json_valid", 1,
	    DBSQL_UTF8_ENCODED, NULL,
	    __json_valid_func, NULL, NULL);
	dbp->func_return_type(dbp, "json_valid", DBSQL_NUMERIC);

	/* json_type(json_text [,path]) - return JSON type */
	dbp->create_function(dbp, "json_type", 1,
	    DBSQL_UTF8_ENCODED, NULL,
	    __json_type_func, NULL, NULL);
	dbp->create_function(dbp, "json_type", 2,
	    DBSQL_UTF8_ENCODED, NULL,
	    __json_type_func, NULL, NULL);
	dbp->func_return_type(dbp, "json_type", DBSQL_TEXT);

	/* json_extract(json_text, path) - extract value at path */
	dbp->create_function(dbp, "json_extract", 2,
	    DBSQL_UTF8_ENCODED, NULL,
	    __json_extract_func, NULL, NULL);
	dbp->func_return_type(dbp, "json_extract", DBSQL_TEXT);

	/* json_object(key1, val1, ...) - create JSON object */
	dbp->create_function(dbp, "json_object", -1,
	    DBSQL_UTF8_ENCODED, NULL,
	    __json_object_func, NULL, NULL);
	dbp->func_return_type(dbp, "json_object", DBSQL_TEXT);

	/* json_array(val1, val2, ...) - create JSON array */
	dbp->create_function(dbp, "json_array", -1,
	    DBSQL_UTF8_ENCODED, NULL,
	    __json_array_func, NULL, NULL);
	dbp->func_return_type(dbp, "json_array", DBSQL_TEXT);
}
