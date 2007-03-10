/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * There are special exceptions to the terms and conditions of the GPL as it
 * is applied to this software. View the full text of the exception in file
 * LICENSE_EXCEPTIONS in the directory of this software distribution.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * http://creativecommons.org/licenses/GPL/2.0/
 *
 * $Id: dbsql.c 7 2007-02-03 13:34:17Z gburd $
 */


/*
 * This file contains code to implement the command line
 * utility for accessing databases.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "dbsql_config.h"
#include "dbsql.h"

#if !defined(_WIN32) && !defined(WIN32)
#include <signal.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
#endif

#if defined(HAVE_READLINE) && HAVE_READLINE==1
#include <readline/readline.h>
#include <readline/history.h>
#else
#define readline(p) local_getline(p,stdin)
#define add_history(X)
#define read_history(X)
#define write_history(X)
#define stifle_history(X)
#endif

struct globals {
        /* The Berkeley DBSQL database manager. */
	DBSQL *dbp;

        /* The Berkeley DB database environment. */
	DB_ENV *dbenv;

        /* Non-zero if an interrupt (Control-C) has been received. */
	int interrupted_p;

        /*
         * This is the name of our program. It is set in main(), used
         * in a number of other places, mostly for error messages.
         */
	char *progname;

	/* An output stream for error messages, normally stdout. */
	FILE *errfp;

        /*
         * Prompt strings. Initialized in main. Settable using the
         * '.prompt [main] [continuation]' command.
         */
        char prompt[20];         /* First line prompt. default: "SQL> "*/
	char prompt2[20];        /* Continuation prompt. default: "...> " */
};

static struct globals g;

extern int __str_is_numeric(const char*);
extern int isatty();

/*
 * This routine reads a line of text from standard input, stores
 * the text in memory obtained from malloc() and returns a pointer
 * to the text.  NULL is returned at end of file, or if malloc()
 * fails.
 * The interface is like "readline" but no command-line editing
 * is done.
 */
static char *
local_getline(prompt, in)
	char *prompt;
	FILE *in;
{
	char *line;
	int len;
	int n;
	int eol;

	if (prompt && *prompt) {
		printf("%s", prompt);
		fflush(stdout);
	}
	len = 100;
	line = malloc(len);
	if (line == 0)
		return 0;
	n = 0;
	eol = 0;
	while(!eol) {
		if (n + 100 > len) {
			len = (len * 2) + 100;
			line = realloc(line, len);
			if (line == 0)
				return 0;
		}
		if (fgets(&line[n], len - n, in) == 0) {
			if (n == 0) {
				free(line);
				return 0;
			}
			line[n] = 0;
			eol = 1;
			break;
		}
		while(line[n]) {
			n++;
		}
		if (n > 0 && line[n - 1] == '\n') {
			n--;
			line[n] = 0;
			eol = 1;
		}
	}
	line = realloc(line, n + 1);
	return line;
}

/*
 * Retrieve a single line of input text.  "isatty" is true if text
 * is coming from a terminal.  In that case, we issue a prompt and
 * attempt to use "readline" for command-line editing.  If "isatty"
 * is false, use "local_getline" instead of "readline" and issue no prompt.
 *
 * zPrior is a string of prior text retrieved.  If not the empty
 * string, then issue a continuation prompt.
 */
static char *
one_input_line(prior, in)
	const char *prior;
	FILE *in;
{
	char *prompt;
	char *result;
	if (in != 0) {
		return local_getline(0, in);
	}
	if (prior && prior[0]) {
		prompt = g.prompt2;
	} else {
		prompt = g.prompt;
	}
	result = readline(prompt);
	if (result)
		add_history(result);
	return result;
}

struct previous_mode_data {
	int valid;        /* Is there legit data in here? */
	int mode;
	int show_header;
	int col_width[100];
};

/*
 * An pointer to an instance of this structure is passed from
 * the main program to the callback.  This is used to communicate
 * state and mode information.
 */
struct callback_data {
	DBSQL *db;             /* The database */
	int echoOn;            /* True to echo input commands */
	int cnt;               /* Number of records displayed so far */
	FILE *out;             /* Write results here */
	int mode;              /* An output mode setting */
	int show_header;       /* True to show column names in List or
				  Column mode */
	char *dest_table;      /* Name of destination table when MODE_Insert */
	char separator[20];    /* Separator character for MODE_List */
	int col_width[100];    /* Requested width of each column when in column mode*/
	int actual_width[100]; /* Actual width of each column */
	char nullvalue[20];    /* The text to print when a NULL comes back from
				* the database */
	struct previous_mode_data explainPrev; /* Holds the mode information
				* just before .explain ON */
	char outfile[FILENAME_MAX]; /* Filename for *out */
	const char *db_filename; /* Name of the database file */
	char *crypt_key;       /* Encryption key */
};

/*
 * These are the allowed modes.
 */
#define MODE_Line     0  /* One column per line.  Blank line between records */
#define MODE_Column   1  /* One record per line in neat columns */
#define MODE_List     2  /* One record per line with a separator */
#define MODE_Semi     3  /* Same as MODE_List but append ";" to each line */
#define MODE_Html     4  /* Generate an XHTML table */
#define MODE_Insert   5  /* Generate SQL "insert" statements */
#define MODE_NUM_OF   6  /* The number of modes (not a mode itself) */

char *modeDescr[MODE_NUM_OF] = {
	"line",
	"column",
	"list",
	"semi",
	"html",
	"insert"
};

/*
 * Number of elements in an array
 */
#define ARRAY_SIZE(X)  (sizeof(X)/sizeof(X[0]))

/*
 * Output the given string as a quoted string using SQL quoting conventions.
 */
