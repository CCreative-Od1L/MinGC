#pragma once
#include <iostream>

using header_t = uint32_t;
constexpr auto HEADER_ALIGN = 4;	// 设置内存对齐为4位

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
};