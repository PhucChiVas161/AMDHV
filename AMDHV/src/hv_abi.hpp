// Hypervisor.framework x86_64 ABI, verified against both sides of build 25G332.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace amdhv::abi {
struct VmCreate { uint64_t flags, interfaceOut; };
struct Map { uint64_t space, host, guest, size, flags; };
struct VcpuCreate { uint64_t idOut, flags; };
struct Interface {
    uint64_t rwFpOffset, roFpOffset, fpStride, roMsrOffset, msrStride;
    uint8_t reserved[0x100 - 5 * 8];
    uint64_t rw, ro, clock;
};
constexpr unsigned MaxVcpus = 64;
constexpr uint64_t RwStride = 0xe18, RoStride = 0x6d8;
constexpr uint64_t RwFpOffset = 0x38600, RoFpOffset = 0x1b600;
constexpr uint64_t FpStride = 512;
constexpr uint64_t RoMsrOffset = 0x24000, MsrStride = 4096;
constexpr uint64_t RwBytes = 0x41000, RoBytes = 0x64000;
static_assert(sizeof(VmCreate) == 16);
static_assert(sizeof(Map) == 40);
static_assert(sizeof(VcpuCreate) == 16);
static_assert(sizeof(Interface) == 0x118);
static_assert(offsetof(Interface, rw) == 0x100);
static_assert(RwFpOffset >= MaxVcpus * RwStride);
static_assert(RoFpOffset >= MaxVcpus * RoStride);
static_assert(RwBytes >= RwFpOffset + MaxVcpus * FpStride);
static_assert(RoMsrOffset >= RoFpOffset + MaxVcpus * FpStride);
static_assert(RoBytes >= RoMsrOffset + MaxVcpus * MsrStride);
}