static void
output_quoted_string(out, z)
	FILE *out;
	const char *z;
{
	int i;
	int single = 0;
	for(i = 0; z[i]; i++) {
		if (z[i] == '\'')
			single++;
	}
	if (single == 0) {
		fprintf(out, "'%s'",z);
	} else {
		fprintf(out, "'");
		while(*z) {
			for(i = 0; z[i] && z[i] != '\''; i++) {}
			if (i == 0) {
				fprintf(out, "''");
				z++;
			} else if (z[i] == '\'') {
				fprintf(out, "%.*s''", i, z);
				z += i + 1;
			} else {
				fprintf(out, "%s", z);
				break;
			}
		}
		fprintf(out, "'");
	}
}

/*
 * Output the given string with characters that are special to
 * HTML escaped.
 */
static void
output_html_string(out, z)
	FILE *out;
	const char *z;
{
	int i;
	while(*z) {
		for(i = 0; z[i] && z[i] != '<' && z[i] != '&'; i++) {}
		if (i > 0) {
			fprintf(out, "%.*s", i, z);
		}
		if (z[i] == '<') {
			fprintf(out, "&lt;");
		} else if (z[i] == '&') {
			fprintf(out, "&amp;");
		} else {
			break;
		}
		z += i + 1;
	}
}

/*
** This routine runs when the user presses Ctrl-C
*/
static void
interrupt_handler(NOT_USED)
	int NOT_USED;
{
	g.interrupted_p = 1;
	if (g.dbp)
		g.dbp->interrupt(g.dbp);
}

/*
 * This is the callback routine that is invoked
 * for each row of a query result.
 */
static int
callback(arg, num_args, args, cols)
	void *arg;
	int num_args;
	char **args;
	char **cols;
{
	int i;
	struct callback_data *p = (struct callback_data*)arg;
	switch(p->mode) {
	case MODE_Line: {
		int w = 5;
		if (args == 0)
			break;
		for(i = 0; i < num_args; i++) {
			int len = strlen(cols[i]);
			if (len > w)
				w = len;
		}
		if (p->cnt++ > 0)
			fprintf(p->out, "\n");
		for (i = 0; i < num_args; i++) {
			fprintf(p->out, "%*s = %s\n", w, cols[i], 
				(args[i] ? args[i] : p->nullvalue));
		}
		break;
	}
	case MODE_Column: {
		if (p->cnt++ == 0) {
			for(i = 0; i < num_args; i++) {
				int w, n;
				if (i < ARRAY_SIZE(p->col_width)) {
					w = p->col_width[i];
				} else {
					w = 0;
				}
				if (w <= 0) {
					w = strlen(cols[i] ? cols[i] : "");
					if (w < 10)
						w = 10;
					n = strlen(args && args[i] ?
						   args[i] :
						   p->nullvalue);
					if (w < n)
						w = n;
				}
				if (i < ARRAY_SIZE(p->actual_width)) {
					p->actual_width[i] = w;
				}
				if (p->show_header) {
					fprintf(p->out, "%-*.*s%s", w, w,
						cols[i], (i == num_args - 1 ?
							  "\n": "  "));
				}
			}
			if (p->show_header) {
				for(i = 0; i < num_args; i++) {
					int w;
					if (i < ARRAY_SIZE(p->actual_width)) {
						w = p->actual_width[i];
					} else {
						w = 10;
					}
					fprintf(p->out, "%-*.*s%s", w, w,
                 "----------------------------------------------------------"
		 "-----------------------------------",
						(i == num_args - 1) ?
						"\n" : "  ");
				}
			}
		}
		if (args == 0)
			break;
		for(i = 0; i < num_args; i++) {
			int w;
			if (i < ARRAY_SIZE(p->actual_width)) {
				w = p->actual_width[i];
			} else {
				w = 10;
			}
			fprintf(p->out, "%-*.*s%s", w, w,
				args[i] ? args[i] : p->nullvalue,
				i == num_args - 1 ? "\n" : "  ");
		}
		break;
	}
	case MODE_Semi: /* FALLTHROUGH */
	case MODE_List: {
		if (p->cnt++ == 0 && p->show_header) {
			for(i = 0; i < num_args; i++) {
				fprintf(p->out, "%s%s", cols[i],
					i == num_args - 1 ?
					"\n" : p->separator);
			}
		}
		if (args == 0)
			break;
		for(i = 0; i < num_args; i++) {
			char *z = args[i];
			if (z == 0)
				z = p->nullvalue;
			fprintf(p->out, "%s", z);
			if (i < num_args - 1) {
				fprintf(p->out, "%s", p->separator);
			} else if (p->mode == MODE_Semi) {
				fprintf(p->out, ";\n");
			} else {
				fprintf(p->out, "\n");
			}
		}
		break;
	}
	case MODE_Html: {
		if (p->cnt++ == 0 && p->show_header) {
			fprintf(p->out, "<TR>");
			for(i = 0; i < num_args; i++) {
				fprintf(p->out, "<TH>%s</TH>", cols[i]);
			}
			fprintf(p->out, "</TR>\n");
		}
		if (args == 0)
			break;
		fprintf(p->out, "<TR>");
		for(i = 0; i < num_args; i++) {
			fprintf(p->out, "<TD>");
			output_html_string(p->out,
					   args[i] ? args[i] : p->nullvalue);
			fprintf(p->out, "</TD>\n");
		}
		fprintf(p->out, "</TR>\n");
		break;
	}
	case MODE_Insert: {
		if (args == 0)
			break;
		fprintf(p->out, "INSERT INTO %s VALUES(", p->dest_table);
		for(i = 0; i < num_args; i++) {
			char *zSep = i>0 ? ",": "";
			if (args[i] == 0) {
				fprintf(p->out, "%sNULL", zSep);
			} else if (__str_is_numeric(args[i])) {
				fprintf(p->out, "%s%s", zSep, args[i]);
			} else {
				if (zSep[0])
					fprintf(p->out, "%s", zSep);
				output_quoted_string(p->out, args[i]);
			}
		}
		fprintf(p->out, ");\n");
		break;
	}
	}
	return 0;
}

