#pragma once
#include <vector>
#include "gcobject.h"
#include "root.h"
#include "../memory/heap.h"

inline void scan_object(GCObject* obj, std::vector<GCObject*>& grey_list) {
	size_t   body_size = obj->get_size();
	uint8_t* body = (uint8_t*)(obj->user_ptr());
	size_t   word_count = body_size / sizeof(void*);
	for (size_t i = 0; i < word_count; i++) {
		uintptr_t v = *((uintptr_t*)(body + i * sizeof(void*)));

		if (v % HEADER_ALIGN != 0) continue;
		if (which_ptr((void*)v) == SpaceType::Unknown) continue;

		GCObject* child = GCObject::from_user_ptr((void*)v);
		if (!child->is_marked()) {
			grey_list.emplace_back(child);
		}
	}
}

inline void mark_phase() {
	std::vector<GCObject*> grey_list;
	std::vector<GCObject*> marked_list;

	auto& roots = get_roots();
	for (auto p : roots) {
		auto user_ptr = const_cast<void*>(*p);
		if (!user_ptr) continue;
		GCObject* header = GCObject::from_user_ptr(user_ptr);
		if (!header->is_marked()) {
			grey_list.emplace_back(header);
		}
	}

	while (!grey_list.empty()) {
		auto obj = grey_list.back();
		grey_list.pop_back();
		obj->set_mark();
		marked_list.emplace_back(obj);
		scan_object(obj, grey_list);
	}

	for (auto obj : marked_list) {
		obj->clear_mark();
	}
}
