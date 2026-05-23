#pragma once
#include <cstdint>

using header_t = uint32_t;
constexpr auto HEADER_ALIGN = 4;	// 设置内存对齐为4位
constexpr auto MAX_OBJECT_SIZE = 128 * 1024 * 1024;

struct GCObject {
	/*
		Object 的头文件信息存储
		规定：[0:mark][1-4:age][5-31:size]
	*/ 
	header_t header;
	
	// 将用户指针转换为底层的对象分配指针
	static GCObject* from_user_ptr(void* ptr) {
		return static_cast<GCObject*>(ptr) - 1;
	}

	// 将对象分配指针的内存空间返回给用户
	void* user_ptr() {
		return this + 1;
	}
	
	// 设置分配的分配的空间大小
	void set_size(uint32_t sz) {
		header = sz << 5;
	}

	// 获取分配的对象空间大小
	uint32_t get_size() const {
		return header >> 5;
	}

	// 是否被标记
	bool is_marked() const {
		return header & 1; 
	}
	// 设置标记
	void set_mark() {
		header |= 1;
	}
	// 清除标记
	void clear_mark() {
		header &= ~1;
	}

	// 获取对象的年龄
	uint8_t get_age() const {
		return (header >> 1) & 0xf;
	}
	// 增加对象的年龄
	void inc_age() {
		uint8_t age = get_age();
		age = (age + 1) & 0xf;
		header = (header & ~0x1e) | (age << 1);
	}
};