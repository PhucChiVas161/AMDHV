#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
using task_t = void *;
using kern_return_t = int;
using IOByteCount = size_t;
constexpr task_t kernel_task = nullptr;
constexpr unsigned PAGE_SIZE = 4096, kIODirectionInOut = 3, kIODirectionOut = 2;
constexpr unsigned kIOMemoryMapperNone = 0, kIOMemoryPhysicallyContiguous = 0x10;
constexpr unsigned kIOMemoryThreadSafe = 0x00100000;
constexpr int kIOReturnSuccess = 0;
namespace mock {
inline int descriptors = 0, prepared = 0;
inline bool failPrepare = false;
inline bool hostReadOnly = false;
inline int badPage = -1;
inline int allocationBudget = -1;
}
class IOMemoryDescriptor {
protected:
    uintptr_t address_;
    size_t length_;
    bool prepared_ = false;
    bool owned_ = false;
    unsigned direction_ = kIODirectionInOut;
public:
    IOMemoryDescriptor(uintptr_t address, size_t length) : address_(address), length_(length) { ++mock::descriptors; }
    virtual ~IOMemoryDescriptor() { if (owned_) free((void *)address_); --mock::descriptors; }
    static IOMemoryDescriptor *withAddressRange(uint64_t address, uint64_t length, unsigned direction, task_t) {
        auto md = new IOMemoryDescriptor(address, length);
        md->direction_ = direction & 3;
        return md;
    }
    int prepare() {
        if (mock::failPrepare || (!owned_ && mock::hostReadOnly && direction_ != kIODirectionOut)) return -1;
        prepared_ = true; ++mock::prepared; return 0;
    }
    void complete() { if (prepared_) { prepared_ = false; --mock::prepared; } }
    void release() { delete this; }
    IOByteCount readBytes(IOByteCount offset, void *out, IOByteCount size) {
        if (!prepared_ || offset > length_ || size > length_-offset) return 0;
        memcpy(out, reinterpret_cast<const void *>(address_+offset), size); return size;
    }
    IOByteCount writeBytes(IOByteCount offset, const void *in, IOByteCount size) {
        if (!prepared_ || direction_!=kIODirectionInOut || offset>length_ || size>length_-offset) return 0;
        memcpy(reinterpret_cast<void *>(address_+offset),in,size);return size;
    }
    uint64_t getPhysicalSegment(uint64_t offset, IOByteCount *length, unsigned) {
        if (!owned_ && mock::badPage == (int)(offset / PAGE_SIZE)) return 0;
        if (length) *length = length_ - offset;
        return address_ + offset;
    }
};
