// Phase 3 VM/memory integration gate. Run only after enabling AMDHV 0.3.x.
#include <Hypervisor/hv.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "hv_abi.hpp"
using namespace amdhv;
static unsigned failures;
static void check(bool ok, const char *name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
static uint32_t rawTask(uint64_t index, const void *arg) {
    uint64_t result = 0x3000000;
    asm volatile("syscall" : "+a"(result) : "D"(index), "S"(arg) : "rcx", "r11", "memory");
    return (uint32_t)result;
}
static int live() {
    int n = -1; size_t size = sizeof(n);
    if (sysctlbyname("debug.amdhv.vms", &n, &size, nullptr, 0)) return -1;
    return n;
}
static bool waitEmpty() {
    for (int i = 0; i < 100; ++i) {
        if (live() == 0) return true;
        usleep(20000);
    }
    return false;
}
static void childExit(const char *self, const char *mode) {
    pid_t pid = fork();
    if (pid == 0) { execl(self, self, mode, nullptr); _exit(127); }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) { check(false, "child process"); return; }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, mode);
    check(waitEmpty(), "task exit reclaimed VM and mappings");
}
static bool readOnly(uint64_t address) {
    mach_vm_address_t start = address; mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return kr == KERN_SUCCESS && start <= address && address - start < size &&
           (info.protection & VM_PROT_READ) && !(info.protection & VM_PROT_WRITE);
}
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--deny")) {
        auto result = hv_vm_create(HV_VM_DEFAULT);
        if (result == HV_SUCCESS) hv_vm_destroy();
        check(result == HV_DENIED, "missing hypervisor entitlement denied");
        return failures ? 1 : 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "--exit") || !strcmp(argv[1], "--hold"))) {
        if (hv_vm_create(HV_VM_DEFAULT) != HV_SUCCESS) return 1;
        void *memory = nullptr;
        if (posix_memalign(&memory, 4096, 16384)) return 1;
        memset(memory, 0, 16384);
        if (hv_vm_map(memory, 0, 16384, HV_MEMORY_READ | HV_MEMORY_WRITE) != HV_SUCCESS) return 1;
        if (!strcmp(argv[1], "--hold")) sleep(3);
        _exit(0); // Deliberately omit destroy/unmap/free: exercise XNU task callback.
    }
    if (argc != 1) return 2;
    if (live() != 0) { fprintf(stderr, "need idle AMDHV 0.3.x with takeover enabled\n"); return 1; }
    check(rawTask(0, (void *)1) == (uint32_t)HV_FAULT, "copyin failure rejected");
    abi::VmCreate bad{0, 1};
    check(rawTask(0, &bad) == (uint32_t)HV_FAULT, "copyout failure rolls back allocation");
    check(live() == 0, "failed create publishes no target");
    abi::Interface output{};
    abi::VmCreate args{0, (uintptr_t)&output};
    uint32_t result = rawTask(0, &args);
    check(result == HV_SUCCESS, "raw create + interface copyout");
    if (result != HV_SUCCESS) return 1;
    check(output.rwFpOffset == abi::RwFpOffset && output.roFpOffset == abi::RoFpOffset &&
          output.fpStride == abi::FpStride && output.roMsrOffset == abi::RoMsrOffset &&
          output.msrStride == abi::MsrStride, "interface offsets/strides");
    check(output.rw && output.ro && output.clock, "all shared mappings exist");
    if (output.rw && output.ro && output.clock) {
        *(volatile uint64_t *)output.rw = 0x12345678;
        check(*(volatile uint64_t *)output.rw == 0x12345678, "RW interface writable");
        check(readOnly(output.ro) && readOnly(output.clock), "RO interface and clock permissions");
    }
    check(rawTask(0, &args) == (uint32_t)HV_ERROR, "duplicate create rejected without replacing target");
    check(rawTask(1, nullptr) == HV_SUCCESS, "raw destroy");
    check(waitEmpty(), "raw target released");
    for (unsigned i = 0; i < 32; ++i) {
        if (hv_vm_create(HV_VM_DEFAULT) != HV_SUCCESS || hv_vm_destroy() != HV_SUCCESS) {
            check(false, "framework create/destroy loop"); return 1;
        }
    }
    check(waitEmpty(), "32 framework create/destroy cycles");
    if (hv_vm_create(HV_VM_DEFAULT) != HV_SUCCESS) { check(false, "memory-test VM create"); return 1; }
    void *memory = nullptr;
    if (posix_memalign(&memory, 4096, 16384)) return 1;
    memset(memory, 0xA5, 16384);
    check(hv_vm_map(memory, 0, 16384, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) == HV_SUCCESS, "map 4 pages");
    check(hv_vm_map(memory, 0, 4096, HV_MEMORY_READ) == HV_BAD_ARGUMENT, "overlap rejected");
    check(hv_vm_map((char *)memory + 1, 0x10000, 4096, HV_MEMORY_READ) == HV_BAD_ARGUMENT, "unaligned host rejected");
    check(hv_vm_protect(4096, 8192, 0) == HV_SUCCESS, "protect middle pages NONE");
    check(hv_vm_map(memory, 4096, 4096, HV_MEMORY_READ) == HV_BAD_ARGUMENT, "NONE pages retain ownership");
    check(hv_vm_unmap(4096, 8192) == HV_SUCCESS, "partial unmap");
    check(hv_vm_protect(0, 16384, HV_MEMORY_READ) == HV_BAD_ARGUMENT, "protect with hole rejected");
    check(hv_vm_map((char *)memory + 4096, 4096, 8192, HV_MEMORY_READ) == HV_SUCCESS, "remap hole");
    check(hv_vm_unmap(0, 16384) == HV_SUCCESS, "full unmap releases both backings");
    check(hv_vm_destroy() == HV_SUCCESS, "memory-test VM destroy");
    free(memory);
    check(waitEmpty(), "all explicit VMs released");
    childExit(argv[0], "--exit");
    printf("%s: phase 3 VM/memory gate (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
