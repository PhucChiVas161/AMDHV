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
    printf("NX mode%u\n",bits);
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
    check(ok,"configure NX guest");
    // Same EFER path as native EFI: WRMSR exits; the userspace VMM publishes NXE.
    code[0]=0x0f;code[1]=0x30;code[2]=0x0f;code[3]=0xa2;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RCX,0xc0000080);
    hv_vcpu_write_register(c,HV_X86_RAX,0xd00);hv_vcpu_write_register(c,HV_X86_RDX,0);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,32),"EFER WRMSR exits to userspace VMM");
    hv_vmx_vcpu_write_vmcs(c,0x2806,0xd00);hv_vmx_vcpu_write_vmcs(c,0x4012,0x200);
    hv_vcpu_write_register(c,HV_X86_RIP,0x8002);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10),"VMM enables NXE and resumes IA32e guest");
    code[0x10]=0xe6;code[0x11]=0x80;
    auto io=[&]() {hv_vcpu_write_register(c,HV_X86_RIP,0x8010);
        return hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,30) && field(c,VMCS_RO_EXIT_QUALIFIC,0x800040);};
    check(io(),"IO instruction fetch succeeds with EFER.NXE enabled");
    const unsigned char dataCode[]={0x48,0xbb,0,0x90,0,0,0,0,0,0,0xc6,0x03,0x5a,0x0f,0xb6,0x03,0x0f,0xa2};
    memcpy(code+0x20,dataCode,sizeof(dataCode));t[1536+9]=0x8000000000009003ull;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8020);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RAX,0x5a) && code[4096]==0x5a,
        "NX data page remains readable and writable");
    hv_vcpu_write_register(c,HV_X86_RIP,0x9000);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED && reg(c,HV_X86_RIP,0x9000),"NX data page refuses instruction fetch before execution");
    t[1536+9]=0x9003;
    for(unsigned slot : {0u,512u,1024u,1544u}) {
        auto old=t[slot];t[slot]=old|(1ull<<63);hv_vcpu_write_register(c,HV_X86_RIP,0x8010);
        check(hv_vcpu_run(c)==HV_UNSUPPORTED && reg(c,HV_X86_RIP,0x8010),"NX at each page-table level blocks code");
        t[slot]=old;check(io(),"clearing NX resumes IO after TLB flush");
    }
    for(unsigned slot : {1024u,512u}) {
        auto old=t[slot];t[slot]=0x83;check(io(),"executable huge page supports IO fetch with NXE");
        t[slot]|=1ull<<63;hv_vcpu_write_register(c,HV_X86_RIP,0x8010);
        check(hv_vcpu_run(c)==HV_UNSUPPORTED && reg(c,HV_X86_RIP,0x8010),"NX huge page blocks code");
        t[slot]=old;
    }
    hv_vmx_vcpu_write_vmcs(c,0x2806,0x500);t[1544]|=1ull<<63;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8010);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED && reg(c,HV_X86_RIP,0x8010),"NX bit remains reserved when NXE is disabled");
    t[1544]&=~(1ull<<63);check(io(),"NXE-off legacy paging still resumes after clearing reserved NX");
    check(hv_vm_unmap(0x1000,4*4096)==0 && hv_vm_unmap(0x8000,2*4096)==0,"unmap NX guest");
    free(p);check(hv_vcpu_destroy(c)==0 && hv_vm_destroy()==0,"NX cleanup");
}
int main() {setvbuf(stdout,nullptr,_IONBF,0);mode(64);return failures?1:0;}
