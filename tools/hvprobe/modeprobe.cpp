#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned failures;
static bool repairCs;
static void check(bool ok, const char *name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name); failures += !ok;
}
static bool field(hv_vcpuid_t cpu, unsigned enc, uint64_t expected) {
    uint64_t actual = ~0ull;
    auto r = hv_vmx_vcpu_read_vmcs(cpu, enc, &actual);
    if (r || actual != expected) printf("VMCS %x actual=%llx expected=%llx result=%x\n", enc, actual, expected, r);
    return !r && actual == expected;
}
static bool set(hv_vcpuid_t cpu, unsigned enc, uint64_t value) {
    return hv_vmx_vcpu_write_vmcs(cpu, enc, value) == HV_SUCCESS;
}
static void runMode(bool wide) {
    printf("Testing %s\n", wide ? "long64" : "protected32");
    if (hv_vm_create(HV_VM_DEFAULT)) { check(false, "create VM"); return; }
    hv_vcpuid_t cpu;
    if (hv_vcpu_create(&cpu, HV_VCPU_DEFAULT)) { check(false, "create vCPU"); hv_vm_destroy(); return; }
    void *backing = nullptr;
    if (posix_memalign(&backing, 4096, 5*4096)) { check(false, "allocate backing"); hv_vcpu_destroy(cpu); hv_vm_destroy(); return; }
    memset(backing, 0, 5*4096);
    auto bytes = static_cast<unsigned char *>(backing);
    auto table = reinterpret_cast<uint64_t *>(bytes);
    // Four guest page-table levels: virtual 0x100000 -> guest physical 0x8000.
    table[0] = 0x2003; table[512] = 0x3003; table[1024] = 0x4003; table[1536+256] = 0x8003;
    check(hv_vm_map(backing, 0x1000, 4*4096, HV_MEMORY_READ|HV_MEMORY_WRITE) == HV_SUCCESS, "map page tables");
    check(hv_vm_map(bytes+4*4096, 0x8000, 4096, HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC) == HV_SUCCESS, "map code/data");
    // Flat32: mov eax,magic; mov [0x8100],eax; cpuid; hlt.
    const unsigned char code32[] = {0xb8,0x78,0x56,0x34,0x12,0xa3,0x00,0x81,0,0,0x0f,0xa2,0xf4};
    // Long64: mov rax,magic64; mov rbx,0x100100; mov [rbx],rax; cpuid; hlt.
    const unsigned char code64[] = {0x48,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
        0x48,0xbb,0x00,0x01,0x10,0,0,0,0,0,0x48,0x89,0x03,0x0f,0xa2,0xf4};
    memcpy(bytes+4*4096, wide ? code64 : code32, wide ? sizeof(code64) : sizeof(code32));
    bool configured = set(cpu, 0x6800, wide ? 0x80000011 : 0x11) &&
        set(cpu, 0x6804, wide ? 0x20 : 0) && set(cpu, 0x2806, wide ? 0x500 : 0) &&
        set(cpu, 0x6802, wide ? 0x1000 : 0) && set(cpu, 0x681e, wide ? 0x100000 : 0x8000) &&
        set(cpu, 0x681c, wide ? 0x100ff0 : 0x8ff0);
    for (unsigned i = 0; i < 6; ++i) {
        configured &= set(cpu, 0x800+i*2, i==1 ? 8 : 16);
        configured &= set(cpu, 0x4800+i*2, 0xffffffff);
        configured &= set(cpu, 0x4814+i*2, i==1 ? (wide ? 0xa09b : 0xc09b) : 0xc093);
        configured &= set(cpu, 0x6806+i*2, 0);
    }
    check(configured, "configure CPU mode");
    auto result = hv_vcpu_run(cpu);
    printf("run result=%x\n", result);
    check(result == HV_SUCCESS && field(cpu, 0x4402, 10), "CPUID exit");
    uint64_t magic = wide ? 0x1122334455667788ull : 0x12345678;
    uint64_t actual = 0;
    check(hv_vcpu_read_register(cpu, HV_X86_RAX, &actual) == HV_SUCCESS && actual == magic, "full-width register result");
    memcpy(&actual, bytes+4*4096+0x100, wide ? 8 : 4);
    check(actual == magic, "guest address translation and RAM write");
    for (unsigned i = 0; i < 6; ++i) {
        uint64_t selector=0, ar=0, limit=0, base=0;
        hv_vmx_vcpu_read_vmcs(cpu, 0x800+i*2, &selector);
        hv_vmx_vcpu_read_vmcs(cpu, 0x4814+i*2, &ar);
        hv_vmx_vcpu_read_vmcs(cpu, 0x4800+i*2, &limit);
        hv_vmx_vcpu_read_vmcs(cpu, 0x6806+i*2, &base);
        printf("segment %u: sel=%llx ar=%llx limit=%llx base=%llx\n", i, selector, ar, limit, base);
    }
    uint64_t cpuidRip = wide ? 0x100017 : 0x800a;
    check(field(cpu, 0x681e, cpuidRip) && field(cpu, 0x440c, 2), "exit RIP and length");
    check(set(cpu, 0x681e, cpuidRip+2), "advance RIP");
    if (repairCs) check(set(cpu, 0x4816, wide ? 0xa09b : 0xc09b), "diagnostic: restore CS.G before resume");
    check(hv_vcpu_run(cpu) == HV_SUCCESS && field(cpu, 0x4402, 12), "HLT exit");
    if (wide) {
        check(hv_vm_unmap(0x4000, 4096) == HV_SUCCESS, "remove guest PT backing");
        check(set(cpu, 0x681e, 0x100000), "restart paged fetch");
        if (repairCs) check(set(cpu, 0x4816, 0xa09b), "diagnostic: restore CS.G before page walk");
        check(hv_vcpu_run(cpu) == HV_SUCCESS && field(cpu, 0x4402, 48), "page walk blocked by NPT");
        check(field(cpu, 0x2400, 0x4800), "nested fault identifies guest PTE address");
        check(hv_vm_unmap(0x1000, 3*4096) == HV_SUCCESS, "unmap remaining tables");
    } else check(hv_vm_unmap(0x1000, 4*4096) == HV_SUCCESS, "unmap tables");
    check(hv_vm_unmap(0x8000, 4096) == HV_SUCCESS, "unmap code/data");
    check(hv_vcpu_destroy(cpu) == HV_SUCCESS, "destroy vCPU");
    check(hv_vm_destroy() == HV_SUCCESS, "destroy VM");
    free(backing);
}
int main(int argc, char **argv) {
    repairCs = argc == 2 && !strcmp(argv[1], "--repair-cs");
    if (argc > 1 && !repairCs) return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    runMode(false); runMode(true);
    printf("%s: CPU mode gate (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
