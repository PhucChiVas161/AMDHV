#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned failures;
static void check(bool ok, const char *name) { printf("%s: %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }
static bool read(hv_vcpuid_t cpu, unsigned field, uint64_t expected) {
    uint64_t value = ~0ull;
    auto result = hv_vmx_vcpu_read_vmcs(cpu, field, &value);
    if (result || value != expected) printf("field 0x%x result=0x%x actual=0x%llx expected=0x%llx\n", field, result, value, expected);
    return result == HV_SUCCESS && value == expected;
}
static void *threadExit(void *) {
    hv_vcpuid_t cpu;
    // Deliberately leave the thread target behind to test XNU thread-destroy cleanup.
    return (void *)(uintptr_t)(hv_vcpu_create(&cpu, HV_VCPU_DEFAULT) != HV_SUCCESS);
}
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool run = argc == 2 && !strcmp(argv[1], "--run");
    bool leave = argc == 2 && !strcmp(argv[1], "--exit");
    if (argc > 1 && !run && !leave) return 2;
    if (hv_vm_create(HV_VM_DEFAULT) != HV_SUCCESS) { check(false, "VM create"); return 1; }
    hv_vcpuid_t cpu;
    if (hv_vcpu_create(&cpu, HV_VCPU_DEFAULT) != HV_SUCCESS) { check(false, "vCPU create"); hv_vm_destroy(); return 1; }
    if (leave) _exit(0);
    check(cpu == 0, "first vCPU slot");
    check(hv_vm_destroy() == HV_ERROR, "VM destroy refused with live vCPU");
    check(hv_vcpu_write_register(cpu, HV_X86_RAX, 0xabcdef0123456789ull) == HV_SUCCESS, "write GPR through shared region");
    uint64_t value = 0;
    check(hv_vcpu_read_register(cpu, HV_X86_RAX, &value) == HV_SUCCESS && value == 0xabcdef0123456789ull, "read GPR roundtrip");
    check(hv_vcpu_write_register(cpu, HV_X86_RIP, 0x1234) == HV_SUCCESS && read(cpu, VMCS_GUEST_RIP, 0x1234), "register/VMCS alias RIP");
    check(hv_vmx_vcpu_write_vmcs(cpu, VMCS_GUEST_RSP, 0x800) == HV_SUCCESS, "VMCS write RSP");
    check(hv_vcpu_read_register(cpu, HV_X86_RSP, &value) == HV_SUCCESS && value == 0x800, "VMCS/register alias RSP");
    void *memory = nullptr;
    if (run) {
        if (posix_memalign(&memory, 4096, 4096)) return 1;
        memset(memory, 0, 4096);
        // mov dword [0x100],0xdeadbeef; mov eax,0x12345678; cpuid; hlt
        const unsigned char code[] = {0x66,0xc7,0x06,0x00,0x01,0xef,0xbe,0xad,0xde,
                                     0x66,0xb8,0x78,0x56,0x34,0x12,0x0f,0xa2,0xf4};
        memcpy(memory, code, sizeof(code));
        check(hv_vm_map(memory, 0, 4096, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) == HV_SUCCESS, "map guest code/data");
        check(hv_vcpu_write_register(cpu, HV_X86_RIP, 0) == HV_SUCCESS, "set guest entry");
        hv_return_t result = hv_vcpu_run(cpu);
        printf("run CPUID result=0x%x\n", result);
        check(result == HV_SUCCESS && read(cpu, VMCS_RO_EXIT_REASON, 10), "SVM CPUID -> VMX CPUID exit");
        check(hv_vcpu_read_register(cpu, HV_X86_RAX, &value) == HV_SUCCESS && value == 0x12345678, "guest MOV updated RAX");
        check(*(volatile uint32_t *)((char *)memory + 0x100) == 0xdeadbeef, "guest write reached host backing page through NPT");
        check(read(cpu, VMCS_GUEST_RIP, 15) && read(cpu, VMCS_RO_VMEXIT_INSTR_LEN, 2), "CPUID RIP and instruction length");
        check(hv_vcpu_write_register(cpu, HV_X86_RIP, 17) == HV_SUCCESS, "advance past CPUID");
        result = hv_vcpu_run(cpu);
        printf("run HLT result=0x%x\n", result);
        check(result == HV_SUCCESS && read(cpu, VMCS_RO_EXIT_REASON, 12), "SVM HLT -> VMX HLT exit");
        check(hv_vm_unmap(0, 4096) == HV_SUCCESS, "unmap guest page");
        check(hv_vcpu_write_register(cpu, HV_X86_RIP, 0) == HV_SUCCESS, "reset entry for NPF");
        result = hv_vcpu_run(cpu);
        check(result == HV_SUCCESS && read(cpu, VMCS_RO_EXIT_REASON, 48) && read(cpu, VMCS_GUEST_PHYSICAL_ADDRESS, 0), "unmapped fetch -> EPT violation");
        check(hv_vm_map(memory, 0, 4096, HV_MEMORY_READ | HV_MEMORY_WRITE) == HV_SUCCESS, "map without execute permission");
        result = hv_vcpu_run(cpu);
        check(result == HV_SUCCESS && read(cpu, VMCS_RO_EXIT_REASON, 48), "NPT NX blocks instruction fetch");
        check(hv_vm_protect(0, 4096, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) == HV_SUCCESS, "restore execute permission");
        ((unsigned char *)memory)[0] = 0xeb; ((unsigned char *)memory)[1] = 0xfe;
        check(hv_vcpu_run(cpu) == HV_SUCCESS, "infinite loop yields at step budget");
        check(read(cpu, VMCS_RO_EXIT_REASON, 1), "step budget reports host exit");
        check(hv_vcpu_write_register(cpu, HV_X86_CR0, 0x80000011) == HV_SUCCESS, "request unsupported paging configuration");
        check(hv_vcpu_run(cpu) == HV_UNSUPPORTED, "unsupported paging configuration rejected");
        check(hv_vm_unmap(0, 4096) == HV_SUCCESS, "final unmap");
    }
    check(hv_vcpu_destroy(cpu) == HV_SUCCESS, "explicit vCPU destroy");
    for (unsigned i = 0; i < 8; ++i) {
        pthread_t thread; void *result = (void *)1;
        int err = pthread_create(&thread, nullptr, threadExit, nullptr);
        if (!err) err = pthread_join(thread, &result);
        check(!err && result == nullptr, "thread exit with live vCPU");
    }
    hv_return_t destroyed = HV_ERROR;
    for (unsigned i = 0; i < 100; ++i) {
        destroyed = hv_vm_destroy();
        if (destroyed == HV_SUCCESS) break;
        usleep(20000);
    }
    check(destroyed == HV_SUCCESS, "VM destroyed after thread cleanup");
    free(memory);
    if (argc == 1) {
        pid_t child = fork();
        if (child == 0) { execl(argv[0], argv[0], "--exit", nullptr); _exit(127); }
        int status = 0;
        check(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "process exit with live VM and vCPU");
    }
    printf("%s: vCPU %s gate (%u failures)\n", failures ? "FAIL" : "PASS", run ? "SVM" : "lifecycle", failures);
    return failures ? 1 : 0;
}
