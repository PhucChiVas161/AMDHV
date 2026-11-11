// Shared by the real transition and the userspace preservation test.
// Buffers are private, resident, 64-byte aligned; host >=576, guest >=512 bytes.
// Host CR0.EM/TS=0, CR4.OSXSAVE/OSFXSR=1, XCR0[1:0]=3, EFER.FFXSR=0.
// Only x87/MMX/SSE may execute in the guest (CR4.OSXSAVE is forbidden).
// XSAVE/XRSTOR mask 3 preserves host init-state tracking without touching AVX upper state.
.macro AMDHV_FP_ENTER guest, host
    mov $3, %eax
    xor %edx, %edx
    xsave64 (\host)
    fxrstor64 (\guest)
.endm

// Extended standard-format switch has a CPUID-validated mask/layout below3968.
// Data-transfer helpers are tested with the host-enabled mask in userspace;
// privileged XCR0 expansion/restoration stays in the GIF-disabled transition.
.macro AMDHV_XSAVE_ENTER guest, host
    AMDHV_XSAVE_PAIR \host, \guest, 4072(\host)
.endm
.macro AMDHV_XSAVE_LEAVE guest, host
    AMDHV_XSAVE_PAIR \guest, \host, 4072(\host)
.endm
// Data-only pair also exercised in userspace with a validated enabled mask.
.macro AMDHV_XSAVE_PAIR from, to, mask
    mov \mask, %eax
    xor %edx, %edx
    xsave64 (\from)
    mov \mask, %eax
    xrstor64 (\to)
.endm
.macro AMDHV_FP_ENTER_MODE guest, host
    cmpq $0, 4088(\host)
    je Lfp_enter_legacy\@
    // Preserve the pointer before ECX becomes the XCR index. Save every
    // selected physical component, including those disabled in host XCR0.
    mov \guest, %r10
    mov 4072(\host), %eax
    xor %edx, %edx
    xor %ecx, %ecx
    xsetbv
    AMDHV_XSAVE_PAIR \host, %r10, 4072(\host)
    mov 4088(\host), %eax
    xor %edx, %edx
    xor %ecx, %ecx
    xsetbv
    jmp Lfp_enter_done\@
Lfp_enter_legacy\@:
    AMDHV_FP_ENTER \guest, \host
Lfp_enter_done\@:
.endm
.macro AMDHV_FP_LEAVE_MODE guest, host
    cmpq $0, 4088(\host)
    je Lfp_leave_legacy\@
    // VMEXIT restores host CR4. Expand XCR0 only within GIF0 to save guest
    // state and restore all original physical host state (including PKRU).
    mov \guest, %r10
    mov 4072(\host), %eax
    xor %edx, %edx
    xor %ecx, %ecx
    xsetbv
    AMDHV_XSAVE_PAIR %r10, \host, 4072(\host)
    mov 4064(\host), %eax
    xor %edx, %edx
    xor %ecx, %ecx
    xsetbv
    jmp Lfp_leave_done\@
Lfp_leave_legacy\@:
    AMDHV_FP_LEAVE \guest, \host
Lfp_leave_done\@:
.endm
.macro AMDHV_FP_LEAVE guest, host
    fxsave64 (\guest)
    mov $3, %eax
    xor %edx, %edx
    xrstor64 (\host)
.endm
