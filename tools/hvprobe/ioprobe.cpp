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
    printf("IO mode%u\n",bits);
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
    check(ok,"configure IO guest");
    for(unsigned width : {1u,2u,4u}) for(bool input : {false,true}) for(bool immediate : {false,true}) {
        unsigned n=0;
        if(width>1 && ((bits==16 && width==4)||(bits!=16 && width==2))) code[n++]=0x66;
        code[n++]=(immediate?0xe4:0xec)+(input?0:2)+(width==1?0:1);
        if(immediate) code[n++]=0x80;
        code[n]=0x0f;code[n+1]=0xa2;
        hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RDX,0x3f8);
        hv_vcpu_write_register(c,HV_X86_RAX,0x12345678);
        uint64_t qual=(uint64_t(immediate?0x80:0x3f8)<<16)|(width-1)|(input?8:0)|(immediate?64:0);
        auto r=hv_vcpu_run(c);
        check(r==0 && field(c,VMCS_RO_EXIT_REASON,30) && field(c,VMCS_RO_EXIT_QUALIFIC,qual) &&
            field(c,VMCS_RO_VMEXIT_INSTR_LEN,n) && reg(c,HV_X86_RIP,0x8000) && reg(c,HV_X86_RAX,0x12345678),
            "scalar IO exits before execution with exact port/width/direction/encoding");
        // Only userspace supplies an IN result and advances RIP.
        hv_vcpu_write_register(c,HV_X86_RAX,0x5a); hv_vcpu_write_register(c,HV_X86_RIP,0x8000+n);
        check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RAX,0x5a),"userspace IO emulation resumes to CPUID");
    }
    for(bool input : {false,true}) {
        code[0]=0xf3; code[1]=input?0x6c:0x6e;
        hv_vcpu_write_register(c,HV_X86_RIP,0x8000); hv_vcpu_write_register(c,HV_X86_RDX,0x3f8);
        hv_vcpu_write_register(c,HV_X86_RCX,2); hv_vcpu_write_register(c,HV_X86_RSI,0x8100);hv_vcpu_write_register(c,HV_X86_RDI,0x8200);
        code[0x200]=0xa5;
        uint64_t info=(uint64_t(bits==16?0:bits==32?1:2)<<7)|(uint64_t(input?0:3)<<15);
        check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,30) &&
            field(c,VMCS_RO_EXIT_QUALIFIC,(0x3f8ull<<16)|48|(input?8:0)) && field(c,0x440e,info) &&
            reg(c,HV_X86_RCX,2) && reg(c,HV_X86_RSI,0x8100) && reg(c,HV_X86_RDI,0x8200) && code[0x200]==0xa5,
            "REP string IO metadata exact; indices/count/memory remain untouched");
    }
    code[4095]=0xe6;code[4096]=0x80;hv_vcpu_write_register(c,HV_X86_RIP,0x8fff);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,30) && field(c,VMCS_RO_EXIT_QUALIFIC,0x800040),"IO decode handles instruction crossing code-page boundary");
    check(hv_vm_unmap(0x1000,4*4096)==0 && hv_vm_unmap(0x8000,2*4096)==0,"unmap IO guest");
    free(p);check(hv_vcpu_destroy(c)==0 && hv_vm_destroy()==0,"IO cleanup");
}
int main() {setvbuf(stdout,nullptr,_IONBF,0);mode(16);mode(32);mode(64);return failures?1:0;}
