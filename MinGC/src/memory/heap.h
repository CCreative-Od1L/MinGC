#pragma once
#include "space.h"

constexpr auto EDEN_SIZE = 8 * 1024 * 1024;
constexpr auto SURVIVOR_SIZE = 1 * 1024 * 1024;
constexpr auto OLD_SIZE = 10 * 1024 * 1024;
constexpr auto TOTAL_HEAP = EDEN_SIZE + 2 * SURVIVOR_SIZE + OLD_SIZE;

constexpr auto PROMOTION_AGE = 3;

enum class SpaceType { Eden, S0, S1, Old, Unknown };

struct Heap
{
	Space eden;
	Space s0;
	Space s1;
	Space old;
	
	Space* from_space = &s0;
	Space* to_space = &s1;

	Heap() {
		uint8_t* buffer = new uint8_t[TOTAL_HEAP];

		eden = {buffer, buffer, buffer + EDEN_SIZE};
		s0 = {eden.end, eden.end, eden.end + SURVIVOR_SIZE};
		s1 = {s0.end, s0.end, s0.end + SURVIVOR_SIZE};
		old = {s1.end, s1.end, s1.end + OLD_SIZE};
	}
	void* allocate(size_t user_size) {
		if (user_size > MAX_OBJECT_SIZE) {
			return nullptr;
		}

		auto ptr = eden.allocate(user_size);
		if (ptr) {
			return ptr;
		}
		collect_minor_gc();
		return eden.allocate(user_size);
	}
	SpaceType which_ptr(void* ptr) const {
		if (eden.contains(ptr)) return SpaceType::Eden;
		if (s0.contains(ptr))  return SpaceType::S0;
		if (s1.contains(ptr))  return SpaceType::S1;
		if (old.contains(ptr)) return SpaceType::Old;
		return SpaceType::Unknown;
	}
	void swap_survivors() {
		from_space->reset();
		std::swap(from_space, to_space);
	}
	void collect_minor_gc();
	void collect_full_gc(bool is_oom = false);
};

extern Heap heap;

inline void* gc_malloc(size_t size) {
	return heap.allocate(size);
}

inline SpaceType which_ptr(void* ptr) {
	return heap.which_ptr(ptr);
}
