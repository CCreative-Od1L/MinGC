#pragma once
#include <cstdint>
#include "../gc/gcobject.h"

struct Space
{
	uint8_t* start;
	uint8_t* top;
	uint8_t* end;

	// 分配空间
	void* allocate(size_t user_size) {
		size_t total_size = sizeof(GCObject) + user_size;
		if (top + total_size > end) {
			return nullptr;
		}
		GCObject* obj = reinterpret_cast<GCObject*>(top);
		obj->set_size(user_size);
		top += total_size;
		return obj->user_ptr();
	}

	// 获取剩余空间
	size_t remaining() const {
		return end - top;
	}
	// 空间重置
	void reset() {
		top = start;
	}
	// 判断指针的归属
	bool contains(void* ptr) const {
		return ptr >= start && ptr < end;
	}
};
