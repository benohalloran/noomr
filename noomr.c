#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include "noomr.h"
#include "memmap.h"
#include "stack.h"


#define max(a, b) ((a) > (b) ? (a) : (b))

shared_data_t * shared;
size_t my_growth;

inline bool speculating(void) {
  return true; //TODO implement
}

static inline bool out_of_range(void * payload) {
  return payload < (void*) shared->base ||
    payload >= (void *) (((char*)shared->base) + shared->spec_growth);
}

static inline block_t * getblock(void * user_payload) {
  return (block_t *) (((char*) user_payload) - sizeof(block_t));
}

static inline void * getpayload(block_t * block) {
  return (void *) (((char*) block) + sizeof(block_t));
}

static inline header_t * lookup_header(void * user_payload) {
  return out_of_range(user_payload) ? NULL : getblock(user_payload)->header;
}

static inline stack_t * get_stack_of_size(size_t size) {
  size_t class = SIZE_TO_CLASS(ALIGN(size));
  return speculating() ? &shared->spec_free[class] : &shared->seq_free[class];
}

void synch_lists() {
  size_t i;
  volatile header_page_t * page;
  for (page = shared->firstpg; page != NULL; page = (header_page_t *) page->next) {
    for (i = 0; i < page->next_free; i++) {
      page->headers[i].spec_next = page->headers[i].seq_next;
    }
  }
}

// This step can be eliminated by sticking the two items in an array and swaping the index
// mapping to spec / seq free lists
// pre-spec is still needed OR 3x wide CAS operations -- 2 for the pointers
// (which need to be adjacent to each other) and another for the counter
static void promote_list() {
  size_t i;
  volatile header_page_t * page;
  for (page = shared->firstpg; page != NULL; page = (header_page_t *) page->next) {
    for (i = 0; i < page->next_free; i++) {
      page->headers[i].seq_next = page->headers[i].spec_next;
    }
  }
}

static header_page_t * payload_to_header_page(void * payload) {
  if (out_of_range(payload)) {
    return NULL;
  } else {
    block_t * block = getblock(payload);
    return  (header_page_t *) (((intptr_t) block) & ~(PAGE_SIZE - 1));
  }
}

static inline size_t noomr_usable_space(void * payload) {
  return getblock(payload)->header->size;
}


/**
 NOTE: when in spec the headers pointers will not be
 into all process's private address space
 To solve this, promise all spec-growth regions
*/
static header_page_t * lastheaderpg() {
  volatile header_page_t * page = shared->firstpg;
  if (page == NULL) {
    return NULL;
  }
  for (; page->next != NULL; page = (header_page_t *) page->next) {
    ;
  }
  assert(page != NULL);
  return (header_page_t *) page;
}

void allocate_header_page() {
  header_page_t * hpg = NULL; //TODO map
  hpg->next_free = 0;
  hpg->next = NULL;
  ; //TODO implement
}

static inline void map_headers(char * begin, size_t block_size, size_t num_blocks) {
  size_t i, header_index;
  header_page_t * page;
  assert(block_size == ALIGN(block_size));
  for (i = 0; i < num_blocks; i++) {
    // passing the non-atomic conditional ensures that no more than
    // HEADERS_PER_PAGE allocations will go to this header page
    map_block: while ((page = lastheaderpg()) == NULL ||
            page->next_free >= (HEADERS_PER_PAGE - 1)) {
      allocate_header_page();
    }
    header_index = __sync_add_and_fetch(&page->next_free, 1);
    // for now, sanity check to make sure header index is still in bounds
    // I do not believe that this
    if (header_index >= HEADERS_PER_PAGE) {
      goto map_block;
    }
    page->headers[header_index].size = block_size;
    // mmaped pages are padded with zeros, set NULL anyways
    page->headers[header_index].spec_next.next = NULL;
    page->headers[header_index].seq_next.next = NULL;
    __sync_synchronize(); // mem fence
    // get the i-block
    block_t * block = (block_t *) (&begin[i * (block_size + sizeof(block_t))]);
    block->header = &page->headers[header_index];
  }
}

static void grow(unsigned klass) {
  size_t size = CLASS_TO_SIZE(klass);
  size_t blocks = max(1024 / size, 5);
  size_t my_region_size = size * blocks;
  size_t growth_bytes = size * blocks;
  if (speculating()) {
    growth_bytes = __sync_add_and_fetch(&shared->spec_growth, my_region_size) - my_growth;
    __sync_add_and_fetch(&my_growth, size * blocks);
  }
  // perform the grow operation
  // TODO whiteboard & figure out if need a -1
  char * end = sbrk(growth_bytes);
  char * base = end - my_region_size;
  // now map headers for my new (private) address region
  map_headers(base, size, blocks);
}

void beginspec() {
  my_growth = 0;
  synch_lists();
}

void endspec() {
  promote_list();
}

#define container_of(ptr, type, member) ({ \
                const typeof( ((type *)0)->member ) *__mptr = (ptr); \
                (type *)( (char *)__mptr - __builtin_offsetof(type,member) );})

static inline header_t * convert_head_mode_aware(node_t * node) {
  return speculating() ? container_of(node, header_t, spec_next) :
                         container_of(node, header_t, seq_next);
}

void * noomr_malloc(size_t size) {
  header_t * header;
  stack_t * stack;
  node_t * stack_node;
  size_t aligned = ALIGN(size);
#ifdef COLLECT_STATS
  __sync_add_and_fetch(&shared->allocations, 1);
#endif
  if (size > MAX_SIZE) {
    return (void*) allocate_large(aligned);
  } else {
    alloc: stack = get_stack_of_size(aligned);
    stack_node = pop(stack);
    if (! stack_node) {
      grow(SIZE_TO_CLASS(aligned));
      goto alloc;
    }
    header = convert_head_mode_aware(stack_node);
    assert(header->payload != NULL);
    return header->payload;
  }
}

void noomr_free(void * payload) {
  if (out_of_range(payload)) {
    // A huge block is unmapped directly to kernel
    block_t * block = getblock(payload);
    munmap(block, block->huge_block_sz + sizeof(block_t));
  } else {
    // TODO do I need to delay the free while speculating?
    //  there is no overwrite issue
    header_t * header = getblock(payload)->header;
    // look up the index & push onto the stack
    stack_t * stack = get_stack_of_size(header->size);
    push(stack, speculating() ? &header->spec_next : &header->seq_next);
  }
}


void print_noomr_stats() {
#ifdef COLLECT_STATS
  int index;
  printf("NOOMR stats\n");
  printf("allocations: %u\n", shared->allocations);
  printf("frees: %u\n", shared->frees);
  printf("sbrks: %u\n", shared->sbrks);
  printf("huge allocations: %u\n", shared->huge_allocations);
  for (index = 0; index < NUM_CLASSES; index++) {
    printf("class %d allocations: %u\n", index, shared->allocs_per_class[index]);
  }
#endif
}


int main(int argc, char ** argv) {
  printf("header_t size %lu\n", sizeof(header_t));
  printf("header_pg_t size %lu\n", sizeof(header_page_t));
  printf("stack size: %lu\n", sizeof(stack_t));
  printf("headers / page struct: %lu\n", HEADERS_PER_PAGE);
}
