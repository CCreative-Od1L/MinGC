#include <cassert>
#include <cstring>
#include <string>
#include <iostream>
#include "./memory/heap.h"
#include "./gc/gcobject.h"
#include "./gc/root.h"
#include "./gc/mark.h"

void log_debug(std::string message) {
	std::cout << message << std::endl;
}

// --- Stage 1 tests ---
void test_stage1() {
	void* user_ptr = gc_malloc(64);
	GCObject* obj = GCObject::from_user_ptr(user_ptr);
	assert(obj->get_size() == 64);
	assert(obj->get_age() == 0);
	assert(!obj->is_marked());
	assert(which_ptr(user_ptr) == SpaceType::Eden);
	log_debug("Stage 1: obj check finish");

	void* big_p = gc_malloc(EDEN_SIZE + 1);
	assert(big_p == nullptr);
	log_debug("Stage 1: Eden size + 1 can't be allocated");

	void* big_p_2 = gc_malloc(MAX_OBJECT_SIZE + 1);
	assert(big_p_2 == nullptr);
	log_debug("Stage 1: Max Object + 1 can't be allocated");
}

// --- Stage 2 tests ---
void test_stage2_mark_root() {
	void* p = gc_malloc(sizeof(void*));
	gc_add_root(&p);

	mark_phase();

	GCObject* obj = GCObject::from_user_ptr(p);
	assert(!obj->is_marked());  // marks cleared after mark_phase
	log_debug("Stage 2: mark_root pass");

	gc_remove_root(&p);
}

void test_stage2_mark_chain() {
	void* a = gc_malloc(sizeof(void*));
	void* b = gc_malloc(sizeof(void*));

	// A.body[0] = &B
	std::memcpy(a, &b, sizeof(void*));

	gc_add_root(&a);

	mark_phase();

	GCObject* objA = GCObject::from_user_ptr(a);
	GCObject* objB = GCObject::from_user_ptr(b);
	assert(!objA->is_marked());
	assert(!objB->is_marked());
	log_debug("Stage 2: mark_chain pass");

	gc_remove_root(&a);
}

void test_stage2_unreachable_not_marked() {
	void* p = gc_malloc(sizeof(void*));

	mark_phase();

	GCObject* obj = GCObject::from_user_ptr(p);
	assert(!obj->is_marked());
	log_debug("Stage 2: unreachable not marked pass");
}

void test_stage2_roots_not_duplicated() {
	void* p = gc_malloc(sizeof(void*));
	gc_add_root(&p);
	gc_add_root(&p);  // duplicate should be ignored

	auto& roots = get_roots();
	assert(roots.size() == 1);
	log_debug("Stage 2: roots de-duplication pass");

	gc_remove_root(&p);
}

void test_stage2_remove_root() {
	void* p = gc_malloc(sizeof(void*));
	gc_add_root(&p);
	gc_remove_root(&p);

	auto& roots = get_roots();
	assert(roots.size() == 0);
	log_debug("Stage 2: remove root pass");
}

void test_stage2_null_root() {
	void* p = nullptr;
	gc_add_root(&p);

	mark_phase();  // should not crash with null root
	log_debug("Stage 2: null root pass");

	gc_remove_root(&p);
}

int main() {
	test_stage1();

	test_stage2_mark_root();
	test_stage2_mark_chain();
	test_stage2_unreachable_not_marked();
	test_stage2_roots_not_duplicated();
	test_stage2_remove_root();
	test_stage2_null_root();

	log_debug("All tests passed");
	return 0;
}