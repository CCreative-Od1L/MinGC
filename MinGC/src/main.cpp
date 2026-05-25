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

	std::vector<GCObject*> marked;
	mark_phase(marked);

	GCObject* obj = GCObject::from_user_ptr(p);
	assert(obj->is_marked());
	for (auto o : marked) o->clear_mark();
	log_debug("Stage 2: mark_root pass");

	gc_remove_root(&p);
}

void test_stage2_mark_chain() {
	void* a = gc_malloc(sizeof(void*));
	void* b = gc_malloc(sizeof(void*));

	// A.body[0] = &B
	std::memcpy(a, &b, sizeof(void*));

	gc_add_root(&a);

	std::vector<GCObject*> marked;
	mark_phase(marked);

	GCObject* objA = GCObject::from_user_ptr(a);
	GCObject* objB = GCObject::from_user_ptr(b);
	assert(objA->is_marked());
	assert(objB->is_marked());
	for (auto o : marked) o->clear_mark();
	log_debug("Stage 2: mark_chain pass");

	gc_remove_root(&a);
}

void test_stage2_unreachable_not_marked() {
	void* p = gc_malloc(sizeof(void*));

	std::vector<GCObject*> marked;
	mark_phase(marked);

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

	std::vector<GCObject*> marked;
	mark_phase(marked);  // should not crash with null root
	log_debug("Stage 2: null root pass");

	gc_remove_root(&p);
}

// --- Stage 3 tests ---
void test_stage3_copy_and_root_update() {
	void* p = gc_malloc(sizeof(void*));
	void* original = p;
	gc_add_root(&p);

	heap.collect_minor_gc();

	assert(p != original);
	assert(which_ptr(p) != SpaceType::Eden);
	log_debug("Stage 3: copy and root update pass");

	gc_remove_root(&p);
}

void test_stage3_chain_fix() {
	void* parent = gc_malloc(sizeof(void*));
	uint64_t* child_data = (uint64_t*)gc_malloc(sizeof(uint64_t));
	*child_data = 0xDEADBEEFCAFEull;
	std::memcpy(parent, &child_data, sizeof(void*));
	gc_add_root(&parent);

	heap.collect_minor_gc();

	uint64_t** pslot = (uint64_t**)parent;
	assert(*pslot != child_data);
	assert(**pslot == 0xDEADBEEFCAFEull);
	log_debug("Stage 3: chain fix pass");

	gc_remove_root(&parent);
}

void test_stage3_eden_reclaimed() {
	void* live = gc_malloc(64);
	gc_add_root(&live);
	heap.collect_minor_gc();

	void* fresh = gc_malloc(64);
	assert(fresh != nullptr);
	assert(which_ptr(fresh) == SpaceType::Eden);
	log_debug("Stage 3: Eden reclaimed pass");

	gc_remove_root(&live);
}

void test_stage3_forwarding_dedup() {
	void* shared = gc_malloc(sizeof(void*));
	void* r1 = gc_malloc(sizeof(void*));
	void* r2 = gc_malloc(sizeof(void*));
	std::memcpy(r1, &shared, sizeof(void*));
	std::memcpy(r2, &shared, sizeof(void*));
	gc_add_root(&r1);
	gc_add_root(&r2);

	heap.collect_minor_gc();

	void* t1 = *(void**)r1;
	void* t2 = *(void**)r2;
	assert(t1 == t2);
	assert(t1 != shared);
	log_debug("Stage 3: forwarding dedup pass");

	gc_remove_root(&r1);
	gc_remove_root(&r2);
}

void test_stage3_promotion() {
	void* p = gc_malloc(sizeof(void*));
	gc_add_root(&p);

	for (int i = 0; i <= PROMOTION_AGE; i++) {
		heap.collect_minor_gc();
	}

	assert(which_ptr(p) == SpaceType::Old);
	log_debug("Stage 3: promotion pass");

	gc_remove_root(&p);
}

void test_stage3_old_gen_not_moved() {
	void* p = gc_malloc(sizeof(void*));
	gc_add_root(&p);

	for (int i = 0; i <= PROMOTION_AGE; i++) {
		heap.collect_minor_gc();
	}

	void* saved_addr = p;
	heap.collect_minor_gc();
	assert(p == saved_addr);
	assert(which_ptr(p) == SpaceType::Old);
	log_debug("Stage 3: Old Gen not moved pass");

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

	test_stage3_copy_and_root_update();
	test_stage3_chain_fix();
	test_stage3_eden_reclaimed();
	test_stage3_forwarding_dedup();
	test_stage3_promotion();
	test_stage3_old_gen_not_moved();

	log_debug("All tests passed");
	return 0;
}