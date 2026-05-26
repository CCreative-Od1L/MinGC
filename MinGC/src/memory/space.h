#pragma once
#include <cstdint>
#include "../gc/gcobject.h"

struct FreeBlock {
	size_t		size;
	FreeBlock*	next_block;
};

struct Space
{
	uint8_t* start;
	uint8_t* top;
	uint8_t* end;

	FreeBlock* free_list;

	// 分配空间
	void* allocate(size_t user_size) {
		size_t total_size = sizeof(GCObject) + user_size;
		// 分配空间字节对齐
		total_size = (total_size + HEADER_ALIGN - 1) & ~(HEADER_ALIGN - 1);
		if (total_size < sizeof(FreeBlock)) {
			total_size = sizeof(FreeBlock);
		}
		if (top + total_size > end) {
			return nullptr;
		}
		GCObject* obj = reinterpret_cast<GCObject*>(top);
		obj->set_size(user_size);
		top += total_size;
		return obj->user_ptr();
	}

	void* allocate_raw(size_t size) {
		FreeBlock* prev = nullptr, *cur = free_list;
		while (cur != nullptr) {
			if (cur->size >= size) {
				if (cur->size - size < sizeof(FreeBlock)) {
					if (prev) {
						prev->next_block = cur->next_block;
					}
					else {
						free_list = free_list->next_block;
					}
					return cur;
				}
				else {
					FreeBlock* remainder = (FreeBlock*)((uint8_t*)cur + size);
					if (prev) {
						prev->next_block = remainder;
					}
					else {
						free_list = remainder;
					}
					remainder->size = cur->size - size;
					remainder->next_block = cur->next_block;
					return (uint8_t*)cur;
				}
			}
			prev = cur;
			cur = cur->next_block;
		}

		if (top + size > end) return nullptr;
		uint8_t* ptr = top;
		top += size;
		return ptr;
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

