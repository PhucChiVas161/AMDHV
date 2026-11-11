#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned failures;
static void check(bool ok,const char *s) {printf("%s: %s\n",ok?"PASS":"FAIL",s);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t want) {uint64_t v=0;return !hv_vmx_vcpu_read_vmcs(c,f,&v)&&v==want;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t want) {uint64_t v=0;return !hv_vcpu_read_register(c,r,&v)&&v==want;}
static int run(uint64_t efer) {
    printf("Paging controls EFER=%llx\n",efer);
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT)) return 2;
    hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)) {hv_vm_destroy();return 2;}
    void *p=nullptr;if(posix_memalign(&p,4096,9*4096)) {hv_vcpu_destroy(c);hv_vm_destroy();return 2;}
    memset(p,0,9*4096);auto t=static_cast<uint64_t*>(p);auto b=static_cast<unsigned char*>(p);
    auto code=b+6*4096;auto dataA=b+7*4096;auto dataB=b+8*4096;
    // Two 2MiB global guest translations: same virtual code, different data backing.
    t[0]=0x2003;t[512]=0x3003;t[1024]=0x181; // supervisor RO, global, huge
    t[1536]=0x5003;t[2048]=0x6003;t[2560]=0x200183;
    uint64_t a=0x1111222233334444ull,bval=0xaabbccddeeff0099ull;
    memcpy(dataA,&a,8);memcpy(dataB,&bval,8);
    bool ok=!hv_vm_map(p,0x1000,6*4096,HV_MEMORY_READ|HV_MEMORY_WRITE) &&
        !hv_vm_map(code,0x8000,4096,HV_MEMORY_READ|HV_MEMORY_EXEC) &&
        !hv_vm_map(code,0x208000,4096,HV_MEMORY_READ|HV_MEMORY_EXEC) &&
        !hv_vm_map(dataA,0x9000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE) &&
        !hv_vm_map(dataB,0x209000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE);
    const struct {unsigned f;uint64_t v;} fields[]={{0x6800,0x80000011},{0x6804,0x20},{0x6802,0x1000},{0x2806,efer}};
    for(auto e:fields) ok &= !hv_vmx_vcpu_write_vmcs(c,e.f,e.v);
    for(unsigned i=0;i<6;++i) {
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff);
        ok &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xa09b:0xc093);
    }
    const unsigned char setup[]={0x0f,0x20,0xc0,0x48,0x0d,0,0,5,0,0x0f,0x22,0xc0,
        0x0f,0x20,0xe0,0x48,0x0d,0x90,0,0,0,0x0f,0x22,0xe0,
        0xbb,0,0x40,0,0,0x0f,0x22,0xdb,0x0f,0x20,0xda,
        0x48,0xbb,0,0x90,0,0,0,0,0,0,0x48,0x8b,0x03,0x0f,0xa2,0xf4};
    memcpy(code,setup,sizeof(setup));hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    check(ok,"configure two long-mode page-table roots");
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10) &&
        field(c,VMCS_GUEST_CR0,0x80050011) && field(c,VMCS_GUEST_CR4,0xb0) &&
        field(c,VMCS_GUEST_CR3,0x4000) && reg(c,HV_X86_RDX,0x4000) && reg(c,HV_X86_RAX,bval),
        "guest updates WP/AM/PSE/PGE and CR3, executes through new root and reads its data");
    const unsigned char back[]={0xb8,0,0x10,0,0,0x0f,0x22,0xd8,
        0x48,0xbb,0,0x90,0,0,0,0,0,0,0x48,0x8b,0x03,0x0f,0xa2};
    memcpy(code+0x80,back,sizeof(back));hv_vcpu_write_register(c,HV_X86_RIP,0x8080);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RAX,a),
        "switch back flushes global guest translations and observes original data");
    const unsigned char write[]={0x48,0xbb,0,0x90,0,0,0,0,0,0,0xc6,0x03,0x5a,0x0f,0xa2};
    memcpy(code+0x100,write,sizeof(write));hv_vcpu_write_register(c,HV_X86_RIP,0x8100);
    hv_vmx_vcpu_write_vmcs(c,VMCS_GUEST_CR0,0x80000011);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10) && dataA[0]==0x5a,
        "WP=0 permits supervisor write to guest read-only page");
    dataA[0]=0xa5;hv_vcpu_write_register(c,HV_X86_RIP,0x8100);
    hv_vmx_vcpu_write_vmcs(c,VMCS_GUEST_CR0,0x80010011);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED && reg(c,HV_X86_RIP,0x810a) && dataA[0]==0xa5,
        "WP=1 faults before guest read-only write; exception injection still unsupported");
    check(!hv_vm_unmap(0x1000,6*4096) && !hv_vm_unmap(0x8000,4096) && !hv_vm_unmap(0x208000,4096) &&
        !hv_vm_unmap(0x9000,4096) && !hv_vm_unmap(0x209000,4096),"unmap paging-control guest");
    free(p);check(!hv_vcpu_destroy(c)&&!hv_vm_destroy(),"paging-control cleanup");return failures?1:0;
}

int main() { int a=run(0x500), b=run(0xd00); return a || b || failures ? 1 : 0; }
