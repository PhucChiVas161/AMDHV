// Four-level, 4 KiB AMD nested page tables. Allocator also supplies node storage.
// Callers serialize edits and flush the guest ASID before its next entry.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace amdhv {
constexpr uint64_t NptAddressMask = 0x000ffffffffff000ull;
constexpr uint64_t NptOwned = 1ull << 9; // software bit: backed, even with permissions=0
inline bool pageRange(uint64_t base, uint64_t size, uint64_t limit) {
    return size && !(base & 4095) && !(size & 4095) && base < limit && size <= limit - base;
}
inline bool nptFlags(uint64_t flags) {
    // NPT cannot express write-only or execute-only access.
    return !(flags & ~7ull) && (flags == 0 || (flags & 1));
}
inline uint64_t nptLeaf(uint64_t pa, uint64_t flags) {
    return (pa & NptAddressMask) | NptOwned | (flags ? 5ull : 0ull) |
           ((flags & 2) ? 2ull : 0ull) | ((flags & 4) ? 0ull : (1ull << 63));
}
struct NptNode {
    uint64_t *entries;
    uint64_t pa;
    void *cookie;
    NptNode *children[512];
};
template <class Allocator> class Npt {
    Allocator allocator_;
    NptNode *root_ = nullptr;
    unsigned nodes_ = 0;
    // No table-count cap: one leaf table per 2 MiB of guest RAM, bounded only
    // by allocation failure (and by the 48-bit guest-physical range).
    NptNode *alloc() {
        NptNode *node = allocator_.allocate();
        if (node) ++nodes_;
        return node;
    }
    void freeTree(NptNode *node, unsigned level) {
        if (!node) return;
        if (level) for (auto child : node->children) freeTree(child, level - 1);
        allocator_.release(node); --nodes_;
    }
    bool prune(NptNode *node, unsigned level) {
        bool empty = true;
        for (unsigned i = 0; i < 512; ++i) {
            if (level && node->children[i] && prune(node->children[i], level - 1)) {
                allocator_.release(node->children[i]); --nodes_;
                node->children[i] = nullptr; node->entries[i] = 0;
            }
            if (node->entries[i]) empty = false;
        }
        return empty;
    }
public:
    bool init() { if (!root_) root_ = alloc(); return root_ != nullptr; }
    uint64_t rootPa() const { return root_ ? root_->pa : 0; }
    unsigned nodeCount() const { return nodes_; }
    void release() { freeTree(root_, 3); root_ = nullptr; }
    void trim() { if (root_) prune(root_, 3); }
    uint64_t *leaf(uint64_t gpa, bool create = false) {
        if (gpa >= (1ull << 48) || (gpa & 4095) || !root_) return nullptr;
        NptNode *node = root_;
        for (unsigned level = 3; level; --level) {
            unsigned idx = (gpa >> (12 + level * 9)) & 511;
            if (!node->children[idx]) {
                if (!create) return nullptr;
                NptNode *child = alloc();
                if (!child) return nullptr;
                node->children[idx] = child;
                node->entries[idx] = child->pa | 7;
            }
            node = node->children[idx];
        }
        return &node->entries[(gpa >> 12) & 511];
    }
};
}
