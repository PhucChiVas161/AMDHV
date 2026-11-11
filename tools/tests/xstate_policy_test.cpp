#include "xstate_policy.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace amdhv;
int main(){
    static_assert(HostXcr0Offset>=XstateLimit && SwitchMaskOffset==HostXcr0Offset+8 && GuestXcr0Offset+8==4096);
    static_assert(HostXcr0Offset==4064 && SwitchMaskOffset==4072 && GuestXcr0Offset==4088); // assembly ABI
    for(uint64_t v:{1ull,3ull,7ull,0xe7ull,0x201ull,0x203ull,0x207ull,0x2e7ull})assert(validGuestXcr0(v));
    for(uint64_t v:{0ull,2ull,5ull,0x27ull,0x67ull,0xa7ull,0xe3ull,0x2e3ull,0x1001ull,1ull<<63})assert(!validGuestXcr0(v));
    assert(validGuestXcr0(7,7)&&!validGuestXcr0(0xe7,7)&&!validGuestXcr0(0x207,7));
    XstateComponent p[]={{2,256,576,0},{5,64,832,0},{6,512,896,0},{7,1024,1408,0},{9,8,2432,0}};
    assert(validXstateLayout(0x2e7,p,5)&&validXstateLayout(7,p,1));
    assert(!validXstateLayout(0x2e7,p,4)&&!validXstateLayout(7,p,5));
    for(unsigned which=0;which<5;++which){
        auto old=p[which];
        p[which].flags=1;assert(!validXstateLayout(0x2e7,p,5));p[which]=old;
        p[which].size=0;assert(!validXstateLayout(0x2e7,p,5));p[which]=old;
        p[which].offset=3968;assert(!validXstateLayout(0x2e7,p,5));p[which]=old;
        p[which].size=0xffffffff;assert(!validXstateLayout(0x2e7,p,5));p[which]=old;
        p[which].offset=575;assert(!validXstateLayout(0x2e7,p,5));p[which]=old;
    }
    auto old=p[4];p[4].offset=831;assert(!validXstateLayout(0x2e7,p,5));p[4]=old;
    p[4].id=7;assert(!validXstateLayout(0x2e7,p,5));p[4]=old;
    p[4].offset=3960;assert(validXstateLayout(0x2e7,p,5));
    p[4].size=9;assert(!validXstateLayout(0x2e7,p,5));
    puts("PASS: XCR0 dependencies/capability subset, standard XSAVE layout, overlaps, supervisor exclusion and metadata bounds");
}
