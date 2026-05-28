#pragma once
#include <unordered_set>
#include <unordered_map>
#include "gcobject.h"

extern std::unordered_set<void**> weak_refs;

inline void gc_add_weak_ref(void** slot) {
	weak_refs.insert(slot);
}

inline void gc_remove_weak_ref(void** slot) {
	weak_refs.erase(slot);
}

inline void clear_dead_weak_refs(const std::unordered_map<GCObject*, GCObject*>* forwarding_map) {
	for (void** slot : weak_refs) {
		if (!*slot) continue;
		GCObject* header = GCObject::from_user_ptr(*slot);
		if (!header->is_marked()) {
			*slot = nullptr;
		}
		else if (forwarding_map) {
			auto it = forwarding_map->find(header);
			if (it != forwarding_map->end()) {
				*slot = it->second->user_ptr();
			}
		}
	}
}