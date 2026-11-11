#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// Each guest sets a flag then waits for the other guest, without PAUSE exits.
// Both FIRST run calls reaching CPUID proves overlapping guest execution:
// a serialized runner must return a host-interrupt exit from at least one call.
int main(int argc,char **argv){
    bool baseline=argc==2&&!strcmp(argv[1],"--baseline");
    if(argc!=1&&!baseline)return 2;
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT))return 2;
    void *mem=nullptr;
    if(posix_memalign(&mem,4096,8192)){hv_vm_destroy();return 2;}
    memset(mem,0,8192);auto b=static_cast<unsigned char*>(mem);
    for(unsigned n=0;n<2;++n){
        unsigned char code[]={0xc7,5,0,0,0,0,1,0,0,0,0x83,0x3d,0,0,0,0,1,0x75,0xf7,0xb8,0x78,0x56,0x34,0x12,0x0f,0xa2};
        uint32_t own=0x9000+n*4,other=0x9000+(1-n)*4;
        memcpy(code+2,&own,4);memcpy(code+12,&other,4);memcpy(b+n*0x100,code,sizeof(code));
    }
    b[0x200]=0xeb;b[0x201]=0xfe;
    if(hv_vm_map(mem,0x8000,8192,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC)){
        free(mem);hv_vm_destroy();return 2;
    }
    std::atomic<unsigned> ready{0},generation{0},done{0},errors{0};
    std::atomic<bool> stop{false};std::atomic<unsigned> loopExits[2]{};
    bool reached[2]={};std::thread threads[2];
    for(unsigned n=0;n<2;++n)threads[n]=std::thread([&,n]{
        hv_vcpuid_t c=~0u;bool created=!hv_vcpu_create(&c,HV_VCPU_DEFAULT);bool configured=created;
        auto set=[&](unsigned f,uint64_t v){if(created)configured &= !hv_vmx_vcpu_write_vmcs(c,f,v);};
        set(0x6800,0x33);set(0x6804,0);set(0x2806,0);
        for(unsigned i=0;i<6;++i){set(0x800+i*2,i==1?8:16);set(0x6806+i*2,0);set(0x4800+i*2,0xffffffff);set(0x4814+i*2,i==1?0xc09b:0xc093);}
        ++ready;
        for(unsigned trial=0;trial<64;++trial){
            while(generation.load(std::memory_order_acquire)<trial+1)std::this_thread::yield();
            bool ok=configured;
            if(ok){ok &= !hv_vcpu_write_register(c,HV_X86_RIP,0x8000+n*0x100);
                ok &= !hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
                ok &= !hv_vcpu_run(c);
                uint64_t reason=0,rip=0,ax=0;
                ok &= !hv_vmx_vcpu_read_vmcs(c,VMCS_RO_EXIT_REASON,&reason);
                ok &= !hv_vcpu_read_register(c,HV_X86_RIP,&rip);
                ok &= !hv_vcpu_read_register(c,HV_X86_RAX,&ax);
                reached[n]=ok && reason==10 && rip==0x8018+n*0x100 && ax==0x12345678;
                if(ok && reason!=1 && reason!=10)ok=false;
            }
            if(!ok)++errors;
            done.fetch_add(1,std::memory_order_release);
        }
        while(generation.load(std::memory_order_acquire)<65)std::this_thread::yield();
        if(configured){
            if(hv_vcpu_write_register(c,HV_X86_RIP,0x8200))++errors;
            while(!stop.load()){
                uint64_t reason=0;
                if(hv_vcpu_run(c) || hv_vmx_vcpu_read_vmcs(c,VMCS_RO_EXIT_REASON,&reason) || reason!=1)++errors;
                ++loopExits[n];
            }
        }else ++loopExits[n];
        if(created && hv_vcpu_destroy(c))++errors;
    });
    while(ready.load()!=2)std::this_thread::yield();
    unsigned overlap=0;
    for(unsigned trial=0;trial<64;++trial){
        auto flags=reinterpret_cast<uint32_t*>(b+4096);
        __atomic_store_n(flags,0,__ATOMIC_RELEASE);__atomic_store_n(flags+1,0,__ATOMIC_RELEASE);
        reached[0]=reached[1]=false;done.store(0);
        generation.store(trial+1,std::memory_order_release);
        while(done.load(std::memory_order_acquire)!=2)std::this_thread::yield();
        if(reached[0]&&reached[1])++overlap;
    }
    generation.store(65,std::memory_order_release);
    while(!loopExits[0].load() || !loopExits[1].load())std::this_thread::yield();
    // Mutate an unused data page while both code loops keep running. Every
    // writer must drain readers, preserve code pins and let readers resume.
    bool mutations=true;
    for(unsigned i=0;i<8;++i){
        mutations &= !hv_vm_protect(0x9000,4096,HV_MEMORY_READ);
        mutations &= !hv_vm_unmap(0x9000,4096);
        mutations &= !hv_vm_map(b+4096,0x9000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE);
    }
    stop=true;
    for(auto &t:threads)t.join();
    bool clean=!hv_vm_unmap(0x8000,8192);clean &= !hv_vm_destroy();free(mem);
    printf("SMP first-call guest handshake overlap=%u/64 errors=%u\n",overlap,errors.load());
    printf("%s: mapping writer progress with two active guests; normal thread teardown and pin cleanup\n",mutations&&clean&&!errors.load()?"PASS":"FAIL");
    bool ok=mutations&&clean&&!errors.load()&&(baseline?overlap==0:overlap>0);
    printf("%s: %s; both guest flags must be observed within the first run calls\n",ok?"PASS":"FAIL",baseline?"serialized baseline":"parallel guest execution");
    return ok?0:1;
}
