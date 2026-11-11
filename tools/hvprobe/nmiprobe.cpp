#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <sys/sysctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned failures;
static void check(bool ok,const char *label){printf("%s: %s\n",ok?"PASS":"FAIL",label);failures+=!ok;}
static bool field(hv_vcpuid_t c,unsigned f,uint64_t expected){uint64_t got=~0ull;auto r=hv_vmx_vcpu_read_vmcs(c,f,&got);
 if(r||got!=expected)printf("VMCS%x=%llx expected=%llx r=%x\n",f,got,expected,r);return !r&&got==expected;}
static bool reg(hv_vcpuid_t c,hv_x86_reg_t r,uint64_t expected){uint64_t got=~0ull;return !hv_vcpu_read_register(c,r,&got)&&got==expected;}
int main(){
 setvbuf(stdout,nullptr,_IONBF,0);int fast=0;size_t size=sizeof(fast);
 if(sysctlbyname("debug.amdhv.fast",&fast,&size,nullptr,0)||fast!=1){puts("Requires fast mode and physical vNMI");return 2;}
 if(hv_vm_create(HV_VM_DEFAULT))return 2;
 hv_vcpuid_t c;if(hv_vcpu_create(&c,HV_VCPU_DEFAULT)){hv_vm_destroy();return 2;}
 void *mem=nullptr;if(posix_memalign(&mem,4096,5*4096)){hv_vcpu_destroy(c);hv_vm_destroy();return 2;}
 memset(mem,0,5*4096);auto t=static_cast<uint64_t*>(mem);auto b=static_cast<unsigned char*>(mem)+4*4096;
 t[0]=0x2003;t[512]=0x3003;t[1024]=0x4003;t[1536+8]=0x8003;
 bool ok=!hv_vm_map(mem,0x1000,4*4096,HV_MEMORY_READ|HV_MEMORY_WRITE)&&!hv_vm_map(b,0x8000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
 uint64_t gdt[]={0,0x00af9b000000ffffull,0x00cf93000000ffffull};memcpy(b+0x200,gdt,sizeof(gdt));
 const unsigned char mainCode[]={0xfa,0xb8,0x78,0x56,0x34,0x12,0x0f,0xa2,0xf4};memcpy(b,mainCode,sizeof(mainCode));
 const unsigned char nmiCode[]={0xff,0x05,0xfa,0x01,0,0,0xb8,0x11,0x22,0x33,0x44,0x0f,0xa2,0x48,0xcf};memcpy(b+0x100,nmiCode,sizeof(nmiCode));
 const unsigned char irqCode[]={0xb8,0x55,0x66,0x77,0x08,0x0f,0xa2,0x48,0xcf};memcpy(b+0x180,irqCode,sizeof(irqCode));
 auto gate=[&](unsigned v,unsigned target){auto p=b+0x400+16*v;p[0]=target;p[1]=target>>8;p[2]=8;p[5]=0x8e;};gate(2,0x8100);gate(64,0x8180);
 const struct{unsigned f;uint64_t v;} fields[]={{0x6800,0x80010033},{0x6802,0x1000},{0x6804,0x20},{0x2806,0xd00},
  {0x6816,0x8200},{0x4810,sizeof(gdt)-1},{0x6818,0x8400},{0x4812,0x7ff},{0x681e,0x8000},{0x681c,0x8f00},{0x6820,2}};
 for(auto f:fields)ok &= !hv_vmx_vcpu_write_vmcs(c,f.f,f.v);
 for(unsigned i=0;i<6;++i){ok &= !hv_vmx_vcpu_write_vmcs(c,0x800+i*2,i==1?8:16);
  ok &= !hv_vmx_vcpu_write_vmcs(c,0x4800+i*2,0xffffffff);ok &= !hv_vmx_vcpu_write_vmcs(c,0x4814+i*2,i==1?0xa09b:0xc093);}
 check(ok,"configure native NMI/IRQ IDT, private counter, stack and paging");
 auto run=[&](){for(unsigned i=0;i<256;++i){auto r=hv_vcpu_run(c);if(r){printf("run=%x\n",r);return false;}uint64_t reason=0;hv_vmx_vcpu_read_vmcs(c,0x4402,&reason);if(reason!=1)return reason==10;}return false;};
 auto count=[&](){uint32_t value=0;memcpy(&value,b+0x300,4);return value;};
 check(run()&&reg(c,HV_X86_RIP,0x8006)&&reg(c,HV_X86_RAX,0x12345678),"IF-clear guest reaches initial CPUID");
 hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000202);
 bool delivered=run();
 check(delivered&&reg(c,HV_X86_RIP,0x810b)&&reg(c,HV_X86_RAX,0x44332211)&&count()==1&&field(c,0x4824,8)&&field(c,0x4016,0),
  "NMI enters guest vector2 with IF=0, retires entry once and publishes NMI blocking");
 if(delivered){
  hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000202);
  check(hv_vcpu_run(c)!=HV_SUCCESS&&reg(c,HV_X86_RIP,0x810b)&&count()==1,"blocked nested NMI is rejected before guest mutation");
  hv_vmx_vcpu_write_vmcs(c,0x4016,0);hv_vcpu_write_register(c,HV_X86_RIP,0x810d);
  check(run()&&reg(c,HV_X86_RIP,0x8006)&&reg(c,HV_X86_RSP,0x8f00)&&field(c,0x4824,0)&&reg(c,HV_X86_RFLAGS,2),
   "native IRETQ unblocks NMI and restores interrupted IF-clear guest/stack");
  hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000202);
  check(run()&&count()==2&&field(c,0x4824,8),"a second NMI can be delivered after IRETQ");
  hv_vcpu_write_register(c,HV_X86_RFLAGS,0x202);hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000040);
  check(run()&&reg(c,HV_X86_RIP,0x8185)&&reg(c,HV_X86_RAX,0x08776655)&&field(c,0x4824,8),
   "NMI blocking does not block an IF-enabled external IRQ");
  hv_vcpu_write_register(c,HV_X86_RIP,0x8187);
  check(run()&&reg(c,HV_X86_RIP,0x810b)&&field(c,0x4824,0),"IRETQ from nested IRQ also clears architectural NMI blocking");
  hv_vcpu_write_register(c,HV_X86_RIP,0x810d);
  check(run()&&reg(c,HV_X86_RIP,0x8006)&&reg(c,HV_X86_RFLAGS,2)&&count()==2,"NMI return remains balanced after nested IRQ");
 }
 check(!hv_vcpu_destroy(c),"destroy NMI vCPU");check(!hv_vm_unmap(0x1000,4*4096)&&!hv_vm_unmap(0x8000,4096),"unmap NMI guest");
 free(mem);check(!hv_vm_destroy(),"NMI guest cleanup");return failures?1:0;
}
