// AMDHV: AMD SVM backend for Hypervisor.framework on Ryzentosh.
// At start AMDHV only reads CPUID. Everything else runs on demand:
//   sudo sysctl debug.amdhv.selftest=1      phase 1: VMRUN self-test on every CPU
//   sysctl debug.amdhv.result              summary, details in `ioreg -c AMDHV -l`
//   sudo sysctl debug.amdhv.takeover=1      phase 2: install trap stubs, kern.hv_support=1
//   sudo sysctl debug.amdhv.takeover=0      remove them again
//   sysctl -n debug.amdhv.log              trace of every trap call
// Boot-arg -amdhvoff makes start() bail out without touching anything.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include <IOKit/IOLocks.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include "svm_selftest.hpp"
#include "hv_traps.hpp"
#include "klog.hpp"
#include "vm_backend.hpp"
#include "vcpu_svm.hpp"

extern "C" unsigned int real_ncpus;

using namespace amdhv;

class AMDHV : public IOService {
    OSDeclareDefaultStructors(AMDHV)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    bool terminate(IOOptionBits options = 0) override {
        // XNU raw traps do not retain the kext. Remove through AuxKC + reboot.
        if (traps_.runtimePinned()) return false;
        return IOService::terminate(options);
    }

    int runSelfTest();            // called from sysctl, returns errno
    int setTakeover(bool on);     // called from sysctl, returns errno
    bool takeoverActive() const { return traps_.active(); }

private:
    void publishInfo(const SvmInfo &info);
    void publishResults();

    SvmSelfTest test_;
    SvmInfo info_ = {};
    TrapHost traps_;
};

OSDefineMetaClassAndStructors(AMDHV, IOService)

#define LOG(fmt, ...) IOLog("AMDHV: " fmt "\n", ##__VA_ARGS__)

// ---- sysctl debug.amdhv.* ----
static AMDHV  *gInstance = nullptr;
static IOLock *gLock = nullptr;
static char    gResult[64] = "not-run";

// IOLockLock/IOLockUnlock are not in the exported symbol sets; lck_mtx_* are (Libkern).
static void amdhvLock()   { lck_mtx_lock(IOLockGetMachLock(gLock)); }
static void amdhvUnlock() { lck_mtx_unlock(IOLockGetMachLock(gLock)); }

static int sysctlSelfTest SYSCTL_HANDLER_ARGS {
    int value = 0;
    int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr) return err;   // plain read returns 0
    if (value != 1) return EINVAL;
    amdhvLock();
    err = gInstance ? gInstance->runSelfTest() : ENODEV;
    amdhvUnlock();
    return err;
}

static int sysctlTakeover SYSCTL_HANDLER_ARGS {
    amdhvLock();
    int value = gInstance && gInstance->takeoverActive();
    int err = sysctl_handle_int(oidp, &value, 0, req);
    if (!err && req->newptr) {
        if (value != 0 && value != 1) err = EINVAL;
        else err = gInstance ? gInstance->setTakeover(value == 1) : ENODEV;
    }
    amdhvUnlock();
    return err;
}

static int sysctlLog SYSCTL_HANDLER_ARGS {
    if (req->newptr) {             // any write clears the log
        klogClear();
        return 0;
    }
    return klogCopyOut(req);
}

static int sysctlFast SYSCTL_HANDLER_ARGS {
    amdhvLock();
    int value=fastRunEnabled()?1:0;
    int err=sysctl_handle_int(oidp,&value,0,req);
    if(!err && req->newptr) {
        if(value!=0 && value!=1)err=EINVAL;
        else if(!gInstance)err=ENODEV;
        else if(gInstance->takeoverActive() || vmBackendCount())err=EBUSY;
        else setFastRunEnabled(value==1);
    }
    amdhvUnlock();return err;
}

static int sysctlVms SYSCTL_HANDLER_ARGS {
    int count = static_cast<int>(vmBackendCount());
    return sysctl_handle_int(oidp, &count, 0, req);
}

static int sysctlTraceExits SYSCTL_HANDLER_ARGS {
    amdhvLock();
    int value=fullExitTraceEnabled()?1:0;
    int err=sysctl_handle_int(oidp,&value,0,req);
    if(!err && req->newptr) {
        if(value!=0 && value!=1)err=EINVAL;
        else if(!gInstance)err=ENODEV;
        else setFullExitTraceEnabled(value==1);
    }
    amdhvUnlock();return err;
}

static int sysctlParallel SYSCTL_HANDLER_ARGS {
    amdhvLock();
    int value=parallelRunEnabled()?1:0;
    int err=sysctl_handle_int(oidp,&value,0,req);
    if(!err && req->newptr) {
        if(value!=0 && value!=1)err=EINVAL;
        else if(!gInstance)err=ENODEV;
        else if(gInstance->takeoverActive() || vmBackendCount())err=EBUSY;
        else setParallelRunEnabled(value==1);
    }
    amdhvUnlock();return err;
}

