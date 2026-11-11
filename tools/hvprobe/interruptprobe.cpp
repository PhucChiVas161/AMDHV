#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned failures;
static void check(bool ok, const char *label) { printf("%s: %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
static bool exitIs(hv_vcpuid_t cpu, uint64_t expected) {
    uint64_t reason=0, length=0, qualification=0;
    return hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_REASON, &reason)==0 && reason==expected &&
        (expected!=1 || (hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_VMEXIT_INSTR_LEN, &length)==0 && length==0 &&
        hv_vmx_vcpu_read_vmcs(cpu, VMCS_RO_EXIT_QUALIFIC, &qualification)==0 && qualification==0));
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (hv_vm_create(HV_VM_DEFAULT)) return 2;
    hv_vcpuid_t cpu;
    if (hv_vcpu_create(&cpu, HV_VCPU_DEFAULT)) { hv_vm_destroy(); return 2; }
    void *page=nullptr;
    if (posix_memalign(&page, 4096, 4096)) return 2;
    memset(page, 0, 4096); static_cast<unsigned char *>(page)[0]=0xf4;
    if (hv_vm_map(page,0,4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC)) return 2;
    check(hv_vcpu_interrupt(nullptr,0)==0,"empty interrupt request");
    hv_vcpuid_t ids[2]={cpu,63};
    check(hv_vcpu_interrupt(ids,2)!=0,"mixed live/missing vCPU request rejected atomically");
    check(hv_vcpu_run(cpu)==0 && exitIs(cpu,12),"rejected batch leaves no pending cancellation");
    check(hv_vcpu_interrupt(&cpu,1)==0 && hv_vcpu_interrupt(&cpu,1)==0,"idle requests coalesce");
    check(hv_vcpu_run_until(cpu,HV_DEADLINE_FOREVER)==0 && exitIs(cpu,1),"pending request returns host exit with no guest instruction");
    uint64_t rip=~0ull;
    check(hv_vcpu_read_register(cpu,HV_X86_RIP,&rip)==0 && rip==0,"cancellation preserves RIP");
    check(hv_vcpu_run(cpu)==0 && exitIs(cpu,12),"request consumed once; next run reaches HLT");
    std::atomic<bool> stop{false}, ready{false}; std::atomic<unsigned> requests{0}, errors{0};
    // A separate host thread continuously kicks an instruction-bounded guest loop.
    static_cast<unsigned char *>(page)[0]=0xeb; static_cast<unsigned char *>(page)[1]=0xfe;
    std::thread kicker([&] {
        ready=true;
        while (!stop.load()) { if (hv_vcpu_interrupt(&cpu,1)) ++errors; else ++requests; std::this_thread::yield(); }
    });
    while (!ready.load()) std::this_thread::yield();
    bool canceled=false;
    for (unsigned i=0;i<1000 && !canceled;++i) {
        auto r=hv_vcpu_run(cpu);
        canceled=r==0 && exitIs(cpu,1);
        if (r && r!=HV_UNSUPPORTED) break;
    }
    stop=true; kicker.join();
    check(canceled && requests>0 && errors==0,"cross-thread interrupt observed by runner (pre-entry or in-flight)");
    check(hv_vcpu_interrupt(&cpu,1)==0,"queue request before destroy");
    check(hv_vcpu_destroy(cpu)==0,"destroy vCPU with pending request");
    check(hv_vcpu_create(&cpu,HV_VCPU_DEFAULT)==0,"recreate vCPU slot");
    static_cast<unsigned char *>(page)[0]=0xf4;
    check(hv_vcpu_run(cpu)==0 && exitIs(cpu,12),"reused slot has no stale cancellation");
    check(hv_vcpu_destroy(cpu)==0,"destroy recreated vCPU");
    check(hv_vcpu_interrupt(&cpu,1)!=0,"destroyed vCPU rejected");
    check(hv_vm_unmap(0,4096)==0,"unmap guest"); free(page);
    check(hv_vm_destroy()==0,"destroy VM");
    printf("%s: host cancellation gate (%u failures); no guest IRQ injection claimed\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
