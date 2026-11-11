#pragma once
#include "IOMemoryDescriptor.h"
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
    IOBufferMemoryDescriptor(void *address, size_t size) : IOMemoryDescriptor((uintptr_t)address, size) { owned_ = true; }
public:
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(task_t, unsigned, size_t size, uint64_t) {
        if (mock::allocationBudget == 0) return nullptr;
        if (mock::allocationBudget > 0) --mock::allocationBudget;
        void *buffer = nullptr;
        if (posix_memalign(&buffer, PAGE_SIZE, size)) return nullptr;
        return new IOBufferMemoryDescriptor(buffer, size);
    }
    void *getBytesNoCopy() { return (void *)address_; }
};
