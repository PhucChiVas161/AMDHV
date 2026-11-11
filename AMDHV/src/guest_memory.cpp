#include "guest_memory.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#ifdef KERNEL
#include <mach/vm_param.h>
#endif
namespace amdhv {
namespace {
constexpr kern_return_t BadArgument = (kern_return_t)0xfae94003;
constexpr kern_return_t NoResources = (kern_return_t)0xfae94005;
constexpr kern_return_t Error = (kern_return_t)0xfae94001;
// No policy limit on guest RAM (the former 8 GiB per VM / 16 GiB total made a
// 16 GiB Docker VM fail to start). Guest RAM is wired host RAM, so the only
// bound kept is physical: all VMs together cannot wire more than exists.
// Sizing the VM against what the host itself needs is the user's decision.
#ifdef KERNEL
// ::max_mem (mach/vm_param.h, com.apple.kpi.unsupported): usable physical memory.
uint64_t pinBudget() { return ::max_mem; }
#else
uint64_t pinBudget() { return 16ull << 30; } // unit-test model
#endif
// All GuestMemory lifecycle/map operations run under the exclusive backend gate.
// Account entire prepared descriptors: partial unmap does not release their pins.
uint64_t totalPinned = 0;
bool valid(const abi::Map &in) {
    return in.space == 0 && pageRange(in.guest, in.size, 1ull << 48);
}
}
NptNode *KernelNptAllocator::allocate() {
    auto node = IONewZero(NptNode, 1);
    if (!node) return nullptr;
    auto md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, PAGE_SIZE, NptAddressMask);
    if (!md) { IODelete(node, NptNode, 1); return nullptr; }
    if (md->prepare() != kIOReturnSuccess) {
        md->release(); IODelete(node, NptNode, 1); return nullptr;
    }
    IOByteCount length = 0;
    node->pa = md->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    if (!node->pa || (node->pa & ~NptAddressMask) || length < PAGE_SIZE) {
        md->complete(); md->release(); IODelete(node, NptNode, 1); return nullptr;
    }
    node->entries = static_cast<uint64_t *>(md->getBytesNoCopy());
    bzero(node->entries, PAGE_SIZE);
    node->cookie = md;
    return node;
}
void KernelNptAllocator::release(NptNode *node) {
    auto md = static_cast<IOBufferMemoryDescriptor *>(node->cookie);
    md->complete(); md->release(); IODelete(node, NptNode, 1);
}
void GuestMemory::release() {
    // Caller must have stopped every vCPU before revoking backing pages.
    tables_.release();
    while (regions_) {
        Region *r = regions_; regions_ = r->next;
        r->memory->complete(); r->memory->release();
        IOFree(r->live, r->size / PAGE_SIZE);
        IODelete(r, Region, 1);
    }
    totalPinned -= pinned_;
    pinned_ = 0;
}
// Called after host state/interrupts are restored, with the backend gate held.
// Only pinned, live guest mappings are read; never dereference a guest physical pointer.
bool GuestMemory::readPhysical(uint64_t guest, void *out, size_t size, bool execute) {
    if (!size || guest >= (1ull<<48) || size > (1ull<<48)-guest) return false;
    auto bytes = static_cast<uint8_t *>(out);
    while (size) {
        auto entry = tables_.leaf(guest & ~4095ull);
        if (!entry || !(*entry & NptOwned) || !(*entry & 1) || (execute && (*entry >> 63))) return false;
        Region *region = regions_;
        while (region && !(guest >= region->guest && guest-region->guest < region->size)) region = region->next;
        if (!region || !region->live[(guest-region->guest)/PAGE_SIZE]) return false;
        size_t chunk = PAGE_SIZE - (guest & 4095);
        if (chunk > size) chunk = size;
        if (region->memory->readBytes(guest-region->guest, bytes, chunk) != chunk) return false;
        guest += chunk; bytes += chunk; size -= chunk;
    }
    return true;
}
// Preflight the complete range before any store; only writable prepared host pins.
bool GuestMemory::writePhysical(uint64_t guest, const void *data, size_t size, bool checkOnly) {
    if(!size || guest >= (1ull<<48) || size > (1ull<<48)-guest || (!data && !checkOnly)) return false;
    for(unsigned pass=0; pass<(checkOnly?1u:2u); ++pass) {
        uint64_t at=guest; size_t left=size; auto bytes=static_cast<const uint8_t *>(data);
        while(left) {
            auto entry=tables_.leaf(at & ~4095ull);
            if(!entry || !(*entry&NptOwned) || (*entry&3)!=3) return false;
            Region *r=regions_;
            while(r && !(at>=r->guest && at-r->guest<r->size)) r=r->next;
            if(!r || !r->writePinned || !r->live[(at-r->guest)/PAGE_SIZE]) return false;
            size_t chunk=PAGE_SIZE-(at&4095); if(chunk>left)chunk=left;
            if(pass && r->memory->writeBytes(at-r->guest,bytes,chunk)!=chunk) return false;
            at+=chunk;left-=chunk;if(bytes)bytes+=chunk;
        }
    }
    return true;
}
kern_return_t GuestMemory::map(task_t task, const abi::Map &in) {
    if (!valid(in) || !pageRange(in.host, in.size, 1ull << 47) || !nptFlags(in.flags)) return BadArgument;
    const uint64_t budget = pinBudget();
    if (totalPinned > budget || in.size > budget - totalPinned) return NoResources;
    for (uint64_t off = 0; off < in.size; off += PAGE_SIZE) {
        uint64_t *entry = tables_.leaf(in.guest + off);
        if (entry && (*entry & NptOwned)) return BadArgument;
    }
    auto r = IONewZero(Region, 1);
    if (!r) return NoResources;
    r->size = in.size; r->guest = in.guest; r->remaining = in.size / PAGE_SIZE;
    r->live = static_cast<uint8_t *>(IOMalloc(r->remaining));
    r->memory = IOMemoryDescriptor::withAddressRange(in.host, in.size, kIODirectionInOut | kIOMemoryThreadSafe, task);
    if (!r->live || !r->memory) {
        if (r->live) IOFree(r->live, r->remaining);
        if (r->memory) r->memory->release();
        IODelete(r, Region, 1); return NoResources;
    }
    // Prefer a writable pin so later vm_protect can enable writes on writable backing.
    // EFI ROM mappings may be host read-only: a read pin is sufficient for guest R/RX/NONE.
    r->writePinned = r->memory->prepare() == kIOReturnSuccess;
    if (!r->writePinned) {
        r->memory->release(); r->memory = nullptr;
        if (!(in.flags & 2))
            r->memory = IOMemoryDescriptor::withAddressRange(in.host, in.size, kIODirectionOut | kIOMemoryThreadSafe, task);
        if (!r->memory || r->memory->prepare() != kIOReturnSuccess) {
            if (r->memory) r->memory->release();
            IOFree(r->live, r->remaining); IODelete(r, Region, 1); return Error;
        }
    }
    uint64_t off = 0;
    for (; off < in.size; off += PAGE_SIZE) {
        IOByteCount length = 0;
        uint64_t pa = r->memory->getPhysicalSegment(off, &length, kIOMemoryMapperNone);
        uint64_t *entry = tables_.leaf(in.guest + off, true);
        if (!pa || (pa & ~NptAddressMask) || length < PAGE_SIZE || !entry) break;
        *entry = nptLeaf(pa, in.flags);
    }
    if (off != in.size) {
        ++generation_; // intermediate tables may have been created and trimmed
        // Nothing can run during this transaction. Roll back all installed leaves.
        for (uint64_t undo = 0; undo < off; undo += PAGE_SIZE) *tables_.leaf(in.guest + undo) = 0;
        tables_.trim();
        r->memory->complete(); r->memory->release();
        IOFree(r->live, r->remaining); IODelete(r, Region, 1); return NoResources;
    }
    memset(r->live, 1, r->remaining);
    r->next = regions_; regions_ = r; pinned_ += in.size; totalPinned += in.size;
    ++generation_;
    return 0;
}
kern_return_t GuestMemory::unmap(const abi::Map &in) {
    if (!valid(in)) return BadArgument;
    const uint64_t end = in.guest + in.size;
    for (Region **link = &regions_; *link;) {
        Region *r = *link;
        uint64_t first = in.guest > r->guest ? in.guest : r->guest;
        uint64_t last = end < r->guest + r->size ? end : r->guest + r->size;
        for (uint64_t gpa = first; gpa < last; gpa += PAGE_SIZE) {
            size_t i = (gpa - r->guest) / PAGE_SIZE;
            if (!r->live[i]) continue;
            uint64_t *entry = tables_.leaf(gpa);
            if (entry) *entry = 0;
            r->live[i] = 0; --r->remaining;
        }
        if (!r->remaining) {
            *link = r->next; pinned_ -= r->size; totalPinned -= r->size;
            r->memory->complete(); r->memory->release();
            IOFree(r->live, r->size / PAGE_SIZE); IODelete(r, Region, 1);
        } else link = &r->next;
    }
    tables_.trim();
    ++generation_;
    return 0;
}
kern_return_t GuestMemory::protect(const abi::Map &in) {
    if (!valid(in) || !nptFlags(in.flags)) return BadArgument;
    // A read-only host pin must never become guest-writable through vm_protect.
    if (in.flags & 2) {
        const uint64_t end = in.guest + in.size;
        for (Region *r = regions_; r; r = r->next) {
            if (r->writePinned) continue;
            uint64_t first = in.guest > r->guest ? in.guest : r->guest;
            uint64_t last = end < r->guest + r->size ? end : r->guest + r->size;
            for (uint64_t gpa = first; gpa < last; gpa += PAGE_SIZE)
                if (r->live[(gpa - r->guest) / PAGE_SIZE]) return BadArgument;
        }
    }
    // Validate the whole range before changing any permission.
    for (uint64_t off = 0; off < in.size; off += PAGE_SIZE) {
        uint64_t *entry = tables_.leaf(in.guest + off);
        if (!entry || !(*entry & NptOwned)) return BadArgument;
    }
    for (uint64_t off = 0; off < in.size; off += PAGE_SIZE) {
        uint64_t *entry = tables_.leaf(in.guest + off);
        *entry = nptLeaf(*entry, in.flags);
    }
    ++generation_;
    return 0;
}
}
