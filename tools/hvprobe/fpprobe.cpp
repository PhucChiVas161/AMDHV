#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" hv_return_t fpu_run_preserved(void *, void *, void *, void *);
static unsigned failures;
static void check(bool ok,const char *label) { printf("%s: %s\n",ok?"PASS":"FAIL",label); failures+=!ok; }
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected) {
    uint64_t v=~0ull; auto r=hv_vmx_vcpu_read_vmcs(c,f,&v);
    if(r || v!=expected) printf("VMCS%x=%llx expected=%llx r=%x\n",f,v,expected,r);
    return !r && v==expected;
}
static void mode(unsigned bits) {
    printf("FP mode%u\n",bits);
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
    check(ok,"configure FP guest");
    ok &= hv_vmx_vcpu_write_vmcs(c,0x6804,bits==64?0x220:0x200)==0;
    check(ok,"enable guest OSFXSR without OSXSAVE");
    alignas(64) unsigned char fp[512]={}, got[512]={}, scratch[2048]={}, before[832]={}, after[832]={};
    check(hv_vcpu_read_fpstate(c,fp,sizeof(fp))==0 && fp[0]==0x7f && fp[1]==3 && fp[24]==0x80 && fp[25]==0x1f,"initial FP reset image");
    for(unsigned i=0;i<16;++i) fp[160+i]=0x5a;
    // x87 init + 1+1; SSE XMM0 invert, XMM1 zero; stop before CPUID.
    const unsigned char instructions[]={0x9b,0xdb,0xe3,0xd9,0xe8,0xd9,0xe8,0xde,0xc1,
        0x66,0x0f,0x76,0xc9,0x66,0x0f,0xef,0xc1,0x66,0x0f,0xef,0xc9,0x0f,0xa2,0xf4};
    memcpy(code,instructions,sizeof(instructions));
    check(hv_vcpu_write_fpstate(c,fp,sizeof(fp))==0,"write guest FP state");
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    auto r=fpu_run_preserved(fp,scratch,before,after);
    check(r==0 && field(c,VMCS_RO_EXIT_REASON,10),"x87 WAIT/FNINIT/arithmetic and SSE reach CPUID");
    bool host=memcmp(before,after,5)==0 && memcmp(before+6,after+6,26)==0 &&
        memcmp(before+160,after+160,256)==0 && memcmp(before+512,after+512,8)==0 &&
        memcmp(before+576,after+576,256)==0;
    for(unsigned i=0;i<8;++i) host &= memcmp(before+32+i*16,after+32+i*16,10)==0;
    check(host,"raw vCPU trap preserves host x87/XMM/YMM and XSAVE init flags");
    bool fpOK=hv_vcpu_read_fpstate(c,got,sizeof(got))==0;
    for(unsigned i=0;i<16;++i) fpOK &= got[160+i]==0xa5 && got[176+i]==0;
    check(fpOK && got[39]==0x80 && got[40]==0 && got[41]==0x40,"guest FP results published: ST0=2, XMM0=A5, XMM1=0");
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000+sizeof(instructions)-1);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,12),"FP guest resumes to HLT");
    unsigned char again[512]={};
    check(hv_vcpu_read_fpstate(c,again,sizeof(again))==0 && memcmp(got,again,416)==0,"guest FP state survives separate run calls");
    // MMX aliases x87 storage; verify a guest load/store and EMMS cleanup.
    const unsigned char mmx[]={0x0f,0x6f,(unsigned char)(bits==16?0x07:0x03),
        0x0f,0x7f,(unsigned char)(bits==16?0x47:0x43),0x08,0x0f,0x77,0xf4};
    memcpy(code,mmx,sizeof(mmx));
    const uint64_t marker=0x12345678aabbccddull;
    memcpy(code+0x100,&marker,8); memset(code+0x108,0,8);
    hv_vcpu_write_register(c,HV_X86_RBX,0x8100); hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,12) && memcmp(code+0x100,code+0x108,8)==0,
        "MMX guest memory roundtrip and EMMS reach HLT");
    check(hv_vcpu_read_fpstate(c,got,sizeof(got))==0 && got[4]==0,"EMMS publishes empty x87 tags");
    // Exception path must restore host state too; UD2 remains explicitly unsupported.
    code[0]=0x0f; code[1]=0x0b; hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    r=fpu_run_preserved(fp,scratch,before,after);
    host=memcmp(before,after,5)==0 && memcmp(before+6,after+6,26)==0 &&
        memcmp(before+160,after+160,256)==0 && memcmp(before+512,after+512,8)==0 &&
        memcmp(before+576,after+576,256)==0;
    for(unsigned i=0;i<8;++i) host &= memcmp(before+32+i*16,after+32+i*16,10)==0;
    check(r==HV_UNSUPPORTED && host,"unsupported guest exception restores host FP state");
    code[0]=0xf4;
    // An invalid MXCSR must never reach host FXRSTOR; a valid explicit rewrite recovers.
    got[27]=0x80; hv_vcpu_write_fpstate(c,got,sizeof(got));
    check(hv_vcpu_run(c)==HV_UNSUPPORTED,"reject reserved MXCSR before hardware entry");
    got[27]=0; hv_vcpu_write_fpstate(c,got,sizeof(got));
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,12),"valid FP rewrite recovers after rejection");
    hv_vmx_vcpu_write_vmcs(c,0x6804,(bits==64?0x220:0x200)|0x40000);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED,"OSXSAVE and guest AVX remain outside the supported state subset");
    check(hv_vm_unmap(0x1000,4*4096)==0 && hv_vm_unmap(0x8000,2*4096)==0,"unmap FP guest");
    free(p);check(hv_vcpu_destroy(c)==0 && hv_vm_destroy()==0,"FP cleanup");
}
int main() {setvbuf(stdout,nullptr,_IONBF,0);mode(16);mode(32);mode(64);return failures?1:0;}
