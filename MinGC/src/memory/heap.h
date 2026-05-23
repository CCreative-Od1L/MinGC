#pragma once
#include "space.h"

constexpr auto EDEN_SIZE = 8 * 1024 * 1024;
constexpr auto SURVIVOR_SIZE = 1 * 1024 * 1024;
constexpr auto OLD_SIZE = 10 * 1024 * 1024;
constexpr auto TOTAL_HEAP = EDEN_SIZE + 2 * SURVIVOR_SIZE + OLD_SIZE;

enum class SpaceType { Eden, S0, S1, Old, Unknown };

struct Heap
{
	Space eden;
	Space s0;
	Space s1;
	Space old;
	// 初始化函数	
	Heap() {
		uint8_t* buffer = new uint8_t[TOTAL_HEAP];

		eden = {buffer, buffer, buffer + EDEN_SIZE};
		s0 = {eden.end, eden.end, eden.end + SURVIVOR_SIZE};
		s1 = {s0.end, s0.end, s0.end + SURVIVOR_SIZE};
		old = {s1.end, s1.end, s1.end + OLD_SIZE};
	}
	// 分配空间
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
	// 判断用户指针的位置
	SpaceType which_ptr(void* ptr) const {
		if (eden.contains(ptr)) return SpaceType::Eden;
		if (s0.contains(ptr)) return SpaceType::S0;
		if (s1.contains(ptr)) return SpaceType::S1;
		if (old.contains(ptr)) return SpaceType::Old;
		return SpaceType::Unknown;
	}
	// 执行新生代的回收
	void collect_minor_gc() {};
};