SYSCTL_NODE(_debug, OID_AUTO, amdhv, CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, "AMDHV");
SYSCTL_PROC(_debug_amdhv, OID_AUTO, selftest, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlSelfTest, "I", "write 1 to run the SVM self-test on every CPU");
SYSCTL_STRING(_debug_amdhv, OID_AUTO, result, CTLFLAG_RD | CTLFLAG_LOCKED,
              gResult, sizeof(gResult), "last self-test result");
SYSCTL_PROC(_debug_amdhv, OID_AUTO, takeover, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlTakeover, "I", "1 = AMDHV owns the hv traps and kern.hv_support=1");
SYSCTL_PROC(_debug_amdhv, OID_AUTO, log, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlLog, "A", "AMDHV trace log (write anything to clear)");

SYSCTL_PROC(_debug_amdhv, OID_AUTO, fast, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlFast, "I", "opt-in host-IRQ preempted execution without internal TF");
SYSCTL_PROC(_debug_amdhv, OID_AUTO, vms, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
            nullptr, 0, sysctlVms, "I", "live AMDHV VM/task targets");
SYSCTL_PROC(_debug_amdhv, OID_AUTO, trace_exits, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlTraceExits, "I", "1 = full successful-exit trace; 0 = sample fast exits (errors always logged)");
SYSCTL_PROC(_debug_amdhv, OID_AUTO, parallel, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlParallel, "I", "opt-in parallel fast vCPU runs; change only with takeover disabled and no VMs");

static void setNumber(IOService *s, const char *key, uint64_t v, unsigned bits = 64) {
    if (OSNumber *n = OSNumber::withNumber(v, bits)) {
        s->setProperty(key, n);
        n->release();
    }
}

void AMDHV::publishInfo(const SvmInfo &info) {
    setProperty("SVM CPUID", info.cpuidSvm);
    setNumber(this, "SVM Revision", info.revision, 32);
    setNumber(this, "SVM ASIDs", info.nasid, 32);
    setNumber(this, "SVM Feature Bits", info.features, 64);

    static const struct { uint32_t bit; const char *name; } kFeat[] = {
        {svm::FEAT_NP, "NPT"},           {svm::FEAT_LBRV, "LbrVirt"},
        {svm::FEAT_SVML, "SVML"},        {svm::FEAT_NRIPS, "NRIPS"},
        {svm::FEAT_TSC_RATE, "TscRate"}, {svm::FEAT_VMCB_CLEAN, "VmcbClean"},
        {svm::FEAT_FLUSH_ASID, "FlushByAsid"}, {svm::FEAT_DECODE, "DecodeAssists"},
        {svm::FEAT_PAUSE_FILT, "PauseFilter"}, {svm::FEAT_PAUSE_THR, "PauseThreshold"},
        {svm::FEAT_AVIC, "AVIC"},        {svm::FEAT_VLS, "VirtVmsaveVmload"},
        {svm::FEAT_VGIF, "vGIF"},        {svm::FEAT_GMET, "GMET"},
        {svm::FEAT_X2AVIC, "x2AVIC"},    {svm::FEAT_VNMI, "vNMI"},
    };
    if (OSArray *arr = OSArray::withCapacity(16)) {
        for (auto &f : kFeat) {
            if (!(info.features & f.bit)) continue;
            if (OSString *s = OSString::withCString(f.name)) {
                arr->setObject(s);
                s->release();
            }
        }
        setProperty("SVM Features", arr);
        arr->release();
    }
}

void AMDHV::publishResults() {
    const unsigned n = test_.cpuCount();
    const unsigned pass = test_.passCount();

    char summary[64];
    snprintf(summary, sizeof(summary), "%s %u/%u", pass == n ? "PASS" : "FAIL", pass, n);
    strlcpy(gResult, summary, sizeof(gResult));
    setProperty("SelfTest", summary);
    LOG("self-test %s", summary);

    OSArray *cpus = OSArray::withCapacity(n);
    if (!cpus) return;
    for (unsigned c = 0; c < n; c++) {
        const CpuResult &r = test_.result(c);
        OSDictionary *d = OSDictionary::withCapacity(6);
        if (!d) continue;

        if (OSString *s = OSString::withCString(statusName(r.status))) { d->setObject("status", s); s->release(); }
        if (OSNumber *v = OSNumber::withNumber(c, 32))                  { d->setObject("cpu", v); v->release(); }
        if (OSNumber *v = OSNumber::withNumber(r.guestRaxAtCpuid, 64))  { d->setObject("guest-rax", v); v->release(); }
        if (OSNumber *v = OSNumber::withNumber(r.exitInfo1, 64))        { d->setObject("exitinfo1", v); v->release(); }
        if (OSNumber *v = OSNumber::withNumber(r.vmcr, 64))             { d->setObject("vm-cr", v); v->release(); }
        if (OSArray *ex = OSArray::withCapacity(r.exits)) {
            for (unsigned i = 0; i < r.exits; i++) {
                if (OSNumber *v = OSNumber::withNumber(r.exitCodes[i], 64)) { ex->setObject(v); v->release(); }
            }
            d->setObject("exits", ex);
            ex->release();
        }
        cpus->setObject(d);
        d->release();

        if (r.status != kPass) {
            LOG("cpu %u: %s, exits=%u last=0x%llx info1=0x%llx vm_cr=0x%llx",
                c, statusName(r.status), r.exits,
                r.exits ? r.exitCodes[r.exits - 1] : 0ULL, r.exitInfo1, r.vmcr);
        }
        if (r.eferAfter != r.eferBefore) {
            LOG("cpu %u: EFER not restored (0x%llx -> 0x%llx)", c, r.eferBefore, r.eferAfter);
        }
    }
    setProperty("SelfTest CPUs", cpus);
    cpus->release();
}