/*
 * Set the destination table field of the callback_data structure to
 * the name of the table given.  Escape any quote characters in the
 * table name.
 */
static void
set_table_name(p, name)
	struct callback_data *p;
	const char *name;
{
	int i, n;
	int needQuote;
	char *z;

	if (p->dest_table) {
		free(p->dest_table);
		p->dest_table = 0;
	}
	if (name == 0)
		return;
	needQuote = !isalpha(*name) && *name!='_';
	for(i = n = 0; name[i]; i++, n++) {
		if (!isalnum(name[i]) && name[i] != '_' ) {
			needQuote = 1;
			if( name[i] == '\'' )
				n++;
		}
	}
	if (needQuote)
		n += 2;
	z = p->dest_table = malloc(n + 1);
	if (z == 0) {
		fprintf(stderr,"Out of memory!\n");
		exit(1);
	}
	n = 0;
	if (needQuote)
		z[n++] = '\'';
	for(i = 0; name[i]; i++) {
		z[n++] = name[i];
		if (name[i] == '\'')
			z[n++] = '\'';
	}
	if (needQuote)
		z[n++] = '\'';
	z[n] = 0;
}

/*
 * This is a different callback routine used for dumping the database.
 * Each row received by this callback consists of a table name,
 * the table type ("index" or "table") and SQL to create the table.
 * This routine should print text sufficient to recreate the table.
 */
static int
dump_callback(arg, num_args, args, cols)
	void *arg;
	int num_args;
	char **args;
	char **cols;
{
	struct callback_data *p = (struct callback_data *)arg;
	if (num_args != 3)
		return 1;
	fprintf(p->out, "%s;\n", args[2]);
	if (strcmp(args[1], "table") == 0) {
		struct callback_data d2;
		d2 = *p;
		d2.mode = MODE_Insert;
		d2.dest_table = 0;
		set_table_name(&d2, args[0]);
		p->db->exec_printf(p->db, "SELECT * FROM '%q'",
				   callback, &d2, 0, args[0]);
		set_table_name(&d2, 0);
	}
	return 0;
}

/*
 * Text of a help message
 */
static char help_message[] =
  ".databases             List names and files of attached databases\n"
  ".dump ?TABLE? ...      Dump the database in a text format\n"
  ".echo ON|OFF           Turn command echo on or off\n"
  ".exit                  Exit this program\n"
  ".explain ON|OFF        Turn output mode suitable for EXPLAIN on or off.\n"
  ".header(s) ON|OFF      Turn display of headers on or off\n"
  ".help                  Show this message\n"
  ".indices TABLE         Show names of all indices on TABLE\n"
  ".mode MODE             Set mode to one of \"line(s)\", \"column(s)\", \n"
  "                       \"insert\", \"list\", or \"html\"\n"
  ".mode insert TABLE     Generate SQL insert statements for TABLE\n"
  ".nullvalue STRING      Print STRING instead of nothing for NULL data\n"
  ".output FILENAME       Send output to FILENAME\n"
  ".output stdout         Send output to the screen\n"
  ".prompt MAIN CONTINUE  Replace the standard prompts\n"
  ".quit                  Exit this program\n"
  ".read FILENAME         Execute SQL in FILENAME\n"
  ".schema ?TABLE?        Show the CREATE statements\n"
  ".separator STRING      Change separator string for \"list\" mode\n"
  ".show                  Show the current values for various settings\n"
  ".tables ?PATTERN?      List names of tables matching a pattern\n"
  ".timeout MS            Try opening locked tables for MS milliseconds\n"
  ".width NUM NUM ...     Set column widths for \"column\" mode\n"
;

static void process_input(struct callback_data *p, FILE *in);

/*
 * Make sure the database is open.  If it is not, then open it.  If
 * the database fails to open, print an error message and exit.
 */
static void
open_db(p)
	struct callback_data *p;
{
	int rc;
	char *err_msgs = 0;
	const char *filename;
	int flags = DBSQL_THREAD; /* TODO | DBSQL_TEMP_INMEM; */

	if (p->db == 0) {
		filename = p->db_filename;
		if (!strcmp(p->db_filename, ":memory:"))
			filename = 0;
		rc = dbsql_create_env(&p->db, filename, p->crypt_key,0,flags);
		switch (rc) {
		case DB_RUNRECOVERY:
			fprintf(g.errfp, "Database requires recovery.\n");
			break;
		case 0:
			break;
		default:
			fprintf(g.errfp, dbsql_strerror(rc));
			exit(1);
		}
		g.dbenv = p->db->get_dbenv(p->db);
		p->db->set_errfile(p->db, g.errfp);
		p->db->set_errpfx(p->db, g.progname);
		if ((rc = p->db->open(p->db, p->db_filename, 0664,
				      &err_msgs)) != 0) {
			if (err_msgs) {
				fprintf(stderr,
					"Unable to open database \"%s\": %s\n",
					p->db_filename, err_msgs);
				free(err_msgs);
			} else {
				fprintf(stderr,
					"Unable to open database \"%s\"\n",
					p->db_filename);
			}
			exit(1);
		}
	}
}

/*
 * If an input line begins with "." then invoke this routine to
 * process that line.
 *
 * Return 1 to exit and 0 to continue.
 */
static int
do_meta_command(line, p)
	char *line;
	struct callback_data *p;
{
	int i = 1;
	int num_args = 0;
	int n, c;
	int rc = 0;
	char *args[50];

