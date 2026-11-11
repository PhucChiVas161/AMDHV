// Reads lookup tables from this process's own Hypervisor.framework mapping.
#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
static const uint8_t *table(const char *symbol, unsigned offset) {
    const uint8_t *p = dlsym(RTLD_DEFAULT, symbol);
    if (p && !strcmp(symbol, "hv_vcpu_read_register")) {
        if (p[5] != 0xe9) return NULL;
        int32_t jump; memcpy(&jump, p + 6, 4); p += 10 + jump;
    }
    if (!p || memcmp(p + offset, "\x48\x8d", 2)) { fprintf(stderr, "symbol/LEA mismatch: %s at %p\n", symbol, (void *)p); return NULL; }
    int32_t displacement; memcpy(&displacement, p + offset + 3, 4);
    return p + offset + 7 + displacement;
}
int main(void) {
    const uint8_t *vmcs = table("hv_vmx_vcpu_read_vmcs", 14);
    const uint8_t *reg = table("hv_vcpu_read_register", 0x61);
    if (!vmcs || !reg) return 1;
    puts("// Derived from Hypervisor.framework 25G332 lookup tables.\n#pragma once\n#include <stdint.h>\nnamespace amdhv::abi {");
    puts("struct VmcsIndex { uint16_t encoding; uint8_t index; };\nstatic constexpr VmcsIndex VmcsFields[] = {");
    for (unsigned i = 0; i <= 0x6c17 / 2; ++i) if (vmcs[i]) printf("    {0x%04x, %u},\n", i * 2, vmcs[i]);
    puts("};\nstatic constexpr uint8_t RegisterIndex[] = {");
    for (unsigned i = 0; i <= 50; ++i) printf("%u,%s", reg[i], i % 16 == 15 ? "\n" : " ");
    puts("\n};\n}");
}
