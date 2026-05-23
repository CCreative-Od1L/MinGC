#pragma once
#include <unordered_set>

extern std::unordered_set<void**> roots;

inline void gc_add_root(void** ptr) {
	roots.insert(ptr);
}

inline void gc_remove_root(void** ptr) {
	roots.erase(ptr);
}

inline const std::unordered_set<void**>& get_roots() {
	return roots;
}