	/*
	 * Parse the input line into tokens.
	 */
	while(line[i] && num_args < ARRAY_SIZE(args)) {
		while(isspace(line[i])) {
			i++;
		}
		if (line[i] == '\'' || line[i] == '"') {
			int delim = line[i++];
			args[num_args++] = &line[i];
			while(line[i] && line[i] != delim) {
				i++;
			}
			if (line[i] == delim) {
				line[i++] = 0;
			}
		} else {
			args[num_args++] = &line[i];
			while(line[i] && !isspace(line[i])) {
				i++;
			}
			if (line[i])
				line[i++] = 0;
		}
	}

	/*
	 * Process the input line.
	 */
	if (num_args == 0)
		return rc;
	n = strlen(args[0]);
	c = args[0][0];
	if (c == 'd' && n > 1 && strncmp(args[0], "databases", n) == 0) {
		struct callback_data data;
		char *err_msgs = 0;
		open_db(p);
		memcpy(&data, p, sizeof(data));
		data.show_header = 0;
		data.mode = MODE_Column;
		p->db->exec(p->db, "PRAGMA database_list; ", callback,
			    &data, &err_msgs);
		if (err_msgs) {
			fprintf(stderr, "Error: %s\n", err_msgs);
			free(err_msgs);
		}
	} else if(c == 'd' && strncmp(args[0], "dump", n) == 0) {
		char *err_msgs = 0;
		open_db(p);
		fprintf(p->out, "BEGIN TRANSACTION;\n");
		if (num_args == 1) {
			p->db->exec(p->db,
		 "SELECT name, type, sql FROM " MASTER_NAME " "
		 "WHERE type!='meta' AND sql NOT NULL "
		 "ORDER BY substr(type, 2, 1), name",
					  dump_callback, p, &err_msgs);
		} else {
			int i;
			for(i = 1; i < num_args && err_msgs == 0; i++){
				p->db->exec_printf(p->db,
		  "SELECT name, type, sql FROM " MASTER_NAME " "
		  "WHERE tbl_name LIKE '%q' AND type!='meta' AND sql NOT NULL "
                  "ORDER BY substr(type,2,1), name",
						  dump_callback, p,
						  &err_msgs, args[i]);
			}
		}
		if (err_msgs) {
			fprintf(stderr, "Error: %s\n", err_msgs);
			free(err_msgs);
		} else {
			fprintf(p->out, "COMMIT;\n");
		}
	} else if (c=='e' && strncmp(args[0], "echo", n) == 0 && num_args > 1){
		int j;
		char *z = args[1];
		int val = atoi(args[1]);
		for(j = 0; z[j]; j++) {
			if (isupper(z[j]))
				z[j] = tolower(z[j]);
		}
		if (strcmp(z, "on") == 0) {
			val = 1;
		} else if (strcmp(z, "yes") == 0) {
			val = 1;
		}
		p->echoOn = val;
	} else if (c == 'e' && strncmp(args[0], "exit", n) == 0) {
		rc = 1;
	} else if (c=='e' && strncmp(args[0], "explain", n) == 0) {
		int j;
		char *z = num_args >= 2 ? args[1] : "1";
		int val = atoi(z);
		for(j = 0; z[j]; j++) {
			if (isupper(z[j]))
				z[j] = tolower(z[j]);
		}
		if (strcmp(z, "on") == 0) {
			val = 1;
		} else if (strcmp(z, "yes") == 0) {
			val = 1;
		}
		if (val == 1) {
			if (!p->explainPrev.valid) {
				p->explainPrev.valid = 1;
				p->explainPrev.mode = p->mode;
				p->explainPrev.show_header = p->show_header;
				memcpy(p->explainPrev.col_width,
				       p->col_width,sizeof(p->col_width));
			}
			/*
			 * We could put this code under the !p->explainValid
			 * condition so that it does not execute if we are
			 * already in explain mode. However, always executing
			 * it allows us an easy was to reset to explain mode
			 * in case the user previously did an .explain followed
			 * by a .width, .mode or .header command.
			 */
			p->mode = MODE_Column;
			p->show_header = 1;
			memset(p->col_width, 0, ARRAY_SIZE(p->col_width));
			p->col_width[0] = 4;
			p->col_width[1] = 12;
			p->col_width[2] = 10;
			p->col_width[3] = 10;
			p->col_width[4] = 35;
		} else if (p->explainPrev.valid) {
			p->explainPrev.valid = 0;
			p->mode = p->explainPrev.mode;
			p->show_header = p->explainPrev.show_header;
			memcpy(p->col_width, p->explainPrev.col_width,
			       sizeof(p->col_width));
		}
	} else if (c == 'h' && (strncmp(args[0], "header", n) == 0 ||
				strncmp(args[0], "headers", n) == 0) &&
		   num_args > 1) {
		int j;
		char *z = args[1];
		int val = atoi(args[1]);
		for(j = 0; z[j]; j++) {
			if (isupper(z[j]))
				z[j] = tolower(z[j]);
		}
		if (strcmp(z, "on") == 0) {
			val = 1;
		} else if (strcmp(z, "yes") == 0) {
			val = 1;
		}
		p->show_header = val;
	} else if (c == 'h' && strncmp(args[0], "help", n) == 0) {
		fprintf(stderr, help_message);
	} else if(c == 'i' && strncmp(args[0], "indices", n) == 0 &&
		  num_args > 1) {
		struct callback_data data;
		char *err_msgs = 0;
		open_db(p);
		memcpy(&data, p, sizeof(data));
		data.show_header = 0;
		data.mode = MODE_List;
		p->db->exec_printf(p->db,
				  "SELECT name FROM " MASTER_NAME " "
				  "WHERE type='index' AND tbl_name LIKE '%q' "
				  "UNION ALL "
				  "SELECT name FROM " TEMP_MASTER_NAME " "
				  "WHERE type='index' AND tbl_name LIKE '%q' "
				  "ORDER BY 1",
				  callback, &data, &err_msgs, args[1],args[1]);
		if (err_msgs) {
			fprintf(stderr, "Error: %s\n", err_msgs);
			free(err_msgs);
		}
	} else if (c == 'm' && strncmp(args[0], "mode", n) == 0 &&
		   num_args >= 2) {
		int n2 = strlen(args[1]);
		if (strncmp(args[1], "line", n2) == 0 ||
		    strncmp(args[1], "lines", n2) == 0) {
			p->mode = MODE_Line;
		} else if (strncmp(args[1], "column", n2) == 0 ||
			   strncmp(args[1], "columns", n2) == 0) {
			p->mode = MODE_Column;
		} else if (strncmp(args[1], "list", n2) == 0) {
			p->mode = MODE_List;
		} else if (strncmp(args[1], "html", n2) == 0) {
			p->mode = MODE_Html;
		} else if (strncmp(args[1], "insert", n2) == 0) {
			p->mode = MODE_Insert;
			if (num_args >= 3) {
				set_table_name(p, args[2]);
			} else {
				set_table_name(p, "table");
			}
		} else {
			fprintf(stderr, "mode should be on of: column "
				"html insert line list\n");
		}
	} else if(c == 'n' && strncmp(args[0], "nullvalue", n) == 0 &&
		  num_args == 2) {
		sprintf(p->nullvalue, "%.*s", (int)ARRAY_SIZE(p->nullvalue)-1,
			args[1]);
	} else if (c == 'o' && strncmp(args[0], "output", n) == 0 &&
		   num_args == 2) {
		if (p->out != stdout) {
			fclose(p->out);
		}
		if (strcmp(args[1], "stdout") == 0) {
			p->out = stdout;
			strcpy(p->outfile, "stdout");
		} else {
			p->out = fopen(args[1], "w");
			if (p->out == 0) {
				fprintf(stderr, "can't write to \"%s\"\n",
					args[1]);
				p->out = stdout;
			} else {
				strcpy(p->outfile,args[1]);
			}
		}
	} else if (c == 'p' && strncmp(args[0], "prompt", n) == 0 &&
		   (num_args == 2 || num_args == 3)) {
		if (num_args >= 2) {
			strncpy(g.prompt, args[1],
				(int)ARRAY_SIZE(g.prompt) - 1);
		}
		if (num_args >= 3) {
			strncpy(g.prompt2, args[2],
				(int)ARRAY_SIZE(g.prompt2) - 1);
		}
	} else if (c == 'q' && strncmp(args[0], "quit", n) == 0) {
		if (p->db) {
			p->db->close(p->db);
		}
		g.dbenv->close(g.dbenv, 0);
		exit(0);
	} else if (c == 'r' && strncmp(args[0], "read", n) == 0 &&
		   num_args == 2) {
		FILE *alt = fopen(args[1], "r");
		if (alt == 0) {
			fprintf(stderr, "can't open \"%s\"\n", args[1]);
		} else {
			process_input(p, alt);
			fclose(alt);
		}
	} else
	if (c == 's' && strncmp(args[0], "schema", n) == 0) {
		struct callback_data data;
		char *err_msgs = 0;
		open_db(p);
		memcpy(&data, p, sizeof(data));
		data.show_header = 0;
		data.mode = MODE_Semi;
		if (num_args > 1) {
			extern int strcasecmp(const char*,
							 const char*);
			if (strcasecmp(args[1], MASTER_NAME) == 0) {
			       char *new_argv[2], *new_colv[2];
			       new_argv[0] = "CREATE TABLE " MASTER_NAME " (\n"
				       "  type text,\n"
				       "  name text,\n"
				       "  tbl_name text,\n"
				       "  rootpage integer,\n"
				       "  sql text\n"
				       ")";
			       new_argv[1] = 0;
			       new_colv[0] = "sql";
			       new_colv[1] = 0;
			       callback(&data, 1, new_argv, new_colv);
			} else if (strcasecmp(args[1],
						      TEMP_MASTER_NAME) == 0) {
				char *new_argv[2], *new_colv[2];
				new_argv[0] = "CREATE TEMP TABLE "
					TEMP_MASTER_NAME " (\n"
					"  type text,\n"
					"  name text,\n"
					"  tbl_name text,\n"
					"  rootpage integer,\n"
					"  sql text\n"
					")";
				new_argv[1] = 0;
				new_colv[0] = "sql";
				new_colv[1] = 0;
				callback(&data, 1, new_argv, new_colv);
			} else {
				p->db->exec_printf(p->db,
		   "SELECT sql FROM "
		   "  (SELECT * FROM " MASTER_NAME " UNION ALL"
		   "   SELECT * FROM " TEMP_MASTER_NAME ") "
		   "WHERE tbl_name LIKE '%q' AND type!='meta' AND sql NOTNULL "
		   "ORDER BY substr(type,2,1), name",
						  callback, &data, &err_msgs,
						  args[1]);
			}
		} else {
			p->db->exec(p->db,
				   "SELECT sql FROM "
				   "  (SELECT * FROM " MASTER_NAME " UNION ALL"
				   "   SELECT * FROM " TEMP_MASTER_NAME ") "
				   "WHERE type!='meta' AND sql NOTNULL "
				   "ORDER BY substr(type,2,1), name",
				   callback, &data, &err_msgs);
		}
		if (err_msgs) {
			fprintf(stderr, "Error: %s\n", err_msgs);
			free(err_msgs);
		}
	} else if (c == 's' && strncmp(args[0], "separator", n) == 0 &&
		   num_args == 2) {
		sprintf(p->separator, "%.*s", (int)ARRAY_SIZE(p->separator)-1,
			args[1]);
	} else if (c == 's' && strncmp(args[0], "show", n) == 0) {
		int i;
		fprintf(p->out,"%9.9s: %s\n","echo",
			p->echoOn ? "on" : "off");
		fprintf(p->out,"%9.9s: %s\n","explain",
			p->explainPrev.valid ? "on" :"off");
		fprintf(p->out,"%9.9s: %s\n","headers",
			p->show_header ? "on" : "off");
		fprintf(p->out,"%9.9s: %s\n","mode", modeDescr[p->mode]);
		fprintf(p->out,"%9.9s: %s\n","nullvalue", p->nullvalue);
		fprintf(p->out,"%9.9s: %s\n","output",
			strlen(p->outfile) ? p->outfile : "stdout");
		fprintf(p->out,"%9.9s: %s\n","separator", p->separator);
		fprintf(p->out,"%9.9s: ","width");
		for (i = 0; i < (int)ARRAY_SIZE(p->col_width) &&
			     p->col_width[i] != 0; i++) {
			fprintf(p->out, "%d ", p->col_width[i]);
		}
		fprintf(p->out, "\n\n");
	} else if (c == 't' && n > 1 && strncmp(args[0], "tables", n) == 0) {
		char **results;
		int nRow, rc;
		char *err_msgs;
		open_db(p);
		if (num_args == 1) {
			rc = p->db->get_table(p->db,
				       "SELECT name FROM " MASTER_NAME " "
				       "WHERE type IN ('table','view') "
				       "UNION ALL "
				       "SELECT name FROM " TEMP_MASTER_NAME " "
				       "WHERE type IN ('table','view') "
				       "ORDER BY 1",
					     &results, &nRow, 0, &err_msgs);
		} else {
			rc = p->db->exec_table_printf(p->db,
		       "SELECT name FROM " MASTER_NAME " "
		       "WHERE type IN ('table','view') AND name LIKE '%%%q%%' "
		       "UNION ALL "
		       "SELECT name FROM " TEMP_MASTER_NAME " "
		       "WHERE type IN ('table','view') AND name LIKE '%%%q%%' "
		       "ORDER BY 1",
						    &results, &nRow, 0,
						    &err_msgs, args[1],
						    args[1]);
		}
		if( err_msgs ) {
			fprintf(stderr,"Error: %s\n", err_msgs);
			free(err_msgs);
		}
		if (rc == DBSQL_SUCCESS) {
			int len, maxlen = 0;
			int i, j;
			int num_print_col, num_print_row;
			for(i = 1; i <= nRow; i++) {
				if (results[i] == 0)
					continue;
				len = strlen(results[i]);
				if (len > maxlen)
					maxlen = len;
			}
			num_print_col = 80 / (maxlen + 2);
			if (num_print_col < 1)
				num_print_col = 1;
			num_print_row = (nRow + num_print_col - 1) /
				num_print_col;
			for(i = 0; i < num_print_row; i++) {
				for(j = i + 1; j <= nRow; j += num_print_row) {
					char *sp = (j <= num_print_row ? "" : "  ");
					printf("%s%-*s", sp, maxlen,
					       results[j] ? results[j] : "");
				}
				printf("\n");
			}
		}
		p->db->free_table(results);
	} else if (c == 't' && n>1 && strncmp(args[0], "timeout", n) == 0 &&
		   num_args >= 2) {
		open_db(p);
		p->db->set_timeout(p->db, atoi(args[1]));
	} else if (c == 'w' && strncmp(args[0], "width", n) == 0) {
		int j;
		for(j = 1; j < num_args && j < ARRAY_SIZE(p->col_width); j++) {
			p->col_width[j - 1] = atoi(args[j]);
		}
	} else {
		fprintf(stderr, "unknown command or invalid arguments: "
			" \"%s\". Enter \".help\" for help\n", args[0]);
	}
	return rc;
}

