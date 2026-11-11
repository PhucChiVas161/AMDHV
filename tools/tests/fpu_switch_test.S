#include "../../AMDHV/src/fpu_switch.inc"
#ifdef AMDHV_TEST_EXTENDED_FP
#define FP_STASH 4096
#else
#define FP_STASH 1024
#endif
// void roundtrip(guest, scratch[2048], before[832], after[832]); AVX host required.
.text
#ifdef AMDHV_TEST_RUN_TRAP
#define _fpu_roundtrip_test _fpu_run_preserved
#endif
.globl _fpu_roundtrip_test
_fpu_roundtrip_test:
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rdi, %rbx
    mov %rsi, %r12
    mov %rdx, %r13
    mov %rcx, %r14
    mov $7, %eax
    xor %edx, %edx
    xsave64 FP_STASH(%r12)
#ifdef AMDHV_TEST_FP_INIT
    movq $0, 512(%r12)
    movl $0x1f80, 24(%r12)
    mov $7, %eax
    xor %edx, %edx
    xrstor64 (%r12)
#else
    fninit
    fld1
    fldpi
    vmovups Lpattern(%rip), %ymm15
#endif
    mov $7, %eax
    xor %edx, %edx
    xsave64 (%r13)
#ifdef AMDHV_TEST_RUN_TRAP
    // Exact hv_thread_trap(0) used by this build of Hypervisor.framework.
    mov $0x3000001, %eax
    xor %edi, %edi
    xor %esi, %esi
    syscall
    mov %eax, %r15d
#else
    xor %r15d, %r15d
#ifdef AMDHV_TEST_EXTENDED_FP
    AMDHV_XSAVE_ENTER %rbx, %r12
#else
    AMDHV_FP_ENTER %rbx, %r12
#endif
    fninit
    fld1
    fld1
    faddp %st, %st(1)
    pxor %xmm0, %xmm0
    pcmpeqb %xmm15, %xmm15
#ifdef AMDHV_TEST_EXTENDED_FP
    vpcmpeqb %ymm15, %ymm15, %ymm15
    AMDHV_XSAVE_LEAVE %rbx, %r12
#else
    AMDHV_FP_LEAVE %rbx, %r12
#endif
#endif
    mov $7, %eax
    xor %edx, %edx
    xsave64 (%r14)
    mov $7, %eax
    xor %edx, %edx
    xrstor64 FP_STASH(%r12)
    mov %r15d, %eax
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    ret
.const
.p2align 5
Lpattern:
    .quad 0x0123456789abcdef,0xfedcba9876543210,0xaabbccdd55667788,0x9988776655443322
