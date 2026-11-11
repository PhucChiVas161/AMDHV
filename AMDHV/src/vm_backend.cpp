#include "vm_backend.hpp"
#include "svm_defs.hpp"
#include "hv_abi.hpp"
#include "guest_memory.hpp"
#include "vcpu_svm.hpp"
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include "hv_traps.hpp"
#include "klog.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOLib.h>
#include <kern/locks.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/libkern.h>
#include <sys/proc.h>

extern "C" int cpu_number(void); // com.apple.kpi.unsupported

namespace amdhv {
namespace {
constexpr kern_return_t NoResources = (kern_return_t)0xfae94005;
constexpr kern_return_t NoDevice = (kern_return_t)0xfae94006;
constexpr kern_return_t Denied = (kern_return_t)0xfae94007;
constexpr kern_return_t BadAddress = (kern_return_t)0xfae94008;
constexpr unsigned MaxVms = 16;
struct Shared {
    IOBufferMemoryDescriptor *memory = nullptr;
    IOMemoryMap *mapping = nullptr;
    bool prepared = false;
    bool alloc(task_t task, size_t size, bool readOnly) {
        memory = IOBufferMemoryDescriptor::withOptions(kIOMemoryKernelUserShared, size, PAGE_SIZE);
        if (!memory) return false;
        if (memory->prepare(kIODirectionInOut) != kIOReturnSuccess) return false;
        prepared = true;
        bzero(memory->getBytesNoCopy(), size);
        mapping = memory->createMappingInTask(task, 0,
            kIOMapAnywhere | (readOnly ? kIOMapReadOnly : 0));
        return mapping != nullptr;
    }
    void release() {
        if (mapping) { mapping->release(); mapping = nullptr; }
        if (memory) {
            if (prepared) memory->complete(kIODirectionInOut);
            memory->release(); memory = nullptr;
        }
        prepared = false;
    }
};
struct Vcpu {
    thread_t thread = nullptr;
    unsigned id = 0;
    uint64_t runCalls = 0, continued = 0;
    uint8_t *rw = nullptr, *ro = nullptr, *rwFp = nullptr, *roFp = nullptr;
    VcpuState state;
    VcpuSvm svm;
};
struct Vm {
    task_t task = nullptr; // valid until XNU task-destroy callback; no extra task reference
    Shared rw, ro, clock;
    GuestMemory memory;
    Vcpu *cpus[abi::MaxVcpus] = {};
    unsigned cpuCount = 0;
    bool taskGone = false;
    void release() { memory.release(); clock.release(); ro.release(); rw.release(); }
};
static IORWLock *gate;
static IOLock *interruptGate;
struct InterruptSlot { task_t task = nullptr; VcpuInterrupts requests; };
static InterruptSlot interruptSlots[MaxVms];
// Lock order: gate -> interruptGate. Interrupt trap takes only interruptGate,
// so it can request exit while runners hold shared/exclusive gate ownership.
// Writers exclude all runners before changing mappings, registries or lifetimes.
static KernelApi api;
// Set the pending bits, then kick every physical CPU that is executing one of
// the requested vCPUs so it leaves guest mode now, not at the next host tick.
// Without the kick an interrupt/IPI aimed at a busy vCPU waited for an
// unrelated host interrupt on that core (up to a scheduler quantum).
static kern_return_t interruptTask(uint64_t mask) {
    uint64_t cpus = 0;
    lck_mtx_lock(IOLockGetMachLock(interruptGate));
    kern_return_t result = NoDevice;
    for (auto &slot : interruptSlots) if (slot.task == current_task()) {
        if (slot.requests.request(mask)) {
            result = HV_SUCCESS;
            cpus = slot.requests.runningCpus(mask);
        }
        break;
    }
    lck_mtx_unlock(IOLockGetMachLock(interruptGate));
    if (cpus && api.cpus_kick) {
        const boolean_t interrupts = ml_set_interrupts_enabled(false);
        const int self = cpu_number();
        if (self >= 0 && self < 64) cpus &= ~(1ull << self);
        if (cpus) api.cpus_kick(cpus);
        ml_set_interrupts_enabled(interrupts);
    }
    return result;
}
static Vm *vms[MaxVms];
static bool accepting;
static bool parallelRun=false;
// Threads currently blocked on (or about to take) the gate exclusively. A vCPU
// that keeps running in-kernel under its shared lease checks this so mapping,
// lifecycle and state writers are never starved by a guest that does not exit.
static int32_t writersWaiting=0;
struct Guard {
    bool shared_;
    explicit Guard(bool shared=false) : shared_(shared) {
        if(shared_)lck_rw_lock_shared(IORWLockGetMachLock(gate));
        else {
            __atomic_add_fetch(&writersWaiting,1,__ATOMIC_SEQ_CST);
            lck_rw_lock_exclusive(IORWLockGetMachLock(gate));
            __atomic_sub_fetch(&writersWaiting,1,__ATOMIC_SEQ_CST);
        }
    }
    ~Guard() {
        if(shared_)lck_rw_unlock_shared(IORWLockGetMachLock(gate));
        else lck_rw_unlock_exclusive(IORWLockGetMachLock(gate));
    }
    Guard(const Guard &)=delete;
    Guard &operator=(const Guard &)=delete;
};
static int slotForTask(task_t task) {
    for (unsigned i = 0; i < MaxVms; ++i) if (vms[i] && vms[i]->task == task) return static_cast<int>(i);
    return -1;
}
static unsigned countLocked() {
    unsigned count = 0;
    for (auto vm : vms) if (vm) ++count;
    return count;
}
static bool entitled(task_t task) {
    OSObject *ent = IOUserClient::copyClientEntitlement(task, "com.apple.security.hypervisor");
    bool ok = ent == kOSBooleanTrue;
    if (ent) ent->release();
    return ok;
}
static void dispose(unsigned index, const char *why) {
    Vm *vm = vms[index];
    lck_mtx_lock(IOLockGetMachLock(interruptGate));
    interruptSlots[index].task = nullptr;
    interruptSlots[index].requests.reset();
    lck_mtx_unlock(IOLockGetMachLock(interruptGate));
    vms[index] = nullptr;
    vm->release();
    delete vm;
    klog("vm: %s, live=%u", why, countLocked());
}
// XNU may invoke this from another task. Do not use current_task/set_task_target here.
static void taskDestroyed(void *target) {
    Guard lock;
    for (unsigned i = 0; i < MaxVms; ++i) {
        if (vms[i] == target) {
            vms[i]->taskGone = true;
            if (!vms[i]->cpuCount) dispose(i, "task exit");
            return;
        }
    }
}
static void releaseCpu(Vm *vm, unsigned id) {
    Vcpu *cpu = vm->cpus[id];
    if (cpu->continued) klog("vcpu: id=%u run calls=%llu continued in-kernel=%llu", id, cpu->runCalls, cpu->continued);
    if (!cpu->runCalls) {
        klog("vcpu: destroyed before first run; last shared state id=%u", id);
        cpu->state.pull(cpu->rw, cpu->rwFp);
        logVcpuState(cpu->state);
    }
    lck_mtx_lock(IOLockGetMachLock(interruptGate));
    interruptSlots[slotForTask(vm->task)].requests.remove(id);
    lck_mtx_unlock(IOLockGetMachLock(interruptGate));
    vm->cpus[id] = nullptr; --vm->cpuCount;
    cpu->svm.release(); delete cpu;
    klog("vcpu: destroy id=%u remaining=%u", id, vm->cpuCount);
}
static void threadDestroyed(void *target) {
    Guard lock;
    for (unsigned i = 0; i < MaxVms; ++i) if (vms[i]) {
        for (unsigned id = 0; id < abi::MaxVcpus; ++id) if (vms[i]->cpus[id] == target) {
            releaseCpu(vms[i], id);
            if (vms[i]->taskGone && !vms[i]->cpuCount) dispose(i, "last thread exit");
            return;
        }
    }
}
// All guest hardware state is restored before scheduling can resume.
static void scheduled(void *) {}
static void volatileState(void *, int) {}
// XNU calls suspend unconditionally from acpi_sleep_kernel once callbacks are
// registered: a null pointer here is a jump to address 0 on the way to sleep.
// No vCPU can be inside the guest (entry runs with interrupts disabled and
// restores EFER/HSAVE before returning), so there is no hardware state to save.
static void systemSuspend() {}
// Cores come back from S3 with empty TLBs; make that explicit for every ASID.
static void systemResume() { bumpFlushEpoch(); }
// Guest RAM is wired by its IOMemoryDescriptor pins; nothing can be discarded.
static void memoryPressure() {}
static void publishMsrs(Vm *vm, Vcpu *cpu) {
    auto out = static_cast<uint8_t *>(vm->ro.memory->getBytesNoCopy()) + abi::RoMsrOffset + cpu->id * abi::MsrStride;
    for (unsigned id = 1; id <= 5; ++id) {
        const unsigned slot = id - 1;
        cpu->ro[0x58 + id] = (cpu->state.managedMask & (1ull << id)) ? static_cast<uint8_t>(slot) : 0xff;
        VcpuState::write(out, slot * 16, managedMsrAddress(id));
        VcpuState::write(out, slot * 16 + 8, cpu->state.msrs[id]);
    }
}
static kern_return_t createCpu(Vm *vm, uint64_t arg) {
    abi::VcpuCreate in = {};
    if (copyin(arg, &in, sizeof(in))) return BadAddress;
    if (in.flags & ~2ull) return HV_UNSUPPORTED; // HV_VCPU_TSC_RELATIVE; RDPMC remains unsupported
    if (api.get_thread_target()) return HV_ERROR;
    unsigned id = 0;
    while (id < abi::MaxVcpus && vm->cpus[id]) ++id;
    if (id == abi::MaxVcpus) return NoResources;
    Vcpu *cpu = new Vcpu;
    if (!cpu) return NoResources;
    if (!cpu->svm.init()) { delete cpu; return NoResources; }
    cpu->id = id; cpu->thread = current_thread();
    const int vmSlot = slotForTask(vm->task);
    cpu->svm.assignAsid((vmSlot < 0 ? MaxVms : static_cast<unsigned>(vmSlot)) * abi::MaxVcpus + id);
    cpu->rw = static_cast<uint8_t *>(vm->rw.memory->getBytesNoCopy()) + id * abi::RwStride;
    cpu->ro = static_cast<uint8_t *>(vm->ro.memory->getBytesNoCopy()) + id * abi::RoStride;
    cpu->rwFp = static_cast<uint8_t *>(vm->rw.memory->getBytesNoCopy()) + abi::RwFpOffset + id * abi::FpStride;
    cpu->roFp = static_cast<uint8_t *>(vm->ro.memory->getBytesNoCopy()) + abi::RoFpOffset + id * abi::FpStride;
    bzero(cpu->rwFp, abi::FpStride); bzero(cpu->roFp, abi::FpStride);
    bzero(cpu->rw, abi::RwStride); bzero(cpu->ro, abi::RoStride);
    cpu->state.reset(); cpu->state.relativeTsc = (in.flags & 2) != 0;
    cpu->state.tscSnapshot = amdhv_read_tsc();
    cpu->state.publish(cpu->rw, cpu->roFp, cpu->rwFp);
    VcpuState::write(cpu->ro, 0x18, in.flags);
    memset(cpu->ro + 0x58, 0xff, 0x90);
    publishMsrs(vm, cpu);
    cpu->ro[0xe8] = 1; // debug-register cache valid; initial values supplied in RW.
    if (copyout(&id, in.idOut, sizeof(id))) {
        cpu->svm.release(); delete cpu; return BadAddress;
    }
    vm->cpus[id] = cpu; ++vm->cpuCount;
    lck_mtx_lock(IOLockGetMachLock(interruptGate));
    interruptSlots[slotForTask(vm->task)].requests.add(id);
    lck_mtx_unlock(IOLockGetMachLock(interruptGate));
    api.set_thread_target(cpu);
    klog("vcpu: create id=%u count=%u", id, vm->cpuCount);
    return 0;
}
static kern_return_t cpuTrap(Vm *vm, unsigned index, uint64_t arg) {
    Vcpu *cpu = nullptr;
    for (auto item : vm->cpus) if (item && item->thread == current_thread()) { cpu = item; break; }
    if (!cpu) return NoDevice;
    if (index == 5) {
        api.set_thread_target(nullptr); releaseCpu(vm, cpu->id); return 0;
    }
    if (index == 3 || index == 4) {
        unsigned id = compactMsrId(arg);
        if (!id || (id == TscMsrId && !cpu->state.relativeTsc)) return HV_UNSUPPORTED;
        cpu->state.pull(cpu->rw, cpu->rwFp);
        uint64_t bit = 1ull << id;
        if (index == 3) {
            cpu->state.managedMask |= bit;
            cpu->state.msrReadMask |= bit; cpu->state.msrWriteMask |= bit;
        } else {
            cpu->state.managedMask &= ~bit;
            cpu->state.msrReadMask &= ~bit; cpu->state.msrWriteMask &= ~bit;
        }
        cpu->state.publish(cpu->rw, cpu->roFp, cpu->rwFp); publishMsrs(vm, cpu);
        return 0;
    }
    if (index == 10) {
        unsigned id = compactMsrId(static_cast<uint32_t>(arg));
        uint64_t access = arg >> 32;
        if (!id || access > 3) return HV_UNSUPPORTED;
        uint64_t bit = 1ull << id;
        if (access && !(cpu->state.managedMask & bit)) return HV_UNSUPPORTED;
        if (access & 1) cpu->state.msrReadMask |= bit; else cpu->state.msrReadMask &= ~bit;
        if (access & 2) cpu->state.msrWriteMask |= bit; else cpu->state.msrWriteMask &= ~bit;
        return 0;
    }
    if (index == 6) return arg == 0 ? 0 : HV_UNSUPPORTED;
    if (index == 2) { cpu->svm.requestFlush(); return 0; } // hv_vcpu_invalidate_tlb
    if (index == 7) {
        if (!arg || arg >= 92) return HV_UNSUPPORTED;
        cpu->state.pull(cpu->rw, cpu->rwFp); cpu->state.publish(cpu->rw, cpu->roFp, cpu->rwFp); publishMsrs(vm, cpu); return 0;
    }
    if (index == 11) { cpu->ro[0xe8] = 1; return 0; }
    if (index == 1 && arg != ~0ull) return HV_UNSUPPORTED; // no preemption timer advertised
    if (index == 0 || index == 1) {
        ++cpu->runCalls;
        cpu->state.pull(cpu->rw, cpu->rwFp);
        auto &requests = interruptSlots[slotForTask(vm->task)].requests;
        kern_return_t result;
        // hv_vcpu_run_until(forever): like AppleHV, a host interrupt is not a
        // reason to visit the VMM. Host state is fully restored and interrupts
        // are enabled between quanta, so the IRQ has been serviced and urgent
        // preemption has happened; continue the guest directly unless someone
        // needs this thread: a cancellation (tested first thing in run()), an
        // AST that must reach the user boundary, or a waiting gate writer.
        for (;;) {
            result = cpu->svm.run(cpu->state, vm->memory, requests, cpu->id);
            if (result != 0 || index != 1 || !cpu->svm.hostOnlyExit() || !api.ast_pending) break;
            if (__atomic_load_n(&writersWaiting,__ATOMIC_SEQ_CST)) break;
            const boolean_t interrupts = ml_set_interrupts_enabled(false);
            const int ast = api.ast_pending(); // per-CPU data: read without migrating
            ml_set_interrupts_enabled(interrupts);
            if (ast) break;
            ++cpu->continued;
        }
        cpu->state.publish(cpu->rw, cpu->roFp, cpu->rwFp); publishMsrs(vm, cpu);
        *reinterpret_cast<uint32_t *>(cpu->ro + 0x20) = result == 0 ? 1 : 0;
        return result;
    }
    return HV_UNSUPPORTED;
}
static kern_return_t create(uint64_t arg) {
    task_t task = current_task();
    if (!entitled(task)) return Denied;
    abi::VmCreate in = {};
    if (copyin(arg, &in, sizeof(in))) return BadAddress;
    if (in.flags != 0) return HV_UNSUPPORTED;
    if (slotForTask(task) >= 0 || api.get_task_target()) return HV_ERROR;
    unsigned index = 0;
    while (index < MaxVms && vms[index]) ++index;
    if (index == MaxVms) return NoResources;
    Vm *vm = new Vm;
    if (!vm) return NoResources;
    vm->task = task;
    if (!vm->memory.init() || !vm->rw.alloc(task, abi::RwBytes, false) ||
        !vm->ro.alloc(task, abi::RoBytes, true) ||
        !vm->clock.alloc(task, PAGE_SIZE, true)) {
        vm->release(); delete vm; return NoResources;
    }
    // Sequence 1 publishes the clock tuple (0,0): framework clock reads raw host TSC.
    *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(vm->clock.memory->getBytesNoCopy()) + 0x10) = 1;
    abi::Interface out = {};
    out.rwFpOffset = abi::RwFpOffset; out.roFpOffset = abi::RoFpOffset;
    out.fpStride = abi::FpStride; out.roMsrOffset = abi::RoMsrOffset;
    out.msrStride = abi::MsrStride;
    out.rw = vm->rw.mapping->getAddress();
    out.ro = vm->ro.mapping->getAddress();
    out.clock = vm->clock.mapping->getAddress();
    if (copyout(&out, in.interfaceOut, sizeof(out))) {
        vm->release(); delete vm; return BadAddress;
    }
    // Publish only after every allocation, mapping and copyout succeeds.
    vms[index] = vm;
    lck_mtx_lock(IOLockGetMachLock(interruptGate));
    interruptSlots[index].requests.reset();
    interruptSlots[index].task = task;
    lck_mtx_unlock(IOLockGetMachLock(interruptGate));
    api.set_task_target(vm);
    klog("vm: create pid=%d live=%u", proc_selfpid(), countLocked());
    return HV_SUCCESS;
}
static kern_return_t destroy() {
    int index = slotForTask(current_task());
    if (index < 0) return NoDevice;
    if (vms[index]->cpuCount) return HV_ERROR;
    api.set_task_target(nullptr);
    dispose(static_cast<unsigned>(index), "destroy");
    return HV_SUCCESS;
}
} // namespace

