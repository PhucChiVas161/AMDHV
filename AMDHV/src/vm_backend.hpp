#pragma once
#include "kernel_resolver.hpp"
namespace amdhv {
bool vmBackendInit(const KernelApi &api);
void vmBackendEnable();
// Atomically closes admission only if every VM/target has been destroyed.
bool vmBackendQuiesce();
HvCallbacks vmBackendCallbacks();
kern_return_t vmBackendTrap(int type, unsigned index, uint64_t arg);
unsigned vmBackendCount();
bool parallelRunEnabled();
void setParallelRunEnabled(bool enabled); // only while takeover disabled and no VMs
}
