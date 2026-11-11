#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <sys/sysctl.h>
#include <cpuid.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
extern "C" hv_return_t fpu_run_preserved(void *,void *,void *,void *);
static unsigned failures;
static void check(bool ok,const char *label){printf("%s: %s\n",ok?"PASS":"FAIL",label);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t v){uint64_t got=~0ull;auto r=hv_vmx_vcpu_read_vmcs(c,f,&got);
    if(r||got!=v)printf("VMCS%x=%llx expected=%llx r=%x\n",f,got,v,r);return !r&&got==v;}
static uint64_t xcr(){unsigned a,d;asm volatile("xgetbv":"=a"(a),"=d"(d):"c"(0));return (uint64_t(d)<<32)|a;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t regId,uint64_t expected){uint64_t value=~0ull;
    return !hv_vcpu_read_register(c,regId,&value)&&value==expected;}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);int fast=0;size_t n=sizeof(fast);
    if(sysctlbyname("debug.amdhv.fast",&fast,&n,nullptr,0)||fast!=1||xcr()!=7){puts("Requires fast mode and host XCR0=7");return 2;}
    if(hv_vm_create(HV_VM_DEFAULT))return 2;
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){hv_vm_destroy();return 2;}
    void *mem=nullptr;if(posix_memalign(&mem,4096,6*4096)){hv_vcpu_destroy(c);hv_vm_destroy();return 2;}
    memset(mem,0,6*4096);auto t=static_cast<uint64_t*>(mem);auto code=static_cast<unsigned char*>(mem)+4*4096;
    t[0]=0x2003;t[512]=0x3003;t[1024]=0x4003;t[1536+8]=0x8003;t[1536+9]=0x9003;
    bool mapped=!hv_vm_map(mem,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)&&
        !hv_vm_map(code,0x8000,2*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    auto configure=[&](){bool ok=true;
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x6800,0x80010033);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x6802,0x1000);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x6804,0x620);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x2806,0xd00);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4004,0xffffffff);
        for(unsigned i=0;i<6;++i){ok &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16);
            ok &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff);
            ok &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xa09b:0xc093);}
        return ok;};
    check(mapped&&configure(),"configure isolated XSAVE/AVX guest");
    alignas(64) unsigned char fp[512]={},scratch[8192]={},before[832]={},after[832]={};
    bool hostPreserved=true;
    auto run=[&]() -> hv_return_t {for(unsigned i=0;i<256;++i){
        auto r=fpu_run_preserved(fp,scratch,before,after);
        bool host=memcmp(before,after,5)==0&&memcmp(before+6,after+6,26)==0&&
            memcmp(before+160,after+160,256)==0&&memcmp(before+512,after+512,8)==0&&
            memcmp(before+576,after+576,256)==0&&xcr()==7;
        for(unsigned j=0;j<8;++j)host &= memcmp(before+32+j*16,after+32+j*16,10)==0;
        hostPreserved &= host;if(r)return r;
        uint64_t reason=0;if(hv_vmx_vcpu_read_vmcs(c,0x4402,&reason))return HV_ERROR;
        if(reason!=1)return HV_SUCCESS;}return HV_ERROR;};
    auto prepare=[&](std::initializer_list<unsigned char> bytes){unsigned i=0;for(auto v:bytes)code[i++]=v;
        hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RFLAGS,2);};
    // Enable OSXSAVE, XCR0=7, mutate all 256 bits of YMM15; CPUID separates runs.
    prepare({0xb8,0x20,6,4,0,0x0f,0x22,0xe0,0x31,0xc9,0x31,0xd2,0xb8,7,0,0,0,0x0f,0x01,0xd1,
        0x0f,0x01,0xd0,0xc4,0x41,0x05,0x74,0xff,0x0f,0xa2,0xf4});
    check(run()==0&&field(c,0x4402,10)&&field(c,0x6804,0x40620)&&reg(c,HV_X86_RAX,7),"guest enables OSXSAVE/XSETBV and executes AVX before CPUID");
    uint64_t guestXcr=0;
    check(!hv_vcpu_read_register(c,static_cast<hv_x86_reg_t>(50),&guestXcr)&&guestXcr==7,
        "guest XCR0 is published independently of the host register");
    // Read and rewrite the legacy API without exposing or losing YMM upper halves.
    bool legacy=!hv_vcpu_read_fpstate(c,fp,512)&&!hv_vcpu_write_fpstate(c,fp,512);
    prepare({0xbf,0,0x90,0,0,0xc5,0x7e,0x7f,0x3f,0xf4}); // vmovdqu ymm15,[rdi]
    auto r=run();bool ones=true;for(unsigned i=0;i<32;++i)ones &= code[0x1000+i]==255;
    check(legacy&&r==0&&field(c,0x4402,12)&&ones,"YMM15 survives separate runs and a 512-byte legacy FP API rewrite");
    // Guest-native XSAVE/XRSTOR to guest RAM must also retain the upper halves.
    prepare({0xbf,0x40,0x90,0,0,0xb8,7,0,0,0,0x31,0xd2,0x48,0x0f,0xae,0x27,
        0xc4,0x41,0x05,0xef,0xff,0xb8,7,0,0,0,0x48,0x0f,0xae,0x2f,
        0xbf,0,0x90,0,0,0xc5,0x7e,0x7f,0x3f,0xf4});
    memset(code+0x1000,0,32);r=run();ones=true;for(unsigned i=0;i<32;++i)ones &= code[0x1000+i]==255;
    check(r==0&&field(c,0x4402,12)&&ones,"native guest XSAVE64/XRSTOR64 round-trip AVX through guest-owned RAM");
    unsigned a,b,cc,d;
    if(__get_cpuid_count(0xd,1,&a,&b,&cc,&d) && (a&8)) {
        memset(code+0x1040,0,832);
        prepare({0xbf,0x40,0x90,0,0,0xb8,7,0,0,0,0x31,0xd2,0x48,0x0f,0xc7,0x2f,
            0xc4,0x41,0x05,0xef,0xff,0xb8,7,0,0,0,0x48,0x0f,0xc7,0x1f,
            0xbf,0,0x90,0,0,0xc5,0x7e,0x7f,0x3f,0xf4});
        memset(code+0x1000,0,32);r=run();ones=true;for(unsigned i=0;i<32;++i)ones &= code[0x1000+i]==255;
        check(r==0&&field(c,0x4402,12)&&ones,"guest XSAVES64/XRSTORS64 compacted state round-trip with host XSS=0");
    }
    // Disabling and re-enabling SSE/AVX must not expose host vector contents.
    prepare({0x31,0xc9,0x31,0xd2,0xb8,1,0,0,0,0x0f,0x01,0xd1,0x0f,0x01,0xd0,0x0f,0xa2,0xf4});
    check(run()==0&&field(c,0x4402,10)&&reg(c,HV_X86_RAX,1),"guest XCR0=1/XGETBV reaches an exit with host XCR0 restored");
    hv_vcpu_read_register(c,static_cast<hv_x86_reg_t>(50),&guestXcr);check(guestXcr==1,"XCR0=1 survives an API boundary");
    prepare({0x31,0xc9,0x31,0xd2,0xb8,7,0,0,0,0x0f,0x01,0xd1,
        0xbf,0,0x90,0,0,0xc5,0x7e,0x7f,0x3f,0xf4});
    memset(code+0x1000,0,32);r=run();ones=true;for(unsigned i=0;i<32;++i)ones &= code[0x1000+i]==255;
    check(r==0&&field(c,0x4402,12)&&ones,"disabled guest components remain private across XCR0 changes");
    __get_cpuid_count(0xd,0,&a,&b,&cc,&d);unsigned physical=a;
    bool pku=physical&0x200;
    if((physical&0xe0)==0xe0) {
        prepare({0x31,0xc9,0x31,0xd2,0xb8,0xe7,2,0,0,0x0f,0x01,0xd1,
            0x62,0xf3,0x7d,0x48,0x25,0xc0,0xff,0x62,3,5,0x40,0x25,0xff,0xff,
            0xb8,0x5a,0x5a,0,0,0xc5,0xf8,0x92,0xf8,0x0f,0xa2,0xf4});
        if(!pku)code[6]=0;
        check(run()==0&&field(c,0x4402,10),"guest enables the physical extended mask and mutates ZMM0/ZMM31/opmask K7");
        bool legacyAvx512=!hv_vcpu_read_fpstate(c,fp,512)&&!hv_vcpu_write_fpstate(c,fp,512);
        prepare({0xbf,0,0x90,0,0,0x62,0xf1,0xfe,0x48,0x7f,7,
            0x62,0x61,0xfe,0x48,0x7f,0x7f,1,0xc5,0xf8,0x93,0xdf,0xf4});
        memset(code+0x1000,0,128);r=run();bool full=true;for(unsigned i=0;i<128;++i)full &= code[0x1000+i]==255;
        check(legacyAvx512&&r==0&&field(c,0x4402,12)&&full&&reg(c,HV_X86_RBX,0x5a5a),
            "all ZMM0/ZMM31 lanes and K7 survive re-entry plus a legacy FP API rewrite");
        memset(code+0x1040,0,4032);
        prepare({0xbf,0x40,0x90,0,0,0xb8,0xe7,2,0,0,0x31,0xd2,0x48,0x0f,0xae,0x27,
            0x62,0xf1,0x7d,0x48,0xef,0xc0,0x62,1,5,0x40,0xef,0xff,0xc5,0xc4,0x47,0xff,
            0xb8,0xe7,2,0,0,0x48,0x0f,0xae,0x2f,
            0xbf,0,0x90,0,0,0x62,0xf1,0xfe,0x48,0x7f,7,
            0x62,0x61,0xfe,0x48,0x7f,0x7f,1,0xc5,0xf8,0x93,0xdf,0xf4});
        if(!pku){code[7]=0;code[34]=0;} // both EAX mask immediates
        r=run();full=true;for(unsigned i=0;i<128;++i)full &= code[0x1000+i]==255;
        check(r==0&&field(c,0x4402,12)&&full&&reg(c,HV_X86_RBX,0x5a5a),
            "native guest XSAVE/XRSTOR restores AVX-512/opmask with the CPUID-sized extended layout");
    }
    if(pku) {
        hv_vmx_vcpu_write_vmcs(c,0x6804,0x440620);
        prepare({0x31,0xc9,0x31,0xd2,0xb8,7,2,0,0,0x0f,0x01,0xd1,
            0xb8,4,0,0,0,0x0f,0x01,0xef,0x0f,0x01,0xee,0xf4});
        check(run()==0&&field(c,0x4402,12)&&reg(c,HV_X86_RAX,4),"guest PKE/WRPKRU/RDPKRU use guest key rights with extended XCR0");
        bool legacyPku=!hv_vcpu_read_fpstate(c,fp,512)&&!hv_vcpu_write_fpstate(c,fp,512);
        hv_vcpu_write_register(c,static_cast<hv_x86_reg_t>(50),7);
        prepare({0x31,0xc9,0x0f,0x01,0xee,0xf4});
        check(legacyPku&&run()==0&&field(c,0x4402,12)&&reg(c,HV_X86_RAX,4),
            "PKRU survives re-entry, a legacy FP rewrite and clearing its guest XCR0 bit");
        hv_vmx_vcpu_write_vmcs(c,0x6804,0x400620);prepare({0x31,0xc9,0x0f,0x01,0xee,0xf4});
        check(run()==0&&field(c,0x4402,12)&&reg(c,HV_X86_RAX,4),"PKRU remains guest-owned when guest OSXSAVE is disabled");
        hv_vmx_vcpu_write_vmcs(c,0x6804,0x440620);
        // User page with key1, while guest code stays on a supervisor PTE.
        t[0]=0x2007;t[512]=0x3007;t[1024]=0x4007;t[1536+9]=(1ull<<59)|0x9007;
        prepare({0xb8,0,0x90,0,0,0x48,0x8b,0x18,0xf4});
        check(run()==0&&field(c,0x4402,0)&&field(c,0x4406,0x21)&&field(c,0x6400,0x9000),
            "PKRU access-disable faults a CPL0 read of a user key1 page with guest PF.PK");
        prepare({0x31,0xc9,0x31,0xd2,0xb8,8,0,0,0,0x0f,0x01,0xef,
            0xb8,0,0x90,0,0,0x48,0x89,0x18,0xf4});
        check(run()==0&&field(c,0x4402,0)&&field(c,0x4406,0x23)&&field(c,0x6400,0x9000),
            "PKRU write-disable faults a CPL0 write while CR0.WP is set");
        prepare({0x31,0xc9,0x31,0xd2,0x31,0xc0,0x0f,0x01,0xef,
            0xb8,0,0x90,0,0,0x48,0x8b,0x18,0xf4});
        check(run()==0&&field(c,0x4402,12),"clearing guest PKRU restores user-key data access");
        t[0]=0x2003;t[512]=0x3003;t[1024]=0x4003;t[1536+9]=0x9003;
        // Leave nonzero rights in the old CPU to check slot-reset isolation.
        prepare({0x31,0xc9,0x31,0xd2,0xb8,4,0,0,0,0x0f,0x01,0xef,0xf4});
        check(run()==0&&field(c,0x4402,12),"retain a nonzero guest PKRU for the reset-isolation test");
    }
    for(uint64_t bad : {0ull,2ull,5ull,0x27ull,0x87ull,1ull<<32}){
        hv_vcpu_write_register(c,static_cast<hv_x86_reg_t>(50),bad);prepare({0xf4});
        check(run()==HV_UNSUPPORTED&&field(c,0x681e,0x8000),"unsupported API XCR0 is rejected before entry");
    }
    hv_vcpu_write_register(c,static_cast<hv_x86_reg_t>(50),7);
    prepare({0x31,0xc9,0x31,0xd2,0xb8,5,0,0,0,0x0f,0x01,0xd1,0xf4});
    check(run()==HV_UNSUPPORTED&&field(c,0x681e,0x8009),"invalid guest XSETBV stops before committing the instruction");
    prepare({0xb9,1,0,0,0,0x31,0xd2,0xb8,7,0,0,0,0x0f,0x01,0xd1,0xf4});
    check(run()==HV_UNSUPPORTED&&field(c,0x681e,0x800c),"nonzero guest XSETBV index is rejected without advancing RIP");
    check(!hv_vcpu_destroy(c)&&!hv_vcpu_create(&c,HV_VCPU_DEFAULT)&&configure(),"recreate vCPU in a reused slot");
    hv_vmx_vcpu_write_vmcs(c,0x6804,0x40620);hv_vcpu_write_register(c,static_cast<hv_x86_reg_t>(50),7);
    prepare({0xbf,0,0x90,0,0,0xc5,0x7e,0x7f,0x3f,0xf4});memset(code+0x1000,255,32);
    r=run();bool zero=true;for(unsigned i=0;i<32;++i)zero &= !code[0x1000+i];
    check(r==0&&field(c,0x4402,12)&&zero,"new vCPU cannot inherit the previous guest YMM state");
    if(pku) {
        hv_vmx_vcpu_write_vmcs(c,0x6804,0x440620);prepare({0x31,0xc9,0x0f,0x01,0xee,0xf4});
        check(run()==0&&field(c,0x4402,12)&&reg(c,HV_X86_RAX,0),"new vCPU initializes PKRU without inheriting the old guest rights");
    }
    check(hostPreserved&&xcr()==7,"every success/rejection restores host XCR0, x87, XMM, YMM and init flags");
    check(!hv_vm_unmap(0x1000,4*4096)&&!hv_vm_unmap(0x8000,2*4096),"unmap XSAVE guest");
    free(mem);check(!hv_vcpu_destroy(c)&&!hv_vm_destroy(),"XSAVE guest cleanup");return failures?1:0;
}
