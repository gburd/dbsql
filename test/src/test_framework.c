/**
 * DBSQL Test Framework Implementation
 */

#include "test_framework.h"
#include <assert.h>
#include <glob.h>
#include <unistd.h>

/* Global test context */
test_context_t g_test_ctx = {0};

/* Test registry (linked list) */
static test_entry_t *test_registry_head = NULL;
static test_entry_t *test_registry_tail = NULL;

/**
 * test_register - Register a test function
 */
void
test_register(const char *name, test_func_t func)
{
	test_entry_t *entry = malloc(sizeof(test_entry_t));
	assert(entry != NULL);

	entry->name = name;
	entry->func = func;
	entry->next = NULL;

	if (test_registry_tail) {
		test_registry_tail->next = entry;
		test_registry_tail = entry;
	} else {
		test_registry_head = test_registry_tail = entry;
	}
}

/**
 * test_run_all - Run all registered tests
 */
void
test_run_all(void)
{
	test_entry_t *entry = test_registry_head;

	while (entry) {
		fprintf(g_test_ctx.tap_output, "# Running test: %s\n", entry->name);
		entry->func();
		entry = entry->next;
	}
}

/**
 * test_cleanup_db_files - Remove all database files
 * This ensures tests start with a clean slate and don't fail due to
 * leftover files from previous runs.
 */
void
test_cleanup_db_files(const char *pattern)
{
	glob_t globbuf;
	size_t i;
	const char *patterns[] = {
		"*.db",
		"*.db_*",
		"__db.*",
		NULL
	};

	(void)pattern; /* Unused for now, clean all db files */

	for (i = 0; patterns[i] != NULL; i++) {
		if (glob(patterns[i], GLOB_NOSORT, NULL, &globbuf) == 0) {
			size_t j;
			for (j = 0; j < globbuf.gl_pathc; j++) {
				unlink(globbuf.gl_pathv[j]);
			}
			globfree(&globbuf);
		}
	}
}

#ifdef TEST_TRACK_MEMORY
/* Memory tracking structures */
typedef struct mem_block {
	void *ptr;
	size_t size;
	const char *file;
	int line;
	struct mem_block *next;
} mem_block_t;

static mem_block_t *mem_blocks = NULL;
static size_t total_allocated = 0;
static size_t total_freed = 0;
static size_t current_allocated = 0;
static size_t peak_allocated = 0;

static void
mem_track_alloc(void *ptr, size_t size, const char *file, int line)
{
	mem_block_t *block = (mem_block_t *)malloc(sizeof(mem_block_t));
	block->ptr = ptr;
	block->size = size;
	block->file = file;
	block->line = line;
	block->next = mem_blocks;
	mem_blocks = block;

	total_allocated += size;
	current_allocated += size;
	if (current_allocated > peak_allocated) {
		peak_allocated = current_allocated;
	}
}

static void
mem_track_free(void *ptr)
{
	mem_block_t **p = &mem_blocks;

	while (*p) {
		if ((*p)->ptr == ptr) {
			mem_block_t *block = *p;
			*p = block->next;

			total_freed += block->size;
			current_allocated -= block->size;
			free(block);
			return;
		}
		p = &(*p)->next;
	}

	fprintf(stderr, "WARNING: Attempt to free untracked pointer %p\n", ptr);
}

void *
test_malloc(size_t size, const char *file, int line)
{
	void *ptr = malloc(size);
	if (ptr) {
		mem_track_alloc(ptr, size, file, line);
	}
	return ptr;
}

void *
test_calloc(size_t nmemb, size_t size, const char *file, int line)
{
	void *ptr = calloc(nmemb, size);
	if (ptr) {
		mem_track_alloc(ptr, nmemb * size, file, line);
	}
	return ptr;
}

void *
test_realloc(void *ptr, size_t size, const char *file, int line)
{
	if (ptr) {
		mem_track_free(ptr);
	}
	ptr = realloc(ptr, size);
	if (ptr) {
		mem_track_alloc(ptr, size, file, line);
	}
	return ptr;
}

void
test_free(void *ptr, const char *file, int line)
{
	if (ptr) {
		mem_track_free(ptr);
		free(ptr);
	}
}

void
test_memory_report(void)
{
	fprintf(g_test_ctx.tap_output, "# Memory Report:\n");
	fprintf(g_test_ctx.tap_output, "#   Total allocated: %zu bytes\n", total_allocated);
	fprintf(g_test_ctx.tap_output, "#   Total freed: %zu bytes\n", total_freed);
	fprintf(g_test_ctx.tap_output, "#   Peak usage: %zu bytes\n", peak_allocated);
	fprintf(g_test_ctx.tap_output, "#   Current: %zu bytes\n", current_allocated);

	if (mem_blocks) {
		fprintf(g_test_ctx.tap_output, "# Memory leaks detected:\n");
		mem_block_t *block = mem_blocks;
		while (block) {
			fprintf(g_test_ctx.tap_output, "#   %zu bytes at %p (%s:%d)\n",
				block->size, block->ptr, block->file, block->line);
			block = block->next;
		}
	} else {
		fprintf(g_test_ctx.tap_output, "# No memory leaks detected\n");
	}
}
#endif /* TEST_TRACK_MEMORY */
