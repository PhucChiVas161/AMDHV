// Decode only the pure address->ID lookup inside our own framework mapping.
#include <Hypervisor/hv.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const uint8_t *p = dlsym(RTLD_DEFAULT, "hv_vcpu_read_msr");
    int32_t displacement;
    if (!p || p[5] != 0xe9) return 1;
    memcpy(&displacement, p+6, 4); p += 10+displacement;
    if (p[15] != 0xe8) return 2;
    memcpy(&displacement, p+16, 4);
    uint32_t (*lookup)(uint32_t) = (void *)(p+20+displacement);
    const uint32_t addresses[] = {0x10,0x1b,0x174,0x175,0x176,0x1d9,0x277,
        0xc0000080,0xc0000081,0xc0000082,0xc0000083,0xc0000084,
        0xc0000100,0xc0000101,0xc0000102,0xc0000103};
    for (unsigned i=0;i<sizeof(addresses)/sizeof(*addresses);++i) {
        uint32_t v=lookup(addresses[i]);
        printf("MSR %08x id=%u field=%u\n", addresses[i], v>>16, v&65535);
    }
}
