#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "hv_abi.hpp"
#include "npt.hpp"
namespace amdhv {
struct KernelNptAllocator {
    NptNode *allocate();
    void release(NptNode *node);
};
class GuestMemory {
    struct Region {
        Region *next;
        IOMemoryDescriptor *memory;
        uint64_t guest, size;
        size_t remaining;
        uint8_t *live;
        bool writePinned;
    };
    Npt<KernelNptAllocator> tables_;
    Region *regions_ = nullptr;
    uint64_t pinned_ = 0;
    // Bumped by every NPT edit (always under the exclusive backend gate). Each
    // vCPU flushes its ASID before the next entry that observes a new value.
    uint64_t generation_ = 1;
public:
    uint64_t generation() const { return generation_; }
    bool init() { return tables_.init(); }
    void release();
    uint64_t rootPa() const { return tables_.rootPa(); }
    bool readPhysical(uint64_t guest, void *out, size_t size, bool execute = false);
    bool writePhysical(uint64_t guest, const void *data, size_t size, bool checkOnly = false);
    kern_return_t map(task_t task, const abi::Map &in);
    kern_return_t unmap(const abi::Map &in);
    kern_return_t protect(const abi::Map &in);
};
}
