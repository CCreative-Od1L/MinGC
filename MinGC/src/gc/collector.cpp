#include "../memory/heap.h"
#include "mark.h"

Heap heap;

std::unordered_set<void**> roots;

void Heap::collect_minor_gc() {
	mark_phase();
}
