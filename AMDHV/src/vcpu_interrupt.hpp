#pragma once
#include <stdint.h>
namespace amdhv {
// Registry/liveness changes and request validation are externally serialized.
// The runner consumes only atomic bits, without taking a sleeping lock in SVM.
//
// Kick protocol (same shape as AppleHV's vcpu_interrupt/vcpu_kick): the runner
// publishes its physical CPU with a full barrier *before* testing its pending
// bit; the requester sets the pending bit with a full barrier *before* reading
// the published CPU. At least one side therefore observes the other, so a
// request either stops the next entry or yields a CPU to send a kick IPI to.
class VcpuInterrupts {
    uint64_t pending_ = 0;
    uint32_t running_[64] = {}; // physical CPU + 1; zero (static init) = not in guest
public:
    uint64_t live = 0;
    void reset() {
        live = 0; __atomic_store_n(&pending_, 0, __ATOMIC_RELEASE);
        for (auto &cpu : running_) __atomic_store_n(&cpu, 0u, __ATOMIC_RELEASE);
    }
    void add(unsigned id) { clear(id); live |= 1ull << id; }
    void remove(unsigned id) { live &= ~(1ull << id); clear(id); }
    void clear(unsigned id) { __atomic_fetch_and(&pending_, ~(1ull << id), __ATOMIC_ACQ_REL); }
    bool request(uint64_t mask) {
        if (mask & ~live) return false; // validate entire request before any mutation
        __atomic_fetch_or(&pending_, mask, __ATOMIC_SEQ_CST);
        return true;
    }
    bool consume(unsigned id) {
        uint64_t bit = 1ull << id;
        return (__atomic_fetch_and(&pending_, ~bit, __ATOMIC_SEQ_CST) & bit) != 0;
    }
    // Runner side, interrupts disabled: bracket the guest-entry loop.
    void enter(unsigned id, int cpu) {
        __atomic_exchange_n(&running_[id & 63], cpu >= 0 ? uint32_t(cpu) + 1 : 0u, __ATOMIC_SEQ_CST);
    }
    void leave(unsigned id) { __atomic_store_n(&running_[id & 63], 0u, __ATOMIC_SEQ_CST); }
    // Requester side, after request(): physical CPUs currently inside the
    // entry loop for any vCPU in `mask`. A stale answer only costs a spare IPI.
    uint64_t runningCpus(uint64_t mask) const {
        uint64_t cpus = 0;
        for (unsigned id = 0; id < 64; ++id) {
            if (!(mask & (1ull << id))) continue;
            const uint32_t cpu = __atomic_load_n(&running_[id], __ATOMIC_SEQ_CST);
            if (cpu && cpu <= 64) cpus |= 1ull << (cpu - 1);
        }
        return cpus;
    }
};
}
