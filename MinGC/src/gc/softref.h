#pragma once
#include <unordered_set>
#include <vector>
#include "gcobject.h"

extern std::unordered_set<void**> soft_refs;

inline void gc_add_soft_ref(void** slot) {
	soft_refs.insert(slot);
}

inline void gc_remove_soft_ref(void** slot) {
	soft_refs.erase(slot);
}

inline void soft_ref_grey_list(std::vector<GCObject*>& grey_list) {
	for (void** slot : soft_refs) {
		if (!*slot) continue;
		GCObject* header = GCObject::from_user_ptr(*slot);
		if (!header->is_marked()) {
			grey_list.emplace_back(header);
		}
	}
}

inline void clear_dead_soft_refs() {
	for (void** slot : soft_refs) {
		if (!*slot) continue;
		GCObject* header = GCObject::from_user_ptr(*slot);
		if (!header->is_marked()) {
			*slot = nullptr;
		}
	}
}
