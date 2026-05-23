#include <cassert>
#include <string>
#include <iostream>
#include "./memory/allocator.h"

void log_debug(std::string message) {
	std::cout << message << std::endl;
}

int main() {
	void* user_ptr = gc_malloc(64);
	GCObject* obj = GCObject::from_user_ptr(user_ptr);
	assert(obj->get_size() == 64);
	assert(obj->get_age() == 0);
	assert(!obj->is_marked());
	assert(which_ptr(user_ptr) == SpaceType::Eden);
	log_debug("obj check finish");

	void* big_p = gc_malloc(EDEN_SIZE + 1);
	assert(big_p == nullptr);
	log_debug("Eden size + 1 space can't be allocated");

	void* big_p_2 = gc_malloc(MAX_OBJECT_SIZE + 1);
	assert(big_p_2 == nullptr);
	log_debug("Max Object + 1 size can't be allocated");

	log_debug("Finish test");
	return 0;
}