#pragma once
#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <cstdio>
#include "../../AMDHV/src/segment_attributes.hpp"
// Explicit diagnostic mode only: exercise a pending cache serializer through
// public framework writes on the old kext. Never used by the normal gates.
inline bool normalizeDiagnosticSegments(hv_vcpuid_t cpu) {
    for(unsigned seg=0;seg<8;++seg){uint64_t ar=0,limit=0;
        if(hv_vmx_vcpu_read_vmcs(cpu,0x4814+seg*2,&ar)||
           hv_vmx_vcpu_read_vmcs(cpu,0x4800+seg*2,&limit))return false;
        auto normalized=amdhv::vmxSavedCacheAttributes(seg,amdhv::svmSegmentAttributes(ar),uint32_t(limit));
        if(normalized!=ar){
            printf("DIAGNOSTIC: segment%u AR %llx -> %llx via framework API\n",seg,ar,normalized);
            if(hv_vmx_vcpu_write_vmcs(cpu,0x4814+seg*2,normalized))return false;
        }
    }
    return true;
}
