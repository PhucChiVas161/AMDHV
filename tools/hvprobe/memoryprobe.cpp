#include <Hypervisor/hv.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdint>
static unsigned failures;
static void check(bool ok,const char *s){printf("%s: %s\n",ok?"PASS":"FAIL",s);failures+=!ok;}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(hv_vm_create(HV_VM_DEFAULT)){check(false,"create large-memory VM");return 2;}
    constexpr uint64_t bytes=(4ull<<30)+4096,gpa=0x100000000ull;
    void *p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(p==MAP_FAILED){hv_vm_destroy();check(false,"reserve large guest backing");return 2;}
    auto b=static_cast<unsigned char*>(p);b[0]=0x12;b[bytes-1]=0x34;
    auto r=hv_vm_map(p,gpa,bytes,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    check(r==0,"pin/map more than4GiB across GPA4GiB..8GiB with a bounded NPT");
    if(!r){
        check(!hv_vm_protect(gpa+bytes-4096,4096,HV_MEMORY_READ),"protect a high GPA page beyond the old NPT capacity");
        check(!hv_vm_unmap(gpa+bytes-4096,4096),"partially unmap the large prepared descriptor");
        check(!hv_vm_unmap(gpa,bytes),"release all remaining pages and descriptor pin budget");
        check(!hv_vm_map(p,gpa,4096,HV_MEMORY_READ|HV_MEMORY_WRITE)&&!hv_vm_unmap(gpa,4096),"map again after large descriptor release");
        check(b[0]==0x12&&b[bytes-1]==0x34,"mapping/protect/unmap preserve the host backing bytes");
    }else printf("large map result=%x\n",r);
    check(!hv_vm_destroy(),"large-memory VM cleanup");
    check(!munmap(p,bytes),"release large host reservation");return failures?1:0;
}
