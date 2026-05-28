#include <unordered_map>
#include <cstring>
#include "../memory/heap.h"
#include "mark.h"

Heap heap;

std::unordered_set<void**> roots;
std::unordered_set<void**> weak_refs;
std::unordered_set<void**> soft_refs;

void sweep_old();

void Heap::collect_minor_gc() {
	std::vector<GCObject*> marked_list;
	std::unordered_map<GCObject*, GCObject*> forwarding_map;
	mark_phase(marked_list);

	for (auto obj : marked_list) {
		if (which_ptr(obj) == SpaceType::Old) continue;

		void* new_obj = nullptr;
		auto obj_total_size = obj->total_size();
		if (obj->get_age() >= PROMOTION_AGE) {
			new_obj = heap.old.allocate_raw(obj_total_size);
		}
		else {
			new_obj = heap.to_space->allocate_raw(obj_total_size);
		}

		if (new_obj == nullptr) {
			collect_full_gc();
			new_obj = heap.old.allocate_raw(obj_total_size);
			if (!new_obj) {
				continue;
			}
		}
		obj->inc_age();
		memcpy(new_obj, obj, obj_total_size);
		forwarding_map[obj] = reinterpret_cast<GCObject*>(new_obj);
	}

	// 执行根修复
	for (auto r : roots) {
		void* old_user_ptr = *r;
		if (!old_user_ptr) continue;
		GCObject* old_header = GCObject::from_user_ptr(old_user_ptr);
		auto it = forwarding_map.find(old_header);
		if (it != forwarding_map.end()) {
			*r = it->second->user_ptr();
		}
	}

	// 执行Body修复
	for (auto& [old, new_obj] : forwarding_map) {
		size_t obj_size = new_obj->get_size();
		uint8_t* body = static_cast<uint8_t*>(new_obj->user_ptr());
		auto word_count = obj_size / sizeof(void*);
		for (size_t i = 0; i < word_count; i++) {
			void** slot = reinterpret_cast<void**>(body + i * sizeof(void*));
			void* val = *slot;
			if (!val) continue;

			if ((uintptr_t)val % HEADER_ALIGN != 0) continue;
			if (which_ptr(val) == SpaceType::Unknown) continue;

			GCObject* target = GCObject::from_user_ptr(val);
			auto it = forwarding_map.find(target);
			if (it != forwarding_map.end()) {
				*slot = it->second->user_ptr();
			}
		}
	}

	heap.eden.reset();
	heap.swap_survivors();

	for (auto& [old, new_obj] : forwarding_map) {
		new_obj->clear_mark();
	}
	for (auto obj : marked_list) {
		if (which_ptr(obj) == SpaceType::Old) {
			obj->clear_mark();
		}
	}
}

void sweep_old() {
	Space& old_gen = heap.old;
	old_gen.free_list = nullptr;
	uint8_t* cur = old_gen.start;
	while (cur < old_gen.top) {
		GCObject* header = reinterpret_cast<GCObject*>(cur);
		size_t obj_size = header->total_size();
		if (header->is_marked()) {
			header->clear_mark();
		}
		else if (obj_size >= sizeof(FreeBlock)) {
			FreeBlock* block = reinterpret_cast<FreeBlock*>(header);
			block->size = obj_size;
			block->next_block = old_gen.free_list;
			old_gen.free_list = block;
		}
		cur += obj_size;
	}
}

void Heap::collect_full_gc() {
	std::vector<GCObject*> marked_list;
	mark_phase(marked_list);
	sweep_old();
}
