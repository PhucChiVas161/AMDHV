#include "vcpu_interrupt.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <atomic>
int main() {
    amdhv::VcpuInterrupts q;
    assert(q.request(0)); assert(!q.request(1));
    q.add(0); q.add(63);
    assert(!q.request(3)); assert(!q.consume(0)); // invalid batch has no partial effect
    assert(q.request(1ull << 63)); assert(!q.consume(0));
    assert(q.consume(63)); assert(!q.consume(63));
    assert(q.request(1)); assert(q.request(1)); assert(q.consume(0)); assert(!q.consume(0));
    assert(q.request(1)); q.remove(0); q.add(0); assert(!q.consume(0));
    // Producer and runner exchange pending bits using the actual kernel helper.
    std::atomic<unsigned> acknowledged{0};
    std::thread producer([&] {
        for (unsigned i = 1; i <= 10000; ++i) {
            assert(q.request(1));
            while (acknowledged.load() < i) std::this_thread::yield();
        }
    });
    for (unsigned i = 1; i <= 10000; ++i) {
        while (!q.consume(0)) std::this_thread::yield();
        acknowledged.store(i);
    }
    producer.join();
    assert(q.request(1ull << 63)); q.reset(); q.add(63); assert(!q.consume(63));
    // Kick targets: only vCPUs inside the entry loop report a physical CPU.
    q.add(0); q.add(5);
    assert(q.runningCpus(~0ull) == 0);
    q.enter(0, 3); q.enter(5, 23); q.enter(63, 3);
    assert(q.runningCpus(1) == (1ull << 3));
    assert(q.runningCpus((1ull << 5) | (1ull << 63)) == ((1ull << 23) | (1ull << 3)));
    assert(q.runningCpus(1ull << 7) == 0);
    q.leave(0); assert(q.runningCpus(1) == 0);
    q.enter(5, 64); assert(q.runningCpus(1ull << 5) == 0); // beyond the 64-bit kick mask
    q.reset(); assert(q.runningCpus(~0ull) == 0);
    // Dekker handshake: a request is either consumed before entry or sees the runner's CPU.
    q.add(0);
    for (unsigned round = 0; round < 20000; ++round) {
        std::atomic<bool> go{false}; bool consumed = false; uint64_t seen = 0;
        std::thread requester([&] { while (!go.load()) {} assert(q.request(1)); seen = q.runningCpus(1); });
        go.store(true);
        q.enter(0, 7); consumed = q.consume(0);
        requester.join();
        assert(consumed || seen == (1ull << 7));
        q.leave(0); q.clear(0);
    }
    puts("PASS: interrupt masks, atomic rejection, coalescing, cross-thread consumption, slot reuse and kick handshake");
}
