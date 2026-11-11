#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// EFI-shaped AP entry, including the indirect FF /5 compatibility far jump.
// The userspace loop models the VMM's EFER and masked CR0.PG handling.
static uint64_t field(hv_vcpuid_t c, unsigned f) {
    uint64_t v=0; if(hv_vmx_vcpu_read_vmcs(c,f,&v)) return ~0ull; return v;
}
static uint64_t reg(hv_vcpuid_t c,hv_x86_reg_t r) {
    uint64_t v=0; if(hv_vcpu_read_register(c,r,&v)) return ~0ull; return v;
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT))return 2;
    hv_vcpuid_t keeper;
    if(hv_vcpu_create(&keeper,HV_VCPU_DEFAULT)){hv_vm_destroy();return 2;}
    void *mem=nullptr;
    if(posix_memalign(&mem,4096,8*4096)){hv_vcpu_destroy(keeper);hv_vm_destroy();return 2;}
    memset(mem,0,8*4096);auto b=static_cast<unsigned char*>(mem);
    const unsigned char real[]={0x66,0x89,0xc5,0x8c,0xc8,0x8e,0xd8,0x8e,0xc0,0x8e,0xd0,0x31,0xc0,0x8e,0xe0,0x8e,0xe8,0xbe,0x0d,0x11,0x66,0x8b,0x1c,0xbe,0x2d,0x11,0x66,0x8b,0x14,0x66,0xbf,0x6d,0x11,0,0,0xbe,0xf9,0x10,0x2e,0x66,0x0f,0x01,0x14,0xbe,0x03,0x11,0x2e,0x66,0x0f,0x01,0x1c,0x0f,0x20,0xc0,0x66,0x83,0xc8,3,0x0f,0x22,0xc0,0x2e,0x66,0xff,0x2d};
    const unsigned char compat[]={
        0x8e,0xda,0x8e,0xc2,0x8e,0xe2,0x8e,0xea,0x8e,0xd2,
        0x0f,0x20,0xe0,0x0f,0xba,0xe8,5,0x0f,0x22,0xe0, // CR4.PAE
        0xbe,0x3d,0x11,0,0,0x8b,0x0c,0x33,0x0f,0x22,0xd9, // CR3 from exchange
        0xb9,0x80,0,0,0xc0,0x0f,0x32,0x0f,0xba,0xe8,8,0x0f,0x30, // EFER.LME
        0x0f,0x20,0xc0,0x0f,0xba,0xe8,31,0x0f,0x22,0xc0, // CR0.PG
        0xbf,0x73,0x11,0,0,0x03,0xfb,0xff,0x2f}; // JMP FAR [EDI]
    const unsigned char code64[]={0x48,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x0f,0xa2};
    memcpy(b,real,sizeof(real));memcpy(b+0x2000,compat,sizeof(compat));memcpy(b+0x3000,code64,sizeof(code64));
    const uint64_t gdt[]={0,0,0x00cf9b000000ffffull,0x00cf93000000ffffull,0,0,0,0x00af9b000000ffffull,0};
    memcpy(b+0x1800,gdt,sizeof(gdt));
    auto w16=[&](unsigned o,uint16_t v){memcpy(b+o,&v,2);};
    auto w32=[&](unsigned o,uint32_t v){memcpy(b+o,&v,4);};
    w16(0x10f9,sizeof(gdt)-1);w32(0x10fb,0x87800);
    w32(0x110d,0x86000);w32(0x112d,0x18);w32(0x113d,0x8a000);
    w32(0x116d,0x88000);w16(0x1171,0x10);w32(0x1173,0x89000);w16(0x1177,0x38);
    auto pt=reinterpret_cast<uint64_t*>(b+0x4000);
    pt[0]=0x8b003;pt[512]=0x8c003;pt[1024]=0x8d003;
    for(unsigned i=0x86;i<=0x8d;++i)pt[1536+i]=(uint64_t(i)<<12)|3;
    if(hv_vm_map(mem,0x86000,8*4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC)){
        free(mem);hv_vcpu_destroy(keeper);hv_vm_destroy();return 2;
    }
    std::atomic<unsigned> ready{0},failed{0};std::vector<std::thread> workers;
    for(unsigned n=0;n<3;++n)workers.emplace_back([&,n]{
        hv_vcpuid_t c=~0u;bool created=!hv_vcpu_create(&c,HV_VCPU_TSC_RELATIVE);
        ++ready;while(ready.load()!=3)std::this_thread::yield();
        bool ok=created && c>=1 && c<=3;
        unsigned passed=0;
        for(unsigned trial=0;ok && trial<64;++trial){
            auto set=[&](unsigned f,uint64_t v){ok &= !hv_vmx_vcpu_write_vmcs(c,f,v);};
            set(0x6800,0x30);set(0x6802,0);set(0x6804,0x2000);set(0x2806,trial&1?0x800:0);
            set(0x6000,0xe0000020);set(0x6004,0x40000010);set(0x6002,0x2000);set(0x6006,0);
            set(0x4002,0x200080);set(0x4012,0);set(0x4016,0);set(0x4824,0);
            for(unsigned i=0;i<6;++i){set(0x800+i*2,0x8600);set(0x6806+i*2,0x86000);set(0x4800+i*2,0xffff);set(0x4814+i*2,i==1?0x9b:0x93);}
            ok &= !hv_vcpu_write_register(c,HV_X86_RIP,0);
            ok &= !hv_vcpu_write_register(c,HV_X86_RSP,0x800);
            ok &= !hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
            bool done=false;
            for(unsigned step=0;ok && step<1024;++step){
                auto r=hv_vcpu_run(c);auto reason=field(c,0x4402);auto rip=reg(c,HV_X86_RIP);
                if(r){printf("AP%u trial%u run=%x RIP=%llx\n",c,trial,r,rip);ok=false;break;}
                if(reason==1)continue;
                if((reason==31 || reason==32) && reg(c,HV_X86_RCX)==0xc0000080){
                    if(reason==31){auto efer=field(c,0x2806);ok &= !hv_vcpu_write_register(c,HV_X86_RAX,uint32_t(efer));ok &= !hv_vcpu_write_register(c,HV_X86_RDX,efer>>32);}
                    else set(0x2806,(reg(c,HV_X86_RDX)<<32)|uint32_t(reg(c,HV_X86_RAX))|(trial&1));
                    ok &= !hv_vcpu_write_register(c,HV_X86_RIP,rip+2);continue;
                }
                if(reason==28 && field(c,0x6400)==0 && (reg(c,HV_X86_RAX)&0x80000000)){
                    auto value=reg(c,HV_X86_RAX);set(0x6800,(value&~0xe0000000ull)|0x80000000);
                    set(0x6004,value);set(0x2806,field(c,0x2806)|0x400);set(0x4012,0x200);
                    ok &= !hv_vcpu_write_register(c,HV_X86_RIP,rip+3);continue;
                }
                done=reason==10 && rip==0x8900a && reg(c,HV_X86_RAX)==0x1122334455667788ull &&
                    field(c,0x802)==0x38 && field(c,0x6802)==0x8a000 &&
                    field(c,0x2806)==(trial&1?0xd01:0x500) && (field(c,0x6800)&0x80000001)==0x80000001 &&
                    (field(c,0x6804)&0x20);
                if(!done)printf("AP%u trial%u unexpected reason=%llu RIP=%llx EFER=%llx CR0=%llx CR3=%llx qual=%llx\n",c,trial,reason,rip,field(c,0x2806),field(c,0x6800),field(c,0x6802),field(c,0x6400));
                break;
            }
            ok &= done;if(ok)++passed;
        }
        printf("%s: AP slot%u/thread%u real->PE->PAE/LME/PG->indirect far64: %u/64 (alternating NXE/SCE)\n",ok?"PASS":"FAIL",c,n,passed);
        if(created)ok &= !hv_vcpu_destroy(c);if(!ok)++failed;
    });
    for(auto &w:workers)w.join();
    bool clean=!hv_vm_unmap(0x86000,8*4096);
    clean &= !hv_vcpu_destroy(keeper);clean &= !hv_vm_destroy();free(mem);
    printf("%s: AP long transition cleanup\n",clean?"PASS":"FAIL");
    return failed.load() || !clean ? 1:0;
}
