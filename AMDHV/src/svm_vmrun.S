#include "fpu_switch.inc"
// void amdhv_svm_vmrun(uint64_t vmcb_pa /* rdi */, svm::GuestGprs *gprs /* rsi */)
//
// Caller must run with interrupts disabled, EFER.SVME set and VM_HSAVE_PA programmed.
// VMRUN saves host RSP/RIP/RAX (and control/segment state) into the host save area and
// restores them on #VMEXIT. Other GPRs are not saved by hardware, so we do it here.
// Guest RAX and RSP live in the VMCB.

#define G_RBX 0x00
#define G_RCX 0x08
#define G_RDX 0x10
#define G_RSI 0x18
#define G_RDI 0x20
#define G_RBP 0x28
#define G_R8  0x30
#define G_R9  0x38
#define G_R10 0x40
#define G_R11 0x48
#define G_R12 0x50
#define G_R13 0x58
#define G_R14 0x60
#define G_R15 0x68

    .text
    .globl _amdhv_svm_vmrun
    .p2align 4
_amdhv_svm_vmrun:
    push    %rbp
    push    %rbx
    push    %r12
    push    %r13
    push    %r14
    push    %r15
    push    %rsi                    // keep gprs pointer at (%rsp)

    clgi                            // block interrupts/NMI/SMI until STGI

    mov     %rdi, %rax              // VMRUN takes the VMCB physical address in RAX

    mov     G_RBX(%rsi), %rbx
    mov     G_RCX(%rsi), %rcx
    mov     G_RDX(%rsi), %rdx
    mov     G_RDI(%rsi), %rdi
    mov     G_RBP(%rsi), %rbp
    mov     G_R8(%rsi),  %r8
    mov     G_R9(%rsi),  %r9
    mov     G_R10(%rsi), %r10
    mov     G_R11(%rsi), %r11
    mov     G_R12(%rsi), %r12
    mov     G_R13(%rsi), %r13
    mov     G_R14(%rsi), %r14
    mov     G_R15(%rsi), %r15
    mov     G_RSI(%rsi), %rsi

    vmrun   %rax

    // #VMEXIT: RSP/RIP are host values again, GIF = 0. RAX is free to use as scratch.
    mov     (%rsp), %rax
    mov     %rbx, G_RBX(%rax)
    mov     %rcx, G_RCX(%rax)
    mov     %rdx, G_RDX(%rax)
    mov     %rsi, G_RSI(%rax)
    mov     %rdi, G_RDI(%rax)
    mov     %rbp, G_RBP(%rax)
    mov     %r8,  G_R8(%rax)
    mov     %r9,  G_R9(%rax)
    mov     %r10, G_R10(%rax)
    mov     %r11, G_R11(%rax)
    mov     %r12, G_R12(%rax)
    mov     %r13, G_R13(%rax)
    mov     %r14, G_R14(%rax)
    mov     %r15, G_R15(%rax)

    stgi

    pop     %rsi
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbx
    pop     %rbp
    ret

// ---- Fast/step runner: one FP switch per quantum, one lean entry per VMRUN ----
//
// The caller keeps host interrupts disabled from amdhv_svm_fp_enter until
// amdhv_svm_fp_leave returns, and stays on one CPU. In between, guest x87/SSE/
// extended state and guest XCR0 stay loaded while the kext's own C code runs:
// that code is built -mkernel (no FP/SIMD) and cannot be interrupted, so no host
// code observes or disturbs guest FP state. CR0.TS stays clear for that window.

// void amdhv_svm_fp_enter(void *guestFP /* rdi */, void *hostXSAVE /* rsi */)
// Saves host user state into hostXSAVE and loads the guest's. The metadata
// slots at 4064/4072/4088 of hostXSAVE select legacy or extended switching.
    .globl _amdhv_svm_fp_enter
    .p2align 4
_amdhv_svm_fp_enter:
    clts
    AMDHV_FP_ENTER_MODE %rdi, %rsi
    ret

// void amdhv_svm_fp_leave(void *guestFP /* rdi */, void *hostXSAVE /* rsi */, uint64_t hostCr0 /* rdx */)
// Saves guest state, restores every host component and host XCR0, then the
// exact host CR0 (including a lazy TS) sampled before amdhv_svm_fp_enter.
    .globl _amdhv_svm_fp_leave
    .p2align 4
_amdhv_svm_fp_leave:
    mov     %rdx, %r11              // the FP macros clobber rax/rcx/rdx/r10
    clts
    AMDHV_FP_LEAVE_MODE %rdi, %rsi
    mov     %r11, %cr0
    ret