bool vmBackendInit(const KernelApi &resolved) {
    if (!gate) gate = IORWLockAlloc();
    if (!interruptGate) interruptGate = IOLockAlloc();
    if (!gate || !interruptGate) return false;
    api = resolved;
    hostCapsInit();
    return true;
}
void vmBackendEnable() { Guard lock; accepting = true; }
bool vmBackendQuiesce() {
    Guard lock;
    if (countLocked()) return false;
    accepting = false;
    return true;
}
unsigned vmBackendCount() {
    if (!gate) return 0;
    Guard lock(true); return countLocked();
}
bool parallelRunEnabled() { return __atomic_load_n(&parallelRun,__ATOMIC_ACQUIRE); }
void setParallelRunEnabled(bool enabled) { __atomic_store_n(&parallelRun,enabled,__ATOMIC_RELEASE); }
HvCallbacks vmBackendCallbacks() {
    HvCallbacks callbacks = {};
    callbacks.task_destroy = taskDestroyed;
    callbacks.thread_destroy = threadDestroyed;
    callbacks.dispatch = scheduled; callbacks.preempt = scheduled;
    callbacks.volatile_state = volatileState;
    callbacks.suspend = systemSuspend; callbacks.resume = systemResume;
    callbacks.memory_pressure = memoryPressure;
    return callbacks;
}
kern_return_t vmBackendTrap(int type, unsigned index, uint64_t arg) {
    if (!gate || !interruptGate) return NoDevice;
    if (type == HV_TASK_TRAP && index == 13) return interruptTask(arg);
    // A shared lifetime/mapping lease permits separate owning vCPU threads to
    // execute simultaneously. Every other trap/callback is an exclusive writer.
    // Keep step mode exclusive: its software event transaction writes guest PTEs.
    // Lookup, pull, run and publish all stay within the lease; no raw target is
    // carried across an unlock. The opt-in flag cannot change with live VMs.
    Guard lock(type==HV_THREAD_TRAP && (index==0 || index==1) &&
        parallelRunEnabled() && fastRunEnabled());
    if (!accepting) return NoDevice;
    // Never dereference the trap's target: a parallel destroy may have invalidated it.
    if (type == HV_TASK_TRAP && index == 0) return create(arg);
    if (type == HV_TASK_TRAP && index == 1) return destroy();
    int slot = slotForTask(current_task());
    if (slot < 0 || vms[slot]->taskGone) return NoDevice;
    if (type == HV_THREAD_TRAP) return cpuTrap(vms[slot], index, arg);
    if (index == 12) return createCpu(vms[slot], arg);
    if (type == HV_TASK_TRAP && index >= 4 && index <= 6) {
        abi::Map in = {};
        if (copyin(arg, &in, sizeof(in))) return BadAddress;
        auto &memory = vms[slot]->memory;
        if (index == 4) return memory.map(current_task(), in);
        if (index == 5) return memory.unmap(in);
        return memory.protect(in);
    }
    return HV_UNSUPPORTED;
}
} // namespace amdhv
