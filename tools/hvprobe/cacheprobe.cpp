#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <sys/sysctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
static unsigned failures;
static void check(bool ok,const char *s){printf("%s: %s\n",ok?"PASS":"FAIL",s);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected){uint64_t v=~0ull;auto r=hv_vmx_vcpu_read_vmcs(c,f,&v);if(r||v!=expected)printf("VMCS%x=%llx expected=%llx r=%x\n",f,v,expected,r);return !r&&v==expected;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t expected){uint64_t v=~0ull;return !hv_vcpu_read_register(c,r,&v)&&v==expected;}
static void mode(unsigned bits){
    printf("CACHE mode%u\n",bits);
    if(hv_vm_create(HV_VM_DEFAULT)){check(false,"create cache VM");return;}
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){hv_vm_destroy();check(false,"create cache vCPU");return;}
    void *mem=nullptr;if(posix_memalign(&mem,4096,6*4096)){hv_vcpu_destroy(c);hv_vm_destroy();check(false,"allocate cache backing");return;}
    memset(mem,0,6*4096);auto t=static_cast<uint64_t*>(mem);auto code=static_cast<unsigned char*>(mem)+4*4096;
    t[0]=0x2003;t[512]=0x3003;t[1024]=0x4003;t[1536+8]=0x8003;t[1536+9]=0x9003;
    bool ok=!hv_vm_map(mem,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)&&!hv_vm_map(code,0x8000,2*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    hv_vmx_vcpu_write_vmcs(c,0x6800,bits==64?0x80010033:bits==32?0x33:0x30);
    hv_vmx_vcpu_write_vmcs(c,0x6804,bits==64?0x20:0);hv_vmx_vcpu_write_vmcs(c,0x2806,bits==64?0xd00:0);
    hv_vmx_vcpu_write_vmcs(c,0x6802,bits==64?0x1000:0);
    for(unsigned i=0;i<6;++i){ok &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,bits==16?0x800:i==1?8:16);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x6806+i*2,bits==16?0x8000:0);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,bits==16?0xffff:0xffffffff);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?(bits==64?0xa09b:bits==32?0xc09b:0x9b):bits==16?0x93:0xc093);}
    check(ok,"configure coherent guest RAM and cache-instruction mode");
    constexpr uint64_t marker=0x12345678;
    const uint64_t entry=bits==16?0:0x8000;
    auto prepare=[&](){hv_vcpu_write_register(c,HV_X86_RIP,entry);hv_vcpu_write_register(c,HV_X86_RSP,bits==16?0x1f00:0x9f00);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,0x246);hv_vcpu_write_register(c,HV_X86_RAX,marker);hv_vmx_vcpu_write_vmcs(c,0x4824,0);};
    auto run=[&](unsigned expected){for(unsigned i=0;i<256;++i){auto r=hv_vcpu_run(c);if(r){printf("run=%x\n",r);return false;}uint64_t reason=0;hv_vmx_vcpu_read_vmcs(c,0x4402,&reason);if(reason!=1)return reason==expected;}return false;};
    unsigned n=0;
    if(bits==16){const unsigned char store[]={0xc6,6,0,0x1e,0x5a};memcpy(code,store,sizeof(store));n=sizeof(store);}
    else {const unsigned char store[]={0xc6,5,0,0x9e,0,0,0x5a};memcpy(code,store,sizeof(store));n=sizeof(store);
        if(bits==64){int32_t displacement=0x9e00-(0x8000+n);memcpy(code+2,&displacement,4);}}
    for(unsigned i=0;i<64;++i){if(i&1)code[n++]=0xf3;code[n++]=0x0f;code[n++]=9;}unsigned stop=n;code[n++]=0x0f;code[n++]=0xa2;
    prepare();check(run(10)&&reg(c,HV_X86_RIP,entry+stop)&&reg(c,HV_X86_RAX,marker)&&reg(c,HV_X86_RFLAGS,0x246)&&code[0x1e00]==0x5a,
        "WBINVD/WBNOINVD stream completes through bounded quanta and retains coherent guest stores/flags/registers");
    for(bool prefix:{false,true}){
        n=0;if(prefix)code[n++]=0x66;code[n++]=0x0f;code[n++]=8;unsigned length=n;code[n++]=0x0f;code[n++]=0xa2;
        prepare();check(run(13)&&field(c,0x440c,length)&&reg(c,HV_X86_RIP,entry)&&reg(c,HV_X86_RAX,marker)&&code[0x1e00]==0x5a,
            "INVD returns mandatory VMX13 with exact prefixed length and no RIP/data side effect");
        hv_vcpu_write_register(c,HV_X86_RIP,entry+length);check(run(10),"userspace can advance INVD and resume the guest");
    }
    check(!hv_vcpu_destroy(c),"destroy cache vCPU");check(!hv_vm_unmap(0x1000,4*4096)&&!hv_vm_unmap(0x8000,2*4096),"unmap cache guest");
    free(mem);check(!hv_vm_destroy(),"cache guest cleanup");
}
int main(){setvbuf(stdout,nullptr,_IONBF,0);for(unsigned bits:{16u,32u,64u})mode(bits);return failures?1:0;}
