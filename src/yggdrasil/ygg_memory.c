
#include <mach/mach.h>

void* ygg_virtual_alloc(size_t size) {
	ygg_assert(size > 0, "Size can't be 0");
	if (size == 0) {
		return NULL;
	}

	// Round to nearest page size
	size = round_page(size);
	 
	void* data;
    kern_return_t result = mach_vm_allocate((vm_map_t)mach_task_self(), (mach_vm_address_t*)&data, size, VM_FLAGS_ANYWHERE);
	ygg_assert(result == KERN_SUCCESS, "Failed to allocate virtual memory");
	if (result != KERN_SUCCESS) {
		return NULL;
	}
 
    return data;
}

void ygg_virtual_free(void* data, size_t size) {
	// Round to nearest page size
	size = round_page(size);
	
	mach_vm_deallocate((vm_map_t)mach_task_self(), (mach_vm_address_t)data, size);
}
