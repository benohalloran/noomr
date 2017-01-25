#include <stdlib.h>
#include <stdio.h>
#include "noomr.h"
#include "dummy.h"

#define NUM_ROUNDS 100000

size_t class_for_rnd(int rnd) {
  return ALIGN(CLASS_TO_SIZE(rnd % NUM_CLASSES) - sizeof(block_t));
}

typedef struct {
  int * space;
  size_t size;
} record_t;

int main() {
  int rnd;
  size_t alloc_size;

  srand(0);

  for (rnd = 0; rnd < NUM_ROUNDS; rnd++) {
    alloc_size = class_for_rnd(rnd);
    noomr_malloc(alloc_size);
  }
  printf("Performance test complete\n");
  print_noomr_stats();
}