/*
 * Return TRUE if the last non-whitespace character in z[] is a semicolon.
 * z[] is N characters long.
 */
static int
_ends_with_semicolon(z, n)
	const char *z;
	int n;
{
	while(n > 0 && isspace(z[n - 1])) {
		n--;
	}
	return n > 0 && z[n - 1] == ';';
}

/*
 * Test to see if a line consists entirely of whitespace.
 */
static int
_all_whitespace(z)
	const char *z;
{
	for(; *z; z++) {
		if (isspace(*z))
			continue;
		if (*z == '/' && z[1] == '*') {
			z += 2;
			while(*z && (*z != '*' || z[1] != '/')) {
				z++;
			}
			if (*z == 0)
				return 0;
			z++;
			continue;
		}
		if (*z == '-' && z[1] == '-') {
			z += 2;
			while(*z && *z != '\n') {
				z++;
			}
			if (*z == 0)
				return 1;
			continue;
		}
		return 0;
	}
	return 1;
}

/*
 * Return TRUE if the line typed in is an SQL command terminator other
 * than a semi-colon.  The SQL Server style "go" command is understood
 * as is the Oracle "/".
 */
static int
_is_command_terminator(line)
	const char *line;
{
	extern int strncasecmp(const char*,const char*,size_t);
	while(isspace(*line)) {
		line++;
	};
	if (line[0] == '/' && _all_whitespace(&line[1]))
		return 1;  /* Oracle */
	if (strncasecmp(line, "go", 2) ==0 &&
	    _all_whitespace(&line[2])) {
		return 1;  /* SQL Server */
	}
	return 0;
}

