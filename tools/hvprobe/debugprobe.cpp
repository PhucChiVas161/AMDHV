#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static unsigned failures;
static void check(bool ok,const char *label) { printf("%s: %s\n",ok?"PASS":"FAIL",label); failures+=!ok; }
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected) {
    uint64_t v=~0ull; auto r=hv_vmx_vcpu_read_vmcs(c,f,&v);
    if(r || v!=expected) printf("VMCS%x=%llx expected=%llx r=%x\n",f,v,expected,r);
    return !r && v==expected;
}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t expected) {
    uint64_t v=~0ull; return !hv_vcpu_read_register(c,r,&v) && v==expected;
}
static void mode(unsigned bits) {
    printf("DEBUG mode%u\n",bits);
    if(hv_vm_create(HV_VM_DEFAULT)) {check(false,"create VM"); return;}
    hv_vcpuid_t c; if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)) {check(false,"create CPU"); hv_vm_destroy(); return;}
    void *p=nullptr; if(posix_memalign(&p,4096,6*4096)) {hv_vcpu_destroy(c); hv_vm_destroy(); check(false,"allocate");return;}
    memset(p,0,6*4096); auto b=static_cast<unsigned char*>(p); auto code=b+4*4096;
    auto t=static_cast<uint64_t*>(p);t[0]=0x2003;t[512]=0x3003;t[1024]=0x4003;t[1536+8]=0x8003;t[1536+9]=0x9003;
    bool ok=hv_vm_map(p,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)==0 &&
        hv_vm_map(code,0x8000,2*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC)==0;
    if(bits!=16) {
        ok &= hv_vmx_vcpu_write_vmcs(c,0x6800,bits==64?0x80000011:0x11)==0;
        ok &= hv_vmx_vcpu_write_vmcs(c,0x6804,bits==64?0x20:0)==0;
        ok &= hv_vmx_vcpu_write_vmcs(c,0x2806,bits==64?0x500:0)==0;
        ok &= hv_vmx_vcpu_write_vmcs(c,0x6802,bits==64?0x1000:0)==0;
        for(unsigned i=0;i<6;++i) {
            ok &= hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16)==0;
            ok &= hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff)==0;
            ok &= hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?(bits==64?0xa09b:0xc09b):0xc093)==0;
        }
    }
    check(ok,"configure DEBUG guest");
    auto runToExit=[&]() -> hv_return_t {
        for(unsigned n=0;n<32;++n) {auto r=hv_vcpu_run(c); if(r)return r;uint64_t reason=0;
            if(hv_vmx_vcpu_read_vmcs(c,VMCS_RO_EXIT_REASON,&reason))return HV_ERROR;
            if(reason!=1)return HV_SUCCESS;}
        return HV_ERROR;
    };
    auto resetCode=[&](const std::vector<unsigned char> &bytes) {
        memcpy(code,bytes.data(),bytes.size());hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,2);hv_vmx_vcpu_write_vmcs(c,0x4824,0);
        hv_vmx_vcpu_write_vmcs(c,0x4002,0);
    };
    resetCode({0x0f,0x20,0xe0,0x83,0xc8,8,0x0f,0x22,0xe0,0x0f,0x20,0xe1,0x0f,0xa2});
    const uint64_t cr4WithDe=bits==64?0x28:8;
    check(runToExit()==0 && field(c,VMCS_RO_EXIT_REASON,10) &&
          reg(c,HV_X86_RCX,cr4WithDe) && field(c,0x6804,cr4WithDe),
          "CR4.DE can be enabled and read back before virtual DR roundtrips");
    for(unsigned dr : {0u,1u,2u,3u}) {
        resetCode({0x0f,0x23,(unsigned char)(0xc0+dr*8),0x0f,0x21,(unsigned char)(0xc1+dr*8),0x0f,0xa2});
        uint64_t value=bits==64?0x123456789ull:0x12345678;
        hv_vcpu_write_register(c,HV_X86_RAX,value);
        check(runToExit()==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RCX,value),
              "disabled guest DR address roundtrip survives separate run calls");
    }
    resetCode({0x0f,0x22,0xd0,0x0f,0xa2});
    const uint64_t cr2Value=bits==64?0xfedcba9876543210ull:0x76543210ull;
    hv_vcpu_write_register(c,HV_X86_RAX,cr2Value);
    check(runToExit()==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_CR2,cr2Value),
          "MOV CR2 publishes guest-only value across run calls");
    resetCode({0x0f,0x20,0xd1,0x0f,0xa2});
    check(runToExit()==0 && reg(c,HV_X86_RCX,cr2Value),"MOV from CR2 restores per-vCPU guest state");
    hv_vcpu_write_register(c,HV_X86_CR2,0x12345678);
    resetCode({0x0f,0x20,0xd1,0x0f,0xa2});
    check(runToExit()==0 && reg(c,HV_X86_RCX,0x12345678),"framework CR2 write reaches guest operand");
    for(unsigned dr : {6u,7u}) {
        resetCode({0x0f,0x23,(unsigned char)(0xc0+dr*8),0x0f,0x21,(unsigned char)(0xc1+dr*8),0x0f,0xa2});
        hv_vcpu_write_register(c,HV_X86_RAX,0);
        check(runToExit()==0 && reg(c,HV_X86_RCX,dr==6?0xffff0ff0ull:0x400ull),
              "DR6/DR7 reset writes read architectural fixed bits without internal debug state");
    }
    if(bits==64) {
        resetCode({0x41,0x0f,0x23,0xc0,0x41,0x0f,0x21,0xc1,0x0f,0xa2});
        hv_vcpu_write_register(c,HV_X86_R8,0x23456789aull);
        check(runToExit()==0 && reg(c,HV_X86_R9,0x23456789aull),"MOV DR decodes REX extended GPRs");
    }
    if(bits==64) {
        resetCode({0xb8,9,0,0,0,0x44,0x0f,0x22,0xc0,0x44,0x0f,0x20,0xc1,0x0f,0xa2});
        check(runToExit()==0&&field(c,VMCS_RO_EXIT_REASON,10)&&reg(c,HV_X86_RCX,9)&&reg(c,HV_X86_TPR,0x90),
              "CR8 roundtrip publishes APIC TPR in step mode");
        resetCode({0x44,0x0f,0x20,0xc0,0x0f,0xa2});hv_vmx_vcpu_write_vmcs(c,0x4002,0x100000);
        check(hv_vcpu_run(c)==0&&field(c,VMCS_RO_EXIT_REASON,28)&&field(c,VMCS_RO_EXIT_QUALIFIC,0x18)&&reg(c,HV_X86_RIP,0x8000),
              "CR8 load-exiting forwards VMX28 without consuming instruction");
        hv_vmx_vcpu_write_vmcs(c,0x4002,0);hv_vcpu_write_register(c,HV_X86_TPR,0);
    }
    resetCode({0x0f,0x23,0xf8,0x0f,0xa2});hv_vcpu_write_register(c,HV_X86_RAX,0x401);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED && reg(c,HV_X86_RIP,0x8000) && field(c,0x681a,0x400),
          "enabling guest breakpoint rejected without committing DR7 or RIP");
    resetCode({0x0f,0x21,0xc0,0x0f,0xa2});hv_vmx_vcpu_write_vmcs(c,0x4002,0x800000);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,29) && field(c,VMCS_RO_EXIT_QUALIFIC,16) &&
          reg(c,HV_X86_RIP,0x8000),"VMX MOV-DR exiting forwards qualification without emulation");
    resetCode({0x0f,0x21,0xc0,0xf4});hv_vcpu_write_register(c,HV_X86_RFLAGS,0x202);
    hv_vmx_vcpu_write_vmcs(c,0x4824,1);hv_vmx_vcpu_write_vmcs(c,0x4002,4);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,1) && field(c,0x4824,0) && reg(c,HV_X86_RIP,0x8003),
          "emulated MOV DR retires shadow");
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,7),"interrupt window opens after MOV DR");
    // SLDT/STR use the guest VMCB selectors, never host descriptor state.
    if(bits!=16) for(unsigned modrm : {0xc0u,0xc8u}) {
        resetCode({0x66,0x0f,0x00,(unsigned char)modrm,0x0f,0xa2});hv_vcpu_write_register(c,HV_X86_RAX,0);
        check(runToExit()==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RAX,0),
              "SLDT/STR report guest selector zero");
    }
    check(hv_vm_unmap(0x1000,4*4096)==0 && hv_vm_unmap(0x8000,2*4096)==0,"unmap DEBUG guest");
    free(p);check(hv_vcpu_destroy(c)==0 && hv_vm_destroy()==0,"DEBUG cleanup");
}
int main() {setvbuf(stdout,nullptr,_IONBF,0);mode(16);mode(32);mode(64);return failures?1:0;}
