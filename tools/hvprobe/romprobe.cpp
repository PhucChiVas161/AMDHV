// Verify host read-only backing, as used by the Virtualization.framework EFI ROM.
#include <Hypervisor/hv.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstring>
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (hv_vm_create(HV_VM_DEFAULT)) return 2;
    void *page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) { hv_vm_destroy(); return 2; }
    memset(page, 0xf4, 4096);
    if (mprotect(page, 4096, PROT_READ)) { munmap(page, 4096); hv_vm_destroy(); return 2; }
    hv_return_t result = hv_vm_map(page, 0xfff00000, 4096, HV_MEMORY_READ | HV_MEMORY_EXEC);
    printf("read-only host -> guest RX: result=0x%x (%s)\n", result, result ? "FAIL" : "PASS");
    bool ok = result == HV_SUCCESS;
    if (!result) {
        auto protect = hv_vm_protect(0xfff00000, 4096, HV_MEMORY_READ | HV_MEMORY_WRITE);
        printf("reject guest write upgrade on read-only host: %s\n", protect ? "PASS" : "FAIL");
        ok &= protect != HV_SUCCESS;
        ok &= hv_vm_protect(0xfff00000, 4096, HV_MEMORY_READ) == HV_SUCCESS;
        ok &= hv_vm_unmap(0xfff00000, 4096) == HV_SUCCESS;
        auto write = hv_vm_map(page, 0xfff00000, 4096, HV_MEMORY_READ | HV_MEMORY_WRITE);
        printf("reject writable map on read-only host: %s\n", write ? "PASS" : "FAIL");
        ok &= write != HV_SUCCESS;
        if (!write) hv_vm_unmap(0xfff00000, 4096);
    }
    ok &= hv_vm_destroy() == HV_SUCCESS;
    munmap(page, 4096);
    return ok ? 0 : 1;
}