/*
 * Read input from *in and process it.  If *in==0 then input
 * is interactive - the user is typing it it.  Otherwise, input
 * is coming from a file or device.  A prompt is issued and history
 * is saved only if input is interactive.  An interrupt signal will
 * cause this routine to exit immediately, unless input is interactive.
 */
static void
process_input(p, in)
	struct callback_data *p;
	FILE *in;
{
	char *line;
	char *sql = 0;
	int line_len = 0;
	char *err_msgs;
	int rc;
	while(fflush(p->out), (line = one_input_line(sql, in)) != 0) {
		if (g.interrupted_p) {
			if (in != 0)
				break;
			g.interrupted_p = 0;
		}
		if (p->echoOn)
			printf("%s\n", line);
		if ((sql == 0 || sql[0] == 0) &&
		    _all_whitespace(line))
			continue;
		if (line && line[0] == '.' && line_len == 0) {
			int rc = do_meta_command(line, p);
			free(line);
			if (rc)
				break;
			continue;
		}
		if (_is_command_terminator(line)) {
			strcpy(line,";");
		}
		if (sql == 0) {
			int i;
			for(i = 0; line[i] && isspace(line[i]); i++) {}
			if (line[i] != 0) {
				line_len = strlen(line);
				sql = malloc(line_len + 1);
				strcpy(sql, line);
			}
		} else {
			int len = strlen(line);
			sql = realloc(sql, line_len + len + 2 );
			if (sql == 0) {
				fprintf(stderr,"%s: out of memory!\n",
					g.progname);
				exit(1);
			}
			strcpy(&sql[line_len++], "\n");
			strcpy(&sql[line_len], line);
			line_len += len;
		}
		free(line);
		if (sql && _ends_with_semicolon(sql, line_len) &&
		    dbsql_complete_stmt(sql)) {
			p->cnt = 0;
			open_db(p);
			rc = p->db->exec(p->db, sql, callback, p, &err_msgs);
			if (rc || err_msgs) {
				if (in != 0 && !p->echoOn)
					printf("%s\n",sql);
				if (err_msgs != 0) {
					printf("SQL error: %s\n", err_msgs);
					free(err_msgs);
					err_msgs = 0;
				} else {
					printf("SQL error: %s\n",
					       dbsql_strerror(rc));
				}
			}
			free(sql);
			sql = 0;
			line_len = 0;
		}
	}
	if (sql) {
		if (!_all_whitespace(sql))
			printf("Incomplete SQL: %s\n", sql);
		free(sql);
	}
}