// Frame after the pushes below.
#define R_HOST_DR0 0
#define R_BREAK    8
#define R_GPRS     16
#define R_GUEST_PA 24
#define R_HOST_PA  32
#define RUN_HOST_IRQ  1             // host IF=1 across VMRUN: physical IRQs force #VMEXIT
#define RUN_SAVE_HOST 2             // VMSAVE host FS/GS/TR/LDTR/syscall MSRs (first entry of a quantum)
// void amdhv_svm_vmrun_guest(uint64_t guestPA /* rdi */, svm::GuestGprs *gprs /* rsi */,
//                            uint64_t hostPA /* rdx */, uint64_t breakpoint /* rcx */,
//                            uint64_t flags /* r8 */)
// Host VMSAVE state does not change within a quantum, so it is captured once.
// Host segments/MSRs are restored (VMLOAD) before GIF is set on every exit,
// because the C code between entries relies on the host GS base.
    .globl _amdhv_svm_vmrun_guest
    .p2align 4
_amdhv_svm_vmrun_guest:
    push    %rbp
    push    %rbx
    push    %r12
    push    %r13
    push    %r14
    push    %r15
    push    %rdx                    // host VMSAVE physical address
    push    %rdi                    // guest VMCB physical address
    push    %rsi                    // guest GPRs
    push    %rcx                    // optional next-instruction breakpoint
    xor     %eax, %eax
    test    %rcx, %rcx
    jz      Lrun_no_save_dr0
    mov     %dr0, %rax
Lrun_no_save_dr0:
    push    %rax                    // original host DR0 (only used for flags step)
    clgi
    // GIF remains clear: IF1 here cannot deliver a host interrupt. Keep STI
    // far from VMRUN so the host STI shadow cannot bleed into guest state.
    test    $RUN_HOST_IRQ, %r8b
    jz      Lrun_host_if_ready
    sti
    nop
Lrun_host_if_ready:
    test    %rcx, %rcx
    jz      Lrun_no_load_dr0
    mov     %rcx, %dr0
Lrun_no_load_dr0:
    test    $RUN_SAVE_HOST, %r8b
    jz      Lrun_host_saved
    mov     R_HOST_PA(%rsp), %rax
    vmsave  %rax
Lrun_host_saved:
    mov     R_GUEST_PA(%rsp), %rax
    vmload  %rax

    mov     G_RBX(%rsi), %rbx
    mov     G_RCX(%rsi), %rcx
    mov     G_RDX(%rsi), %rdx
    mov     G_RDI(%rsi), %rdi
    mov     G_RBP(%rsi), %rbp
    mov     G_R8(%rsi),  %r8
    mov     G_R9(%rsi),  %r9
    mov     G_R10(%rsi), %r10
    mov     G_R11(%rsi), %r11
    mov     G_R12(%rsi), %r12
    mov     G_R13(%rsi), %r13
    mov     G_R14(%rsi), %r14
    mov     G_R15(%rsi), %r15
    mov     G_RSI(%rsi), %rsi

    vmrun   %rax
    cli                              // GIF=0: mask IF before any host state is exposed

    // #VMEXIT: RSP/RIP are host values again, GIF = 0. RAX is free to use as scratch.
    mov     R_GPRS(%rsp), %rax
    mov     %rbx, G_RBX(%rax)
    mov     %rcx, G_RCX(%rax)
    mov     %rdx, G_RDX(%rax)
    mov     %rsi, G_RSI(%rax)
    mov     %rdi, G_RDI(%rax)
    mov     %rbp, G_RBP(%rax)
    mov     %r8,  G_R8(%rax)
    mov     %r9,  G_R9(%rax)
    mov     %r10, G_R10(%rax)
    mov     %r11, G_R11(%rax)
    mov     %r12, G_R12(%rax)
    mov     %r13, G_R13(%rax)
    mov     %r14, G_R14(%rax)
    mov     %r15, G_R15(%rax)

    mov     R_GUEST_PA(%rsp), %rax
    vmsave  %rax
    mov     R_HOST_PA(%rsp), %rax
    vmload  %rax
    pop     %r10                    // original host DR0
    pop     %r9                     // breakpoint argument
    test    %r9, %r9
    jz      Lrun_no_restore_dr0
    mov     %r10, %dr0
Lrun_no_restore_dr0:
    stgi

    pop     %rsi
    pop     %rdi
    pop     %rdx
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbx
    pop     %rbp
    ret
