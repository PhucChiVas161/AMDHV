#include <cpuid.h>
#include <x86intrin.h>
#include <mach/mach_time.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
int main(){
 unsigned a,b,c,d;__cpuid(0x80000007,a,b,c,d);
 printf("physical CPUID.80000007.EDX=%08x invariantTSC=%u\n",d,(d>>8)&1);
 mach_timebase_info_data_t tb;mach_timebase_info(&tb);
 auto m0=mach_continuous_time();auto t0=__rdtsc();
 std::mutex lock;uint64_t last=0;std::atomic<unsigned> backwards{0};uint64_t apics=0;
 std::vector<std::thread> workers;
 for(unsigned n=0;n<24;++n)workers.emplace_back([&]{for(unsigned i=0;i<10000;++i){
  std::lock_guard<std::mutex> guard(lock);
  unsigned x,y,z,w;__cpuid(1,x,y,z,w);apics|=1ull<<((y>>24)&63);
  _mm_lfence();auto now=__rdtsc();_mm_lfence();if(now<last)++backwards;last=now;
 }});
 for(auto &w:workers)w.join();
 auto t1=__rdtsc();auto m1=mach_continuous_time();
 double ns=double(m1-m0)*tb.numer/tb.denom;
 printf("240000 serialized host samples; APIC-id bitmap=%llx backwards=%u measuredMHz=%.6f seconds=%.3f\n",apics,backwards.load(),double(t1-t0)*1000/ns,ns/1e9);
 puts("Diagnostic: mach time may itself derive from TSC; this is not an independent hardware reference or a guest SMP skew proof.");
 return backwards.load()?1:0;
}