int AMDHV::runSelfTest() {
    if (!info_.cpuidSvm || !(info_.features & svm::FEAT_NP)) {
        strlcpy(gResult, "SKIPPED (no SVM/NPT)", sizeof(gResult));
        return ENOTSUP;
    }
    if (!test_.init(real_ncpus, info_.features & svm::FEAT_NRIPS)) {
        LOG("self-test init failed (memory)");
        strlcpy(gResult, "SKIPPED (alloc failed)", sizeof(gResult));
        return ENOMEM;
    }
    LOG("running self-test on %u CPUs", real_ncpus);
    test_.run();
    publishResults();
    test_.release();
    return 0;
}

int AMDHV::setTakeover(bool on) {
    if (!on) {
        if (!traps_.restore()) return EBUSY;
        setProperty("Takeover", false);
        return 0;
    }
    char why[96];
    bool ok = traps_.takeover(why, sizeof(why));
    setProperty("Takeover", ok);
    setProperty("Takeover Status", why);
    return ok ? 0 : EIO;
}

bool AMDHV::start(IOService *provider) {
    int dummy = 0;
    if (PE_parse_boot_argn("-amdhvoff", &dummy, sizeof(dummy))) {
        IOLog("AMDHV: disabled by -amdhvoff\n");
        return false;
    }
    if (!IOService::start(provider)) return false;

    gLock = IOLockAlloc();
    if (!gLock || !klogInit()) return false;

    info_ = probeSvm();
    publishInfo(info_);
    setProperty("SelfTest", "not-run (sudo sysctl debug.amdhv.selftest=1)");
    LOG("SVM cpuid=%d rev=%u asids=%u features=0x%08x cpus=%u",
        info_.cpuidSvm, info_.revision, info_.nasid, info_.features, real_ncpus);

    gInstance = this;
    sysctl_register_oid(&sysctl__debug_amdhv);
    sysctl_register_oid(&sysctl__debug_amdhv_selftest);
    sysctl_register_oid(&sysctl__debug_amdhv_result);
    sysctl_register_oid(&sysctl__debug_amdhv_takeover);
    sysctl_register_oid(&sysctl__debug_amdhv_log);
    sysctl_register_oid(&sysctl__debug_amdhv_vms);
    sysctl_register_oid(&sysctl__debug_amdhv_fast);
    sysctl_register_oid(&sysctl__debug_amdhv_trace_exits);
    sysctl_register_oid(&sysctl__debug_amdhv_parallel);
    klog("AMDHV 0.3.42 started, SVM features=0x%08x", info_.features);

    registerService();
    return true;
}

void AMDHV::stop(IOService *provider) {
    sysctl_unregister_oid(&sysctl__debug_amdhv_parallel);
    sysctl_unregister_oid(&sysctl__debug_amdhv_trace_exits);
    sysctl_unregister_oid(&sysctl__debug_amdhv_fast);
    sysctl_unregister_oid(&sysctl__debug_amdhv_vms);
    sysctl_unregister_oid(&sysctl__debug_amdhv_log);
    sysctl_unregister_oid(&sysctl__debug_amdhv_takeover);
    sysctl_unregister_oid(&sysctl__debug_amdhv_result);
    sysctl_unregister_oid(&sysctl__debug_amdhv_selftest);
    sysctl_unregister_oid(&sysctl__debug_amdhv);

    if (gLock) {
        amdhvLock();                  // wait for a running self-test
        traps_.restore();             // never leave our trap tables behind
        gInstance = nullptr;
        amdhvUnlock();
        IOLockFree(gLock);
        gLock = nullptr;
    }
    test_.release();
    klogFree();
    LOG("stopped");
    IOService::stop(provider);
}
