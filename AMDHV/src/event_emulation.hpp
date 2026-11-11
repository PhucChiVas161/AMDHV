#pragma once
#include "vcpu_state.hpp"
#include "cr_policy.hpp"
#include "flags_step.hpp"
namespace amdhv {
inline bool eventCode64(const VcpuState &s) {
    return pagingEfer(s.get(0x2806))==0x500 && (s.get(0x4816)&0x2000) &&
        (s.get(0x6800)&0x80000001ull)==0x80000001ull;
}
inline bool validExternalEntry(const VcpuState &s) {
    uint64_t info=s.get(0x4016);
    if(!info)return true;
    return eventCode64(s) && (info&~0xffull)==0x80000000ull && (info&255)>=32 &&
        (s.get(0x6820)&0x200) && !(s.get(0x4824)&~8ull);
}
// Fixed-size transaction for this ring-0, same-stack subset. No host pointer
// derived from a guest address. No writes at all until all translations validate.
struct EventMemoryPlan {
    struct Update { uint64_t pa,value; } updates[32];
    struct Store { uint64_t pa; uint8_t value; } stores[40];
    unsigned count=0,stored=0;
    bool accessed(uint64_t pa,uint64_t value,uint64_t bits) {
        if((value&bits)==bits)return true;
        for(unsigned i=0;i<count;++i)if(updates[i].pa==pa) {updates[i].value|=bits;return true;}
        if(count==32)return false;
        updates[count++]={pa,value|bits};return true;
    }
    template<class Memory> bool translate(Memory &m,const VcpuState &s,uint64_t linear,bool write,uint64_t &pa) {
        if(!validManagedMsr(6,linear))return false;
        uint64_t table=s.get(0x6802);bool writable=true;
        for(int level=3;level>=0;--level) {
            unsigned shift=12+9*unsigned(level);
            uint64_t address=table+((linear>>shift)&511)*8,e=0;
            if(!m.readPhysical(address,&e,8) || !(e&1) || (e&0x7fff000000000000ull) ||
                ((e>>63) && !(s.get(0x2806)&0x800)))return false;
            writable &= (e&2)!=0;
            bool leaf=level==0 || ((e&128) && level<3);
            if((e&128) && level==3)return false;
            if(leaf && level && (e&(((1ull<<shift)-1)&~0x1fffull)))return false;
            if(write && leaf && !writable && (s.get(0x6800)&0x10000))return false;
            if(!accessed(address,e,32|((write&&leaf)?64:0)))return false;
            table=e&0x0000fffffffff000ull;
            if(leaf) {
                pa=(table&~((1ull<<(level?shift:12))-1)) | (linear&((1ull<<(level?shift:12))-1));
                return true;
            }
        }
        return false;
    }
    template<class Memory> bool read(Memory &m,const VcpuState &s,uint64_t linear,void *out,unsigned size) {
        if(!size || linear>~0ull-(size-1))return false;
        auto b=static_cast<uint8_t *>(out);
        for(unsigned i=0;i<size;++i) {uint64_t pa;
            if(!translate(m,s,linear+i,false,pa) || !m.readPhysical(pa,b+i,1))return false;
        }
        return true;
    }
    template<class Memory> bool frame(Memory &m,const VcpuState &s,uint64_t linear,const uint64_t *data) {
        auto b=reinterpret_cast<const uint8_t *>(data);
        if(stored || linear>~0ull-39)return false;
        for(unsigned i=0;i<40;++i) {uint64_t pa;
            if(!translate(m,s,linear+i,true,pa) || !m.writePhysical(pa,nullptr,1,true))return false;
            stores[stored++]={pa,b[i]};
        }
        return true;
    }
    template<class Memory> bool commit(Memory &m) {
        for(unsigned i=0;i<count;++i)if(!m.writePhysical(updates[i].pa,nullptr,8,true))return false;
        for(unsigned i=0;i<stored;++i)if(!m.writePhysical(stores[i].pa,nullptr,1,true))return false;
        for(unsigned i=0;i<count;++i)if(!m.writePhysical(updates[i].pa,&updates[i].value,8))return false;
        for(unsigned i=0;i<stored;++i)if(!m.writePhysical(stores[i].pa,&stores[i].value,1))return false;
        return true;
    }
};
template<class Memory> bool sameRingCode(Memory &m,const VcpuState &s,EventMemoryPlan &p,uint64_t selector) {
    if(selector!=s.get(0x802) || !selector || (selector&7) || selector>s.get(0x4810) ||
       s.get(0x4810)-selector<7 || s.get(0x6816)>~0ull-selector)return false;
    uint64_t descriptor=0;
    return p.read(m,s,s.get(0x6816)+selector,&descriptor,8) && descriptor==0x00af9b000000ffffull;
}
template<class Memory> bool emulateExternalEntry(Memory &m,VcpuState &s) {
    if(!s.get(0x4016) || !validExternalEntry(s) || (s.get(0x4824)&8))return false;
    uint64_t offset=(s.get(0x4016)&255)*16,base=s.get(0x6818);
    if(offset>s.get(0x4812) || s.get(0x4812)-offset<15 || base>~0ull-offset)return false;
    EventMemoryPlan p;uint8_t gate[16]={};
    if(!p.read(m,s,base+offset,gate,16))return false;
    if(gate[4] || (gate[5]!=0x8e && gate[5]!=0x8f) || gate[12] || gate[13] || gate[14] || gate[15])return false;
    uint64_t selector=uint64_t(gate[2])|(uint64_t(gate[3])<<8);
    if(!sameRingCode(m,s,p,selector))return false;
    uint64_t handler=uint64_t(gate[0])|(uint64_t(gate[1])<<8)|(uint64_t(gate[6])<<16)|(uint64_t(gate[7])<<24);
    for(unsigned i=0;i<4;++i)handler|=uint64_t(gate[8+i])<<(32+i*8);
    if(!canonical48(handler))return false;
    uint64_t oldRsp=s.get(0x681c),aligned=oldRsp&~15ull;
    if(aligned<40 || !canonical48(oldRsp))return false;
    uint64_t frame[]={s.get(0x681e),s.get(0x802),s.get(0x6820),oldRsp,s.get(0x804)};
    if(!p.frame(m,s,aligned-40,frame) || !p.commit(m))return false;
    s.set(0x681c,aligned-40);s.set(0x681e,handler);
    if(gate[5]==0x8e)s.set(0x6820,s.get(0x6820)&~0x200ull);
    s.set(0x4016,0);s.set(0x4824,0);
    return true;
}
template<class Memory> bool emulateIret64(Memory &m,VcpuState &s) {
    if(!eventCode64(s) || s.get(0x4824))return false;
    EventMemoryPlan p;uint64_t frame[5]={};
    if(!p.read(m,s,s.get(0x681c),frame,sizeof(frame)) || !sameRingCode(m,s,p,frame[1]) ||
       frame[4]!=s.get(0x804) || !canonical48(frame[0]) || !canonical48(frame[3]) ||
       (frame[2]&~PilotFlags) || !(frame[2]&2) || !p.commit(m))return false;
    s.set(0x681e,frame[0]);s.set(0x681c,frame[3]);s.set(0x6820,frame[2]);s.set(0x4824,0);
    return true;
}
}
