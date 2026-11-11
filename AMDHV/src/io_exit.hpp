#pragma once
#include <stdint.h>
namespace amdhv {
struct IoExit { uint64_t qualification = 0, instructionInfo = 0; };
// Validate opcode/prefix bytes against SVM's authoritative IOIO metadata. No I/O
// instruction is executed here; userspace receives the unadvanced guest RIP.
inline bool translateIo(uint64_t svmInfo, const uint8_t *bytes, unsigned length,
                        unsigned codeBits, uint16_t dx, IoExit &out) {
    if (!length || length>15 || (codeBits!=16 && codeBits!=32 && codeBits!=64)) return false;
    unsigned i=0; bool operand=false,address=false,rep=false; unsigned segment=3;
    for (;i<length;++i) {
        uint8_t b=bytes[i];
        if(b==0x66) operand=true;
        else if(b==0x67) address=true;
        else if(b==0xf2 || b==0xf3) rep=true;
        else if(b==0x26) segment=0;
        else if(b==0x2e) segment=1;
        else if(b==0x36) segment=2;
        else if(b==0x3e) segment=3;
        else if(b==0x64) segment=4;
        else if(b==0x65) segment=5;
        else if(codeBits==64 && b>=0x40 && b<=0x4f) {}
        else break;
    }
    if(i==length) return false;
    uint8_t op=bytes[i++];
    bool immediate=op>=0xe4 && op<=0xe7;
    bool scalarDx=op>=0xec && op<=0xef;
    bool string=op>=0x6c && op<=0x6f;
    if(!immediate && !scalarDx && !string) return false;
    bool input=(op&2)==0;
    unsigned width=(op&1) ? ((codeBits==16) != operand ? 2 : 4) : 1;
    unsigned addressBits=address ? (codeBits==64?32:codeBits==32?16:32) : codeBits;
    unsigned addressCode=addressBits==16?0:addressBits==32?1:2;
    unsigned port=dx;
    if(immediate) {if(i==length) return false; port=bytes[i++];}
    if(i!=length || (svmInfo&1)!=input || ((svmInfo>>2)&1)!=string ||
        ((svmInfo>>4)&7)!=width || ((svmInfo>>7)&7)!=(1u<<addressCode) ||
        ((svmInfo>>16)&0xffff)!=port || ((svmInfo>>3)&1)!=(string && rep)) return false;
    if(input && string) segment=0; // INS always uses ES, regardless of segment prefix
    if(string && ((svmInfo>>10)&7)!=segment) return false;
    out.qualification=(width-1) | (uint64_t(input)<<3) | (uint64_t(string)<<4) |
        (uint64_t(string && rep)<<5) | (uint64_t(immediate)<<6) | (uint64_t(port)<<16);
    out.instructionInfo=string ? (uint64_t(addressCode)<<7) | (uint64_t(segment)<<15) : 0;
    return true;
}
}
