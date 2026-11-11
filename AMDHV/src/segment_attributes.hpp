#pragma once
#include <stdint.h>
namespace amdhv {
// VMX places AVL/L/DB/G at 12..15; SVM packs them into 8..11.
constexpr uint16_t svmSegmentAttributes(uint64_t ar) {
    return (uint16_t)((ar & 0xff) | ((ar >> 4) & 0xf00));
}
constexpr uint64_t vmxSegmentAttributes(uint16_t ar) {
    return (ar & 0xff) | ((uint64_t)(ar & 0xf00) << 4);
}
constexpr uint64_t vmxSavedSegmentAttributes(uint16_t ar, uint32_t limit) {
    return (vmxSegmentAttributes(ar) & ~0x8000ull) | (limit > 0xfffff ? 0x8000ull : 0);
}
// SVM omits some cache metadata that VMX requires. Normalize only known
// architectural invariants, without relaxing input validation of guest caches.
constexpr uint64_t vmxSavedCacheAttributes(unsigned segment,uint16_t ar,uint32_t limit) {
    uint64_t result=vmxSavedSegmentAttributes(ar,limit);
    // AMD can leave DB set in a null SS cache. Not-present data/LDT caches
    // become VMX-unusable; their type/DB/G fields are immaterial.
    if(segment!=1 && segment!=7 && !(ar&0x80))return 0x10000;
    // LTR sets the GDT busy bit, but AMD can omit it in the saved TR type.
    if(segment==7 && (ar&0x9f)==0x89)result|=2;
    // A loaded present data descriptor is accessed even if AMD omits that bit.
    if(segment<6 && segment!=1 && segment!=2 && (ar&0x90)==0x90)result|=1;
    return result;
}
constexpr uint64_t vmxSavedCplAttributes(unsigned segment,uint16_t ar,uint32_t limit,unsigned cpl) {
    const uint64_t value=vmxSavedCacheAttributes(segment,ar,limit);
    // SYSRET can leave stale SS cache DPL on AMD. Actual VMCB.CPL is
    // authoritative, including for unusable SS; never synthesize present/type.
    return segment==2 ? (value&~0x60ull)|((uint64_t(cpl)&3)<<5) : value;
}
}
