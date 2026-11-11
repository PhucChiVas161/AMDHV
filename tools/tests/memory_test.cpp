#include "guest_memory.hpp"
#include <cassert>
#include <initializer_list>

#include <cstdio>
#include <cstdlib>
using namespace amdhv;
constexpr int BadArgument = (int)0xfae94003;
static void noLeaks() { assert(mock::descriptors == 0 && mock::prepared == 0); }
int main() {
    assert(!pageRange(0, 0, 1ull << 48));
    assert(!pageRange(~0ull - 4095, 8192, 1ull << 48));
    assert(!pageRange(1, 4096, 1ull << 48));
    assert(pageRange((1ull << 48) - 4096, 4096, 1ull << 48));
    for (unsigned flags = 0; flags != 16; ++flags)
        assert(nptFlags(flags) == (flags == 0 || flags == 1 || flags == 3 || flags == 5 || flags == 7));
    assert((nptLeaf(0x12345000, 1) & ((1ull << 63) | 7)) == ((1ull << 63) | 5));
    assert((nptLeaf(0x12345000, 7) & 7) == 7);
    assert((nptLeaf(0x12345000, 0) & 1) == 0);
    assert(nptLeaf(0x12345000, 0) & NptOwned);
    {
        Npt<KernelNptAllocator> npt;
        assert(npt.init());
        for (uint64_t gpa : {0ull, 0x200000ull, 0x40000000ull, 0x8000000000ull, 0xfffffffff000ull}) {
            auto entry = npt.leaf(gpa, true); assert(entry);
            *entry = nptLeaf(0x12345000, 7);
            assert((*npt.leaf(gpa) & NptAddressMask) == 0x12345000);
        }
        assert(!npt.leaf(1ull << 48, true));
        npt.release();
    }
    noLeaks();
    void *buffer = nullptr;
    assert(posix_memalign(&buffer, PAGE_SIZE, 4 * PAGE_SIZE) == 0);
    abi::Map map{0, (uintptr_t)buffer, 0x200000, 4 * PAGE_SIZE, 7};
    {
        GuestMemory mem;
        assert(mem.init());
        assert(mem.map(nullptr, map) == 0);
        assert(mem.map(nullptr, map) == BadArgument); // overlap never replaces backing pages
        auto middle = map; middle.guest += PAGE_SIZE; middle.size = 2 * PAGE_SIZE;
        middle.flags = 0;
        assert(mem.protect(middle) == 0);
        assert(mem.map(nullptr, middle) == BadArgument); // PROT_NONE remains owned
        assert(mem.unmap(middle) == 0);
        assert(mem.protect(map) == BadArgument); // hole: no partial permission update
        middle.flags = 3;
        assert(mem.map(nullptr, middle) == 0); // remap a hole while old descriptor is retained
        assert(mem.unmap(map) == 0);
        assert(mock::prepared == 1); // only root table remains, all backing pins released
        assert(mem.unmap(map) == 0); // holes are harmless
        mem.release();
    }
    noLeaks();
    {
        GuestMemory mem; assert(mem.init());
        mock::badPage = 2;
        assert(mem.map(nullptr, map) != 0); // physical-segment failure midway
        mock::badPage = -1;
        assert(mock::prepared == 1);
        assert(mem.map(nullptr, map) == 0); // rolled-back leaves must not collide
        mem.release();
    }
    noLeaks();
    for (int budget = 0; budget < 3; ++budget) {
        GuestMemory mem; assert(mem.init());
        mock::allocationBudget = budget;
        assert(mem.map(nullptr, map) != 0); // table allocation fails at each intermediate level
        mock::allocationBudget = -1;
        assert(mock::prepared == 1);
        assert(mem.map(nullptr, map) == 0);
        mem.release(); noLeaks();
    }
    {
        GuestMemory mem; assert(mem.init());
        mock::failPrepare = true;
        assert(mem.map(nullptr, map) != 0);
        mock::failPrepare = false;
        auto bad = map; bad.host += 1;
        assert(mem.map(nullptr, bad) == BadArgument);
        bad = map; bad.flags = 8;
        assert(mem.map(nullptr, bad) == BadArgument);
        mem.release();
    }
    noLeaks();
    {
        GuestMemory mem; assert(mem.init());
        mock::hostReadOnly = true;
        auto rom = map; rom.flags = 5;
        assert(mem.map(nullptr, rom) == 0); // write pin fails, read pin succeeds
        assert(mem.protect(map) == BadArgument); // cannot grant W on a read pin
        rom.flags = 0; assert(mem.protect(rom) == 0);
        rom.flags = 5; assert(mem.protect(rom) == 0);
        assert(mem.unmap(rom) == 0);
        assert(mem.map(nullptr, map) != 0); // requested write must not fall back
        mock::badPage = 1;
        assert(mem.map(nullptr, rom) != 0); // read pin also rolls back on bad segment
        mock::badPage = -1;
        assert(mock::prepared == 1);
        assert(mem.map(nullptr, rom) == 0);
        mem.release(); mock::hostReadOnly = false;
    }
    noLeaks();
    {
        GuestMemory mem; assert(mem.init());
        auto rom = map; rom.flags = 5;
        assert(mem.map(nullptr, rom) == 0); // read mapping on writable host still pins write
        assert(mem.protect(map) == 0);
        mem.release();
    }
    noLeaks();
    {
        GuestMemory mem; assert(mem.init()); assert(mem.map(nullptr,map)==0);
        memset(buffer,0x5a,4*PAGE_SIZE); uint8_t bytes[2]={};
        assert(mem.readPhysical(map.guest+PAGE_SIZE-1,bytes,2,true) && bytes[0]==0x5a && bytes[1]==0x5a);
        auto part=map; part.guest+=PAGE_SIZE; part.size=PAGE_SIZE; part.flags=1;
        assert(mem.protect(part)==0); assert(!mem.readPhysical(part.guest,bytes,1,true));
        assert(mem.readPhysical(part.guest,bytes,1));
        uint8_t store[2]={0x11,0x22};
        assert(!mem.writePhysical(map.guest+PAGE_SIZE-1,store,2));
        assert(static_cast<uint8_t *>(buffer)[PAGE_SIZE-1]==0x5a);
        assert(mem.writePhysical(map.guest,store,2));
        assert(static_cast<uint8_t *>(buffer)[0]==0x11);
        part.flags=0; assert(mem.protect(part)==0);
        assert(!mem.readPhysical(part.guest,bytes,1)); assert(mem.unmap(part)==0);
        assert(!mem.readPhysical(map.guest+PAGE_SIZE-1,bytes,2));
        assert(!mem.readPhysical((1ull<<48)-1,bytes,2));
        mem.release();
    }
    noLeaks();
    {
        // Stub descriptors model dense mappings without allocating guest RAM.
        // Exercise actual GuestMemory/NPT and global accounting, not a copy.
        GuestMemory a,b,c;assert(a.init()&&b.init()&&c.init());
        abi::Map large{0,0x100000000ull,0,8ull<<30,7};
        assert(a.map(nullptr,large)==0);assert(b.map(nullptr,large)==0);
        auto page=map;page.guest=0;page.size=PAGE_SIZE;
        constexpr int NoResources=(int)0xfae94005;
        assert(c.map(nullptr,page)==NoResources); // 16GiB aggregate pins
        auto over=large;over.guest=16ull<<30;over.size=PAGE_SIZE;
        assert(a.map(nullptr,over)==NoResources); // budget is shared by all VMs
        auto first=large;first.size=PAGE_SIZE;assert(a.unmap(first)==0);
        assert(c.map(nullptr,page)==NoResources); // descriptor still prepared
        assert(a.unmap(large)==0);assert(c.map(nullptr,page)==0);
        a.release();b.release();c.release();
        // One VM may take the whole budget: a single 16 GiB mapping (more than
        // the former 8 GiB per-VM cap and 8192-node NPT bound) must succeed.
        GuestMemory big;assert(big.init());
        abi::Map sixteen{0,0x100000000ull,0,16ull<<30,7};
        assert(big.map(nullptr,sixteen)==0);
        assert(c.init()&&c.map(nullptr,page)==NoResources);
        assert(big.unmap(sixteen)==0);assert(c.map(nullptr,page)==0);
        big.release();c.release();
        GuestMemory reuse;assert(reuse.init());
        mock::badPage=1;assert(reuse.map(nullptr,large)!=0);mock::badPage=-1;
        assert(reuse.map(nullptr,large)==0); // rollback did not consume budget
        reuse.release();
    }
    noLeaks(); free(buffer);
    puts("PASS: NPT permissions/boundaries, partial unmap/remap, overlap, rollback and pin cleanup");
}