/*
 * Return a pathname which is the user's home directory.  A
 * 0 return indicates an error of some kind.  Space to hold the
 * resulting string is obtained from malloc().  The calling
 * function should free the result.
 */
static char *
find_home_dir(void)
{
	char *home_dir = NULL;

#if !defined(_WIN32) && !defined(WIN32)
	struct passwd *pwent;
	uid_t uid = getuid();
	if ((pwent = getpwuid(uid)) != NULL) {
		home_dir = pwent->pw_dir;
	}
#endif

	if (!home_dir) {
		home_dir = getenv("HOME");
		if (!home_dir) {
			home_dir = getenv("HOMEPATH"); /* Windows? */
		}
	}

#if defined(_WIN32) || defined(WIN32)
	if (!home_dir) {
		home_dir = "c:";
	}
#endif

	if (home_dir) {
		char *z = malloc(strlen(home_dir) + 1);
		if (z)
			strcpy(z, home_dir);
		home_dir = z;
	}
	return home_dir;
}

/*
 * Read input from the file given by rc_override.  Or if that
 * parameter is NULL, take input from ~/.dbsqlrc
 */
static void
process_rc(p, rc_override) 
	struct callback_data *p;        /* Configuration data */
	const char *rc_override;        /* Name of config file.  NULL to use
					   default */
{
	char *home_dir = NULL;
	const char *rc = rc_override;
	char *buf;
	FILE *in = NULL;

	if (rc == NULL) {
		home_dir = find_home_dir();
		if (home_dir == 0) {
			fprintf(stderr,"%s: unable to locate home directory\n",
				g.progname);
			return;
		}
		buf = malloc(strlen(home_dir) + 15);
		if (buf == 0) {
			fprintf(stderr,"%s: out of memory\n", g.progname);
			exit(1);
		}
		sprintf(buf,"%s/.dbsqlrc", home_dir);
		free(home_dir);
		rc = (const char*)buf;
	}
	in = fopen(rc, "r");
	if (in) {
		if (isatty(fileno(stdout))) {
			printf("Loading resources from %s\n", rc);
		}
		process_input(p, in);
		fclose(in);
	}
	return;
}

/*
** Show available command line options
*/
static const char options[] = 
  "\t--init filename       read/process named file\n"
  "\t--echo                print commands before execution\n"
  "\t--[no]header          turn headers on or off\n"
  "\t--column              set output mode to 'column'\n"
  "\t--html                set output mode to HTML\n"
  "\t--line                set output mode to 'line'\n"
  "\t--list                set output mode to 'list'\n"
  "\t--separator 'x'       set output field separator (|)\n"
  "\t--nullvalue 'text'    set text string for NULL values\n"
  "\t--version             show DBSQL version\n"
  "\t--help                show this text\n"
;

/*
 *
 */
static void
usage(show_detail)
	int show_detail;
{
	fprintf(stderr, "Usage: db_isql [OPTIONS] FILENAME [SQL]\n");
	if (show_detail) {
		fprintf(stderr, "%s", options);
	} else {
		fprintf(stderr, "Use the --help option for additional "
			"information\n");
	}
	exit(1);
}

/*
 * Initialize the state information in data.
 */
void
main_init(data)
	struct callback_data *data;
{
	memset(data, 0, sizeof(*data));
	data->mode = MODE_List;
	strcpy(data->separator,"|");
	data->show_header = 0;
	strcpy(g.prompt,"SQL> ");
	strcpy(g.prompt2,"...>");
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	char *err_msgs = 0;
	struct callback_data data;
	const char *init_file = 0;
	char *first_cmd = 0;
	int i;

