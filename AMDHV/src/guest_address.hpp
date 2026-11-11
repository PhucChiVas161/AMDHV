#pragma once
#include <stdint.h>
namespace amdhv {
// Four-level guest virtual addresses; these are never host pointers. LA57 is
// still rejected by CR4 policy. Both canonical halves are legal for code64.
constexpr bool canonical48(uint64_t address) {
    return (address>>47)==0 || (address>>47)==0x1ffffull;
}
}
