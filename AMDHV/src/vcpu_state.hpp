#pragma once
#include "hv_abi.hpp"
#include "vmcs_fields.hpp"
#include "managed_msr.hpp"
namespace amdhv {
namespace abi {
// O(1) encoding lookup. VMCS encodings pack width/type in bits 14:10 and a
// small per-group index in bits 6:1. The table is built and verified at compile
// time, so constant encodings fold to a direct array slot.
constexpr unsigned VmcsKeyMask = 0x7c7e;
constexpr unsigned vmcsKey(unsigned encoding) {
    return ((encoding >> 10) & 0x1f) << 6 | ((encoding >> 1) & 0x3f);
}
struct VmcsIndexTable { uint8_t slot[2048]; };
constexpr VmcsIndexTable makeVmcsIndexTable() {
    VmcsIndexTable table{};
    for (auto entry : VmcsFields) table.slot[vmcsKey(entry.encoding)] = entry.index;
    return table;
}
constexpr bool vmcsIndexTableExact(const VmcsIndexTable &table) {
    unsigned used = 0, fields = 0;
    for (auto value : table.slot) if (value) ++used;
    for (auto entry : VmcsFields) {
        if ((entry.encoding & ~VmcsKeyMask) || !entry.index ||
            table.slot[vmcsKey(entry.encoding)] != entry.index) return false;
        ++fields;
    }
    return used == fields; // no two encodings share a slot
}
inline constexpr VmcsIndexTable VmcsIndexes = makeVmcsIndexTable();
static_assert(vmcsIndexTableExact(VmcsIndexes), "VMCS encoding table collision");
constexpr unsigned vmcsIndex(unsigned encoding) {
    return (encoding & ~VmcsKeyMask) ? 0u : VmcsIndexes.slot[vmcsKey(encoding)];
}
}
// Only guest/control/exit fields 1..91 are backed by this shared ABI window.
struct VcpuState {
    bool relativeTsc = false, tscSupported = true, invalidFp = false;
    bool loggedApEntry = false;
    uint8_t fp[512] = {};
    // Private standard-format XSAVE header and extended state. Never exposed
    // through the framework's 512-byte legacy FP window.
    uint8_t xsaveTail[3584] = {};
    void legacyFpChanged() { xsaveTail[0]|=3; }
    uint64_t lastSharedFlags = 0;
    uint64_t specCtrl = 0; // private guest SPEC_CTRL; no shared ABI offset
    void resetFp() {
        for (auto &v : fp) v = 0;
        for (auto &v : xsaveTail) v=0;
        legacyFpChanged();
        fp[0] = 0x7f; fp[1] = 3; // FCW=037f, empty x87 tags
        fp[24] = 0x80; fp[25] = 0x1f; // MXCSR=1f80
        fp[28] = 0xbf; fp[29] = 0xff; // conservative MXCSR capability mask
    }
    bool validFp() const {
        uint32_t mxcsr = 0;
        for (unsigned i=0; i<4; ++i) mxcsr |= uint32_t(fp[24+i]) << (i*8);
        return !(mxcsr & ~0xffbfu); // reject reserved bits and DAZ before FXRSTOR
    }
    uint64_t tscOffset = 0, tscSnapshot = 0;
    uint64_t managedMask = 0, msrReadMask = 0, msrWriteMask = 0, msrs[6] = {};
    uint64_t fields[92] = {};
    uint64_t registers[51] = {};
    uint64_t msrValue(unsigned id) const {
        unsigned field = managedMsrVmcs(id);
        return field ? get(field) : id >= 1 && id <= 5 ? msrs[id] : 0;
    }
    void setMsrValue(unsigned id, uint64_t value) {
        unsigned field = managedMsrVmcs(id);
        if (field) set(field, value);
        else if (id >= 1 && id <= 5) msrs[id] = value;
    }
    static constexpr unsigned index(unsigned encoding) { return abi::vmcsIndex(encoding); }
    uint64_t get(unsigned encoding) const {
        unsigned i = index(encoding); return i && i < 92 ? fields[i] : 0;
    }
    void set(unsigned encoding, uint64_t value) {
        unsigned i = index(encoding); if (i && i < 92) fields[i] = value;
    }
    static uint64_t read(const uint8_t *p, unsigned offset) {
        return *reinterpret_cast<const volatile uint64_t *>(p + offset);
    }
    static void write(uint8_t *p, unsigned offset, uint64_t value) {
        *reinterpret_cast<volatile uint64_t *>(p + offset) = value;
    }
    void reset() {
        loggedApEntry = false;
        specCtrl = 0;
        managedMask = msrReadMask = msrWriteMask = 0; for (auto &v : msrs) v = 0;
        invalidFp = false; lastSharedFlags = 0; resetFp();
        relativeTsc = false; tscSupported = true; tscOffset = tscSnapshot = 0;
        for (auto &v : fields) v = 0;
        for (auto &v : registers) v = 0;
        set(0x6800, 0x10); set(0x6820, 2); set(0x681a, 0x400);
        registers[47] = 0xffff0ff0; registers[50] = 1;
        for (unsigned i = 0; i < 8; ++i) {
            set(0x4800 + i * 2, 0xffff);
            set(0x4814 + i * 2, i == 1 ? 0x9b : i == 6 ? 0x82 : i == 7 ? 0x8b : 0x93);
        }
        set(0x4810, 0xffff); set(0x4812, 0x3ff);
        set(0x2806, 0); set(0x2802, 0x0007040600070406ull);
    }
    void pull(const uint8_t *rw, const uint8_t *rwFp = nullptr) {
        lastSharedFlags = read(rw, 0);
        if (lastSharedFlags & 0x80) {
            if (rwFp) __builtin_memcpy(fp, rwFp, sizeof(fp)); // single private snapshot
            invalidFp = !rwFp || !validFp();
            legacyFpChanged();
        }
        if (read(rw, 0) & 0x100) {
            tscSupported = relativeTsc;
            if (relativeTsc) tscOffset = read(rw, 0xc78);
        }
        for (unsigned id = 1; id <= 5; ++id)
            if (read(rw, 0xc50) & (1ull << id)) msrs[id] = read(rw, 0x7d0 + id * 8);
        // A private snapshot: userspace cannot mutate VMCB inputs after validation.
        for (unsigned i = 1; i <= 80; ++i) {
            if (read(rw, 0x300 + (i / 64) * 8) & (1ull << (i % 64)))
                fields[i] = read(rw, 8 + i * 8);
        }
        for (unsigned i = 0; i < 51; ++i) {
            unsigned idx = abi::RegisterIndex[i];
            registers[i] = idx ? fields[idx] : read(rw, 0xc80 + i * 8);
        }
    }
    void publish(uint8_t *rw, uint8_t *roFp = nullptr, uint8_t *rwFp = nullptr) const {
        if (rwFp) __builtin_memcpy(rwFp, fp, sizeof(fp));
        if (roFp) __builtin_memcpy(roFp, fp, sizeof(fp));
        if (relativeTsc) {
            write(rw, 0xc68, tscSnapshot); write(rw, 0xc70, tscOffset); write(rw, 0xc78, tscOffset);
        }
        for (unsigned id = 1; id <= 5; ++id) write(rw, 0x7d0 + id * 8, msrs[id]);
        write(rw, 0xc50, read(rw, 0xc50) & ~0x3eull);
        for (unsigned i = 1; i < 92; ++i) write(rw, 8 + i * 8, fields[i]);
        for (unsigned i = 0; i < 51; ++i) if (!abi::RegisterIndex[i]) write(rw, 0xc80 + i * 8, registers[i]);
        write(rw, 0x2f0, ~1ull); write(rw, 0x2f8, (1ull << 28) - 1);
        write(rw, 0x300, 0); write(rw, 0x308, 0); write(rw, 0, 0);
    }
};
}
