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
    printf("FLAGS mode%u\n",bits);
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
    check(ok,"configure FLAGS guest");
    for(bool prefix : {false,true}) for(bool nop : {false,true}) {
        unsigned n=0;if(prefix)code[n++]=0x66;code[n++]=0x9c;
        if(nop)code[n++]=0x90;code[n++]=0x0f;code[n]=0xa2;
        unsigned width=prefix?(bits==16?4:2):(bits==64?8:bits/8);
        const uint64_t flags=0x403; // DF + CF; internal TF must not leak onto the stack
        hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RSP,0x9080);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,flags);
        auto r=hv_vcpu_run(c);uint64_t stacked=0;memcpy(&stacked,code+0x1080-width,width);
        check(r==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RSP,0x9080-width) && stacked==flags,
            "PUSHF width/prefix preserves guest DF/CF and hides internal TF");
        code[prefix?1:0]=0x9d;const uint64_t popped=0xa46; // IF/OF/ZF/PF
        memcpy(code+0x1080-width,&popped,width);
        hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
        check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RSP,0x9080) && reg(c,HV_X86_RFLAGS,popped),
            "POPF width/prefix updates IF and arithmetic flags and resumes bounded execution");
    }
    // CPUID discovery toggles EFLAGS.ID with PUSHF/POPF. Keep the real
    // operand width32 in16-bit mode and the native width in32/64-bit mode.
    unsigned cursor=0;
    auto emit=[&](unsigned char byte){code[cursor++]=byte;};
    auto wide=[&](unsigned char op){if(bits==16)emit(0x66);emit(op);};
    wide(0x9c);wide(0x58);if(bits==16)emit(0x66);
    emit(0x35);emit(0);emit(0);emit(0x20);emit(0);wide(0x50);wide(0x9d);
    unsigned cpuidOffset=cursor;emit(0x0f);emit(0xa2);
    for(unsigned toggle=0;toggle<2;++toggle){
        hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RSP,0x9080);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,toggle?0x200202:0x202);
        check(hv_vcpu_run(c)==0&&field(c,VMCS_RO_EXIT_REASON,10)&&reg(c,HV_X86_RIP,0x8000+cpuidOffset)&&
              reg(c,HV_X86_RFLAGS,toggle?0x202:0x200202)&&reg(c,HV_X86_RSP,0x9080),
              "PUSHF/POPF toggles ID for CPUID discovery without leaking internal TF");
        check(hv_vcpu_run(c)==0&&field(c,VMCS_RO_EXIT_REASON,10),"ID survives separate validated mode re-entry");
    }
    hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
    unsigned width=bits==64?8:bits/8;
    // Preserve IF and shadow across the exact 1024-entry quantum boundary.
    memset(code,0x90,1023);code[1023]=0xfb;code[1024]=0x90;code[1025]=0xf4;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,1) && reg(c,HV_X86_RIP,0x8400) &&
          reg(c,HV_X86_RFLAGS,0x202) && field(c,0x4824,1),
          "STI on final step preserves IF and shadow across quantum");
    hv_vmx_vcpu_write_vmcs(c,0x4002,4);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,7) && reg(c,HV_X86_RIP,0x8401) && field(c,0x4824,0),
          "interrupt window opens only after instruction following STI");
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,7) && reg(c,HV_X86_RIP,0x8401),
          "open interrupt window exits before executing next instruction");
    hv_vmx_vcpu_write_vmcs(c,0x4002,0);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,12) && reg(c,HV_X86_RIP,0x8401),
          "HLT with IF1 retains VMX RIP at HLT");
    code[0]=0xfa;code[1]=0xf4;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RFLAGS,2);
    hv_vmx_vcpu_write_vmcs(c,0x4002,4);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,12) && reg(c,HV_X86_RIP,0x8001),
          "IF0 does not report an interrupt window");
    hv_vcpu_write_register(c,HV_X86_RFLAGS,0x202);
    // A consumed instruction intercept must also retire a restored shadow.
    code[0]=0xf3;code[1]=0x90;code[2]=0xf4;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vmx_vcpu_write_vmcs(c,0x4824,1);
    hv_vmx_vcpu_write_vmcs(c,0x4002,4);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,7) && reg(c,HV_X86_RIP,0x8002) && field(c,0x4824,0),
          "intercepted PAUSE retires restored shadow before window exit");
    hv_vmx_vcpu_write_vmcs(c,0x4002,0);
    // MOV SS suppresses #DB for one instruction; PUSHF must still hide internal TF.
    uint64_t gdt[]={0,0x00af9b000000ffffull,0x00cf93000000ffffull};
    memcpy(code+0x800,gdt,sizeof(gdt));
    hv_vmx_vcpu_write_vmcs(c,0x6816,0x8800);hv_vmx_vcpu_write_vmcs(c,0x4810,sizeof(gdt)-1);
    code[0]=0x8e;code[1]=0xd0;code[2]=0x9c;code[3]=0x0f;code[4]=0xa2;
    hv_vcpu_write_register(c,HV_X86_RAX,bits==16?0:16);
    hv_vcpu_write_register(c,HV_X86_RFLAGS,0x202);hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    hv_vcpu_write_register(c,HV_X86_RSP,0x9080);
    auto ssResult=hv_vcpu_run(c);uint64_t ssFlags=0;memcpy(&ssFlags,code+0x1080-width,width);
    check(ssResult==0 && field(c,VMCS_RO_EXIT_REASON,10) && ssFlags==0x202 && field(c,0x4824,0),
          "MOV SS followed by PUSHF hides TF and clears interrupt shadow");
    hv_vmx_vcpu_write_vmcs(c,0x4016,0x80000202);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED,"unsupported NMI injection remains rejected before guest entry");
    hv_vmx_vcpu_write_vmcs(c,0x4016,0);
    hv_vmx_vcpu_write_vmcs(c,0x4824,8);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED,"unsupported NMI blocking remains rejected");
    hv_vmx_vcpu_write_vmcs(c,0x4824,0);

    // PAUSE must preserve architectural state and remain bounded even in spin loops.
    for(unsigned i=0;i<1536;++i) {code[i*2]=0xf3;code[i*2+1]=0x90;}
    code[3072]=0x0f;code[3073]=0xa2;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RSP,0x9080);
    hv_vcpu_write_register(c,HV_X86_RFLAGS,0x403);hv_vcpu_write_register(c,HV_X86_RAX,0x1234);
    auto pauseResult=hv_vcpu_run(c);
    check(pauseResult==0 && field(c,VMCS_RO_EXIT_REASON,1) && reg(c,HV_X86_RIP,0x8800) &&
          reg(c,HV_X86_RFLAGS,0x403) && reg(c,HV_X86_RAX,0x1234) && reg(c,HV_X86_RSP,0x9080),
          "1024 PAUSE hints yield with flags/registers/stack unchanged");
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RIP,0x8c00),
          "PAUSE stream resumes across quantum boundary");
    code[0]=0xf3;code[1]=0x90;code[2]=0xeb;code[3]=0xfc;
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,1) && reg(c,HV_X86_RIP,0x8000),
          "infinite PAUSE/JMP guest loop returns within one quantum");
    for(unsigned op : {0x9cu,0x9du}) {
        code[0]=op;code[1]=0xf3;code[2]=0x90;code[3]=0x0f;code[4]=0xa2;
        uint64_t value=0x403;memcpy(code+0x1080,&value,width);
        hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RSP,0x9080);
        hv_vcpu_write_register(c,HV_X86_RFLAGS,value);
        check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,10) && reg(c,HV_X86_RIP,0x8003) &&
              reg(c,HV_X86_RFLAGS,value) && reg(c,HV_X86_RSP,op==0x9c?0x9080-width:0x9080+width),
              "PUSHF/POPF followed immediately by PAUSE preserves step boundary");
    }
    code[0]=0x9c;code[1]=0x90;code[2]=0x0f;code[3]=0xa2;
    hv_vcpu_write_register(c,HV_X86_RFLAGS,3);hv_vcpu_write_register(c,HV_X86_RIP,0x8000);
    hv_vcpu_write_register(c,HV_X86_RSP,0x9000+width/2);
    auto r=hv_vcpu_run(c);uint64_t crossed=0;memcpy(&crossed,code+0x1000-width/2,width);
    check(r==0 && field(c,VMCS_RO_EXIT_REASON,10) && crossed==3,"hardware PUSHF handles a stack access across pages");
    check(hv_vm_protect(0x9000,4096,HV_MEMORY_READ)==0,"make stack read-only in NPT");
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RSP,0x9080);
    check(hv_vcpu_run(c)==0 && field(c,VMCS_RO_EXIT_REASON,48) && reg(c,HV_X86_RIP,0x8000),
        "PUSHF stack protection fault remains a real NPT exit");
    hv_vm_protect(0x9000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC);
    code[0]=0x9d;code[1]=0x90;code[2]=0x0f;code[3]=0xa2;
    uint64_t illegal=0x3002;memcpy(code+0x1080,&illegal,width);
    hv_vcpu_write_register(c,HV_X86_RIP,0x8000);hv_vcpu_write_register(c,HV_X86_RSP,0x9080);
    check(hv_vcpu_run(c)==HV_UNSUPPORTED && reg(c,HV_X86_RIP,0x8001) && reg(c,HV_X86_RSP,0x9080+width),"POPF enabling unsupported IOPL stops before following instruction");
    check(hv_vm_unmap(0x1000,4*4096)==0 && hv_vm_unmap(0x8000,2*4096)==0,"unmap FLAGS guest");
    free(p);check(hv_vcpu_destroy(c)==0 && hv_vm_destroy()==0,"FLAGS cleanup");
}
int main() {setvbuf(stdout,nullptr,_IONBF,0);mode(16);mode(32);mode(64);return failures?1:0;}