	memset(&g, 0, sizeof(struct globals));
	g.progname = argv[0];
	g.errfp = stderr;
	main_init(&data);

	/*
	 * Make sure we have a valid signal handler early, before anything
	 * else is done.
	 */
#ifdef SIGINT
	signal(SIGINT, interrupt_handler);
#endif

	/*
	 * Do an initial pass through the command-line argument to locate
	 * the name of the database file, the name of the initialization file,
	 * and the first command to execute.
	 */
	for(i = 1; i <= argc - 1; i++) {
		if (argv[i][0] != '-')
			break;
		if (strcmp(argv[i], "--separator") == 0 ||
		    strcmp(argv[i], "--nullvalue") == 0) {
			i++;
		} else if (strcmp(argv[i], "--init") == 0) {
			i++;
			init_file = argv[i];
		} else if (strcmp(argv[i], "--key") ==0 ){
			char new[1024];
			i++;
			snprintf(new, sizeof(new), "%s", argv[i]);
			__dbsql_strdup(NULL, data.crypt_key, new);
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(1);
		}
	}
	if (i < argc) {
		data.db_filename = argv[i++];
	} else {
		data.db_filename = ":memory:";
	}
	if(i < argc) {
		first_cmd = argv[i++];
	}
	data.out = stdout;

	open_db(&data);

	/*
	 * Process the initialization file if there is one.  If no -init option
	 * is given on the command line, look for a file named ~/.dbsqlrc and
	 * try to process it.
	 */
	process_rc(&data, init_file);

	/*
	 * Make a second pass through the command-line argument and set
	 * options.  This second pass is delayed until after the initialization
	 * file is processed so that the command-line arguments will override
	 * settings in the initialization file.
	 */
	for(i = 1; i < argc && argv[i][0] == '-'; i++) {
		char *z = argv[i];
		if (strcmp(z, "--init") == 0 || strcmp(z, "--key") == 0) {
			i++;
		} else if (strcmp(z, "--html") == 0) {
			data.mode = MODE_Html;
		} else if (strcmp(z, "--list") == 0) {
			data.mode = MODE_List;
		} else if (strcmp(z, "--line") == 0) {
			data.mode = MODE_Line;
		} else if (strcmp(z, "--column") == 0) {
			data.mode = MODE_Column;
		} else if (strcmp(z, "--separator") == 0) {
			i++;
			sprintf(data.separator, "%.*s",
				(int)sizeof(data.separator) - 1, argv[i]);
		} else if (strcmp(z, "--nullvalue") == 0) {
			i++;
			sprintf(data.nullvalue, "%.*s",
				(int)sizeof(data.nullvalue) - 1, argv[i]);
		} else if (strcmp(z, "--header") == 0) {
			data.show_header = 1;
		} else if (strcmp(z, "--noheader") == 0) {
			data.show_header = 0;
		} else if (strcmp(z, "--echo") == 0) {
			data.echoOn = 1;
		} else if (strcmp(z, "--version") == 0) {
			int major, minor, patch;
			printf("%s\n", dbsql_version(&major, &minor, &patch));
			return 1;
		} else {
			fprintf(stderr,"%s: unknown option: %s\n",
				g.progname, z);
			fprintf(stderr,"Use --help for a list of options.\n");
			return 1;
		}
	}

	if (first_cmd) {
		/*
		 * Run just the command that follows the database name.
		 */
		if (first_cmd[0] == '.') {
			do_meta_command(first_cmd, &data);
			exit(0);
		} else {
			int rc;
			open_db(&data);
			rc = g.dbp->exec(g.dbp, first_cmd, callback,
					&data, &err_msgs);
			if (rc != 0 && err_msgs != 0) {
				fprintf(stderr,"SQL error: %s\n", err_msgs);
				exit(1);
			}
		}
	} else {
		/*
		 * Run commands received from standard input.
		 */
		if (isatty(fileno(stdout)) && isatty(fileno(stdin))) {
			int major, minor, patch;
			char *home;
			char *history = 0;
			printf("%s\nEnter \".help\" for instructions\n",
			       dbsql_version(&major, &minor, &patch));
			home = find_home_dir();
			if (home &&
			    (history = malloc(strlen(home) + 20)) != 0) {
				sprintf(history, "%s/.dbsql_history", home);
			}
			if (history)
				read_history(history);
			process_input(&data, 0);
			if (history) {
				stifle_history(100);
				write_history(history);
			}
		} else {
			process_input(&data, stdin);
		}
	}
	set_table_name(&data, 0);
	if (g.dbp) {
		g.dbenv->close(g.dbenv, 0);
		g.dbp->close(g.dbp);
	}
	return 0;
}


int
version_check()
{
        int v_major, v_minor, v_patch;

        /* Make sure we're loaded with the right version of the DB library. */
        (void)dbsql_version(&v_major, &v_minor, &v_patch);
        if (v_major != DBSQL_VERSION_MAJOR || v_minor != DBSQL_VERSION_MINOR) {
                fprintf(stderr,
			"%s: version %d.%d doesn't match library version %d.%d\n",
			g.progname, DBSQL_VERSION_MAJOR, DBSQL_VERSION_MINOR,
			v_major, v_minor);
                return (EXIT_FAILURE);
        }

        /* Make sure we're loaded with the right version of the DB library. */
        (void)db_version(&v_major, &v_minor, &v_patch);
        if (v_major != DB_VERSION_MAJOR || v_minor != DB_VERSION_MINOR) {
                fprintf(stderr,
			"%s: version %d.%d doesn't match library version %d.%d\n",
			g.progname, DB_VERSION_MAJOR, DB_VERSION_MINOR,
			v_major, v_minor);
                return (EXIT_FAILURE);
        }
        return (0);
}
