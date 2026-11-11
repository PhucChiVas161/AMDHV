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
    printf("TLB mode%u\n",bits);
    if(hv_vm_create(HV_VM_DEFAULT)) {check(false,"create VM"); return;}
    hv_vcpuid_t c; if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)) {check(false,"create CPU"); hv_vm_destroy(); return;}
    void *p=nullptr; if(posix_memalign(&p,4096,7*4096)) {hv_vcpu_destroy(c); hv_vm_destroy(); check(false,"allocate");return;}
    memset(p,0,7*4096); auto b=static_cast<unsigned char*>(p); auto code=b+4*4096;
    auto t=static_cast<uint64_t*>(p);t[0]=0x2003;t[512]=0x3003;t[1024]=0x4003;t[1536+8]=0x8003;t[1536+9]=0x9003;
    bool ok=hv_vm_map(p,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)==0 &&
        hv_vm_map(code,0x8000,3*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC)==0;
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
    check(ok,"configure TLB guest");
    hv_vmx_vcpu_write_vmcs(c,VMCS_GUEST_CR4,0xa0); // PAE + PGE
    t[1536+9]=0x9103;t[1536+10]=0xa103;t[1536+11]=0x4003; // writable alias of leaf PT
    const uint64_t markerA=0x1111222233334444ull,markerB=0xaaaabbbbccccddddull;
    memcpy(code+4096,&markerA,8);memcpy(code+8192,&markerB,8);
    const unsigned char prefix[]={0x48,0xbb,0,0x90,0,0,0,0,0,0,0x48,0x8b,0x03,0x48,0x89,0xc2,
        0x48,0xb9,0x48,0xb0,0,0,0,0,0,0,0x48,0xc7,0x01,0x03,0xa1,0,0};
    for(bool invlpg : {true,false}) {
        unsigned n=sizeof(prefix);memcpy(code,prefix,n);
        if(invlpg) {const unsigned char op[]={0x0f,0x01,0x3b};memcpy(code+n,op,sizeof(op));n+=sizeof(op);}
        else {const unsigned char op[]={0xb8,0,0x10,0,0,0x0f,0x22,0xd8};memcpy(code+n,op,sizeof(op));n+=sizeof(op);}
        const unsigned char end[]={0x48,0x8b,0x03,0x0f,0xa2};memcpy(code+n,end,sizeof(end));
        t[1536+9]=0x9103;hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
        check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RDX,markerA) && reg(c,HV_X86_RAX,markerB),
            invlpg ? "INVLPG observes rewritten global PTE within one quantum" : "same-value MOV CR3 observes rewritten global PTE within one quantum");
    }
    const unsigned char read[]={0x48,0xbb,0,0x90,0,0,0,0,0,0,0x48,0x8b,0x03,0x0f,0xa2};
    memcpy(code,read,sizeof(read));t[1536+9]=0x9103;hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    check(hv_vcpu_run(c)==0 && reg(c,HV_X86_RAX,markerA),"populate original nested translation");
    check(hv_vm_unmap(0x9000,4096)==0 && hv_vm_map(code+8192,0x9000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE)==0,
        "replace NPT backing between run calls");
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    check(hv_vcpu_run(c)==0 && reg(c,HV_X86_RAX,markerB),"new quantum flushes stale NPT backing");
    const unsigned char write[]={0x48,0xbb,0,0x90,0,0,0,0,0,0,0xc6,0x03,0x5a,0x0f,0xa2};
    memcpy(code,write,sizeof(write));hv_vm_protect(0x9000,4096,HV_MEMORY_READ);
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,48) && code[8192]==0xdd,
        "new quantum honors NPT write downgrade before touching memory");
    code[0]=0x0f;code[1]=0x01;code[2]=0xdf;hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED,"guest INVLPGA cannot target host ASIDs");
    check(hv_vm_unmap(0x1000,4*4096)==0 && hv_vm_unmap(0x8000,3*4096)==0,"unmap TLB guest");
    free(p);check(hv_vcpu_destroy(c)==0 && hv_vm_destroy()==0,"TLB cleanup");
}
int main() {setvbuf(stdout,nullptr,_IONBF,0);mode(64);return failures?1:0;}
