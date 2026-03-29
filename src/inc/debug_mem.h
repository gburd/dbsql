#ifndef DEBUG_MEM_H
#define DEBUG_MEM_H

#ifdef DEBUG_MEMORY

#include <stdio.h>
#include <string.h>

/* Memory canary values */
#define MEM_CANARY_ALIVE 0xCAFEBABE
#define MEM_CANARY_FREED 0xDEADBEEF
#define MEM_FILL_ALLOC   0xCD  /* Clean memory - new allocation */
#define MEM_FILL_FREE    0xDD  /* Dead memory - freed */

/* Track memory blocks */
typedef struct mem_debug_info {
    void *ptr;
    size_t size;
    unsigned int canary_before;
    unsigned int canary_after;
    const char *file;
    int line;
    int alloc_num;
} mem_debug_info_t;

#define MAX_MEM_BLOCKS 10000
static mem_debug_info_t mem_blocks[MAX_MEM_BLOCKS];
static int mem_block_count = 0;
static int alloc_counter = 0;

/* Register an allocation */
static inline void mem_debug_register(void *ptr, size_t size, const char *file, int line) {
    if (mem_block_count >= MAX_MEM_BLOCKS) return;
    
    mem_blocks[mem_block_count].ptr = ptr;
    mem_blocks[mem_block_count].size = size;
    mem_blocks[mem_block_count].canary_before = MEM_CANARY_ALIVE;
    mem_blocks[mem_block_count].canary_after = MEM_CANARY_ALIVE;
    mem_blocks[mem_block_count].file = file;
    mem_blocks[mem_block_count].line = line;
    mem_blocks[mem_block_count].alloc_num = ++alloc_counter;
    mem_block_count++;
    
    fprintf(stderr, "[MEM_ALLOC #%d] %p size=%zu at %s:%d\n", 
        alloc_counter, ptr, size, file, line);
}

/* Unregister an allocation */
static inline void mem_debug_unregister(void *ptr, const char *file, int line) {
    int i;
    for (i = 0; i < mem_block_count; i++) {
        if (mem_blocks[i].ptr == ptr) {
            fprintf(stderr, "[MEM_FREE #%d] %p (allocated at %s:%d) freed at %s:%d\n",
                mem_blocks[i].alloc_num, ptr, 
                mem_blocks[i].file, mem_blocks[i].line,
                file, line);
            
            /* Check if already freed */
            if (mem_blocks[i].canary_before == MEM_CANARY_FREED) {
                fprintf(stderr, "[MEM_ERROR] DOUBLE FREE detected at %s:%d\n", file, line);
                fprintf(stderr, "  Block %p was allocated at %s:%d\n",
                    ptr, mem_blocks[i].file, mem_blocks[i].line);
            }
            
            mem_blocks[i].canary_before = MEM_CANARY_FREED;
            mem_blocks[i].canary_after = MEM_CANARY_FREED;
            
            /* Remove from tracking */
            mem_blocks[i] = mem_blocks[mem_block_count - 1];
            mem_block_count--;
            return;
        }
    }
    fprintf(stderr, "[MEM_ERROR] FREE of untracked pointer %p at %s:%d\n", 
        ptr, file, line);
}

/* Check if pointer is tracked */
static inline int mem_debug_check(void *ptr) {
    int i;
    for (i = 0; i < mem_block_count; i++) {
        if (mem_blocks[i].ptr == ptr) {
            if (mem_blocks[i].canary_before != MEM_CANARY_ALIVE ||
                mem_blocks[i].canary_after != MEM_CANARY_ALIVE) {
                fprintf(stderr, "[MEM_ERROR] Corrupted block %p (alloc #%d at %s:%d)\n",
                    ptr, mem_blocks[i].alloc_num, 
                    mem_blocks[i].file, mem_blocks[i].line);
                return 0;
            }
            return 1;
        }
    }
    return 0;
}

#endif /* DEBUG_MEMORY */

#endif /* DEBUG_MEM_H */
