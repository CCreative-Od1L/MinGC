#pragma once
#include "heap.h"

static Heap heap;

// 分配内存(包含GC流程)
void* gc_malloc(size_t size) {
	return heap.allocate(size);
}

SpaceType which_ptr(void* ptr) {
	return heap.which_ptr(ptr);
}
