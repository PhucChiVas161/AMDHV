// KVM reference probe for AMDHV guest-interrupt semantics on real AMD SVM.
// Build: gcc -O2 -Wall -o kvmref kvmref.c   Run: ./kvmref   (needs /dev/kvm rw, no root)
// No in-kernel irqchip: userspace owns IRQ injection, like the Apple VMM over Hypervisor.framework.
#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define MEM 0x200000
#define CODE 0x10000
#define D(o) (*(volatile uint64_t *)(mem + 0x6000 + (o)))

// Guest code, position independent; data at fixed GPA 0x6000 (identity mapped).
// D+0x00 irq rip, +0x08 irq rflags, +0x10 irq count, +0x20 gp err, +0x28 gp rip,
// +0x30 ud rip, +0x40 nmi rip, +0xf8 db count, +0x100.. db rips.
asm(".pushsection .rodata\n.code64\n.globl g_start, g_end\ng_start:\n"
    "h_irq: push %rax; mov 8(%rsp),%rax; mov %rax,0x6000; mov 24(%rsp),%rax; mov %rax,0x6008;"
    "  incq 0x6010; pop %rax; iretq\n"
    "h_gp: push %rax; mov 8(%rsp),%rax; mov %rax,0x6020; mov 16(%rsp),%rax; mov %rax,0x6028;"
    "  pop %rax; add $8,%rsp; iretq\n"
    "h_ud: push %rax; mov 8(%rsp),%rax; mov %rax,0x6030; pop %rax; iretq\n"
    "h_nmi: push %rax; mov 8(%rsp),%rax; mov %rax,0x6040; pop %rax; nmi_hlt: hlt; iretq\n"
    "h_db: push %rax; push %rbx; mov 0x60f8,%rbx; mov 16(%rsp),%rax; mov %rax,0x6100(,%rbx,8);"
    "  inc %rbx; mov %rbx,0x60f8; cmp $32,%rbx; jb 1f; andq $~0x100,32(%rsp); 1: pop %rbx; pop %rax; iretq\n"
    // T1: IRQ window after STI shadow
    "t_sti: cli; sti; sti_a: nop; sti_b: nop; hlt\n"
    // T2: guest-owned TF trace across STI and MOV SS (pure hardware, KVM does not step)
    "t_tf: cli; pushfq; orq $0x100,(%rsp); popfq; tf_0: nop; tf_1: sti; tf_2: nop; tf_3: mov %ss,%ax;"
    "  tf_4: mov %ax,%ss; tf_5: nop; tf_6: nop; tf_7: pushfq; tf_8: andq $~0x100,(%rsp); tf_9: popfq; tf_10: hlt\n"
    // T3: KVM_GUESTDBG_SINGLESTEP across the same instructions
    "t_step: cli; sti; st_a: nop; st_b: mov %ss,%ax; st_c: mov %ax,%ss; st_d: nop; st_e: nop; st_f: hlt\n"
    // T4: HLT with IF=1, then IRQ wakes it
    "t_hlt: sti; hlt; hlt_e: hlt\n"
    // T5/T6/T7: injection targets
    "t_inj: hlt\n"
    "g_end:\n.popsection");

#define SYM(n) extern const uint8_t n[]
SYM(g_start); SYM(g_end); SYM(h_irq); SYM(h_gp); SYM(h_ud); SYM(h_nmi); SYM(h_db); SYM(nmi_hlt);
SYM(t_sti); SYM(sti_a); SYM(sti_b); SYM(t_tf); SYM(tf_0); SYM(tf_1); SYM(tf_2); SYM(tf_3); SYM(tf_4);
SYM(tf_5); SYM(tf_6); SYM(tf_7); SYM(tf_8); SYM(tf_9); SYM(tf_10); SYM(t_step); SYM(st_a); SYM(st_b);
SYM(st_c); SYM(st_d); SYM(st_e); SYM(st_f); SYM(t_hlt); SYM(hlt_e); SYM(t_inj);
#define GPA(s) ((uint64_t)CODE + (uint64_t)((s) - g_start))

static int kvm, vm, vcpu, fails;
static uint8_t *mem;
static struct kvm_run *run;

static void die(const char *m) { perror(m); exit(2); }
static void check(int ok, const char *name, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void check(int ok, const char *name, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("%s %-28s ", ok ? "PASS" : "FAIL", name); vprintf(fmt, ap); printf("\n");
    va_end(ap); fails += !ok;
}
static struct kvm_regs regs(void) { struct kvm_regs r; if (ioctl(vcpu, KVM_GET_REGS, &r)) die("GET_REGS"); return r; }
static struct kvm_vcpu_events events(void) {
    struct kvm_vcpu_events e; if (ioctl(vcpu, KVM_GET_VCPU_EVENTS, &e)) die("GET_EVENTS"); return e;
}
static void set_events(struct kvm_vcpu_events *e) { if (ioctl(vcpu, KVM_SET_VCPU_EVENTS, e)) die("SET_EVENTS"); }
static int krun(void) { if (ioctl(vcpu, KVM_RUN, 0) && errno != EINTR) die("KVM_RUN"); return run->exit_reason; }
static void start(const uint8_t *entry, uint64_t rflags) {
    struct kvm_regs r = {0};
    r.rip = GPA(entry); r.rsp = 0x80000; r.rflags = rflags | 2;
    if (ioctl(vcpu, KVM_SET_REGS, &r)) die("SET_REGS");
    memset(mem + 0x6000, 0, 0x1000);
    run->request_interrupt_window = 0;
}
static void idt(int vec, const uint8_t *h) {
    uint64_t a = GPA(h); uint8_t *g = mem + 0x5000 + vec * 16;
    memset(g, 0, 16);
    g[0] = a; g[1] = a >> 8; g[2] = 8; g[5] = 0x8e; g[6] = a >> 16; g[7] = a >> 24;
    memcpy(g + 8, (uint32_t[]){(uint32_t)(a >> 32)}, 4);
}

static void dump_cpuid(void) {
    unsigned a, b, c, d;
    __cpuid(0, a, b, c, d); printf("cpuid 0x0        max=%#x vendor=%.4s%.4s%.4s\n", a, (char *)&b, (char *)&d, (char *)&c);
    __cpuid(1, a, b, c, d); printf("cpuid 0x1        eax=%08x ebx=%08x ecx=%08x edx=%08x (family/model/stepping in eax)\n", a, b, c, d);
    __cpuid_count(7, 0, a, b, c, d); printf("cpuid 0x7.0      eax=%08x ebx=%08x ecx=%08x edx=%08x\n", a, b, c, d);
    __cpuid(0x80000001, a, b, c, d); printf("cpuid 0x80000001 eax=%08x ebx=%08x ecx=%08x edx=%08x (SVM=ecx bit2: %u)\n", a, b, c, d, (c >> 2) & 1);
    __cpuid(0x8000000a, a, b, c, d); printf("cpuid 0x8000000a eax=%08x ebx=%08x(ASIDs=%u) ecx=%08x edx=%08x\n", a, b, b, c, d);
    static const char *bits[32] = {"NP","LbrVirt","SVML","NRIPS","TscRate","VmcbClean","FlushByAsid","DecodeAssists",
        0,0,"PauseFilter",0,"PauseFilterThreshold","AVIC",0,"VMSAVEvirt","VGIF","GMET","x2AVIC","SSSCheck",
        "SpecCtrl","ROGPT",0,"HOST_MCE_OVERRIDE","TlbiCtl","VNMI","IbsVirt","ExtLvtAvicAccessChg","NestedVirtVmcbAddrChk","BusLockThreshold","IdleHltIntercept",0};
    printf("svm edx features:");
    for (int i = 0; i < 32; i++) if (d >> i & 1) printf(" %s", bits[i] ? bits[i] : "?");
    printf("\n");
}

int main(int argc, char **argv) {
    dump_cpuid();
    if (argc > 1 && !strcmp(argv[1], "cpuid")) return 0;
    if ((kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC)) < 0) die("/dev/kvm");
    printf("KVM_GET_API_VERSION=%d\n", ioctl(kvm, KVM_GET_API_VERSION, 0));
    if ((vm = ioctl(kvm, KVM_CREATE_VM, 0)) < 0) die("CREATE_VM");
    mem = mmap(0, MEM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    struct kvm_userspace_memory_region reg = {.slot = 0, .guest_phys_addr = 0, .memory_size = MEM, .userspace_addr = (uint64_t)mem};
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &reg)) die("SET_MEM");
    if ((vcpu = ioctl(vm, KVM_CREATE_VCPU, 0)) < 0) die("CREATE_VCPU");
    int sz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    run = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);

    // Identity map first 2 MiB with one large page; 64-bit GDT/IDT like DxeCore state.
    ((uint64_t *)(mem + 0x1000))[0] = 0x2003;
    ((uint64_t *)(mem + 0x2000))[0] = 0x3003;
    ((uint64_t *)(mem + 0x3000))[0] = 0x83;
    uint64_t gdt[] = {0, 0x00af9b000000ffffull, 0x00cf93000000ffffull};
    memcpy(mem + 0x4000, gdt, sizeof gdt);
    memcpy(mem + CODE, g_start, g_end - g_start);
    idt(1, h_db); idt(2, h_nmi); idt(6, h_ud); idt(13, h_gp); idt(0x40, h_irq);

    // Guest CPUID first: KVM rejects EFER.NXE until guest CPUID advertises NX.
    struct kvm_cpuid2 *cp = calloc(1, sizeof *cp + 100 * sizeof(struct kvm_cpuid_entry2));
    cp->nent = 100;
    if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, cp) || ioctl(vcpu, KVM_SET_CPUID2, cp)) die("CPUID2");
    struct kvm_sregs s;
    if (ioctl(vcpu, KVM_GET_SREGS, &s)) die("GET_SREGS");
    struct kvm_segment cs = {.base = 0, .limit = 0xffffffff, .selector = 8, .type = 11, .present = 1, .s = 1, .l = 1, .g = 1};
    struct kvm_segment ds = {.base = 0, .limit = 0xffffffff, .selector = 0x10, .type = 3, .present = 1, .s = 1, .db = 1, .g = 1};
    s.cs = cs; s.ds = s.es = s.fs = s.gs = s.ss = ds;
    s.gdt.base = 0x4000; s.gdt.limit = sizeof gdt - 1; s.idt.base = 0x5000; s.idt.limit = 0xfff;
    s.cr0 = 0x80010033; s.cr3 = 0x1000; s.cr4 = 0x20; s.efer = 0xd00; // CR0/EFER match the native blocker
    if (ioctl(vcpu, KVM_SET_SREGS, &s)) die("SET_SREGS");

    // T1: request an IRQ window starting with IF=0; window must open after the STI shadow.
    start(t_sti, 0);
    run->request_interrupt_window = 1;
    int ex = krun();
    struct kvm_regs r = regs();
    check(ex == KVM_EXIT_IRQ_WINDOW_OPEN && run->ready_for_interrupt_injection && r.rip == GPA(sti_b),
          "irq-window-after-sti", "exit=%d ready=%u rip=%#llx expect sti_b=%#lx (shadow covers sti_a)",
          ex, run->ready_for_interrupt_injection, r.rip, GPA(sti_b));
    struct kvm_interrupt irq = {.irq = 0x40};
    if (ioctl(vcpu, KVM_INTERRUPT, &irq)) die("KVM_INTERRUPT");
    run->request_interrupt_window = 0;
    ex = krun();
    check(ex == KVM_EXIT_HLT && D(0x10) == 1 && D(0) == GPA(sti_b) && (D(8) & 0x200),
          "irq-inject-frame", "exit=%d count=%llu frame.rip=%#llx frame.rflags=%#llx",
          ex, (unsigned long long)D(0x10), (unsigned long long)D(0), (unsigned long long)D(8));

    // T2: guest TF trace. Records every RIP where the #DB trap was delivered.
    start(t_tf, 0);
    ex = krun();
    const uint8_t *tf[] = {tf_0, tf_1, tf_2, tf_3, tf_4, tf_5, tf_6, tf_7, tf_8, tf_9, tf_10};
    printf("INFO tf-trace (exit=%d, n=%llu):", ex, (unsigned long long)D(0xf8));
    for (unsigned i = 0; i < D(0xf8) && i < 32; i++) {
        uint64_t rip = D(0x100 + 8 * i); int k = -1;
        for (int j = 0; j < 11; j++) if (GPA(tf[j]) == rip) k = j;
        printf(" tf_%d", k);
    }
    printf("\n     (#DB lands at tf_N = before executing tf_N; a missing tf_N => trap after tf_N-1 suppressed)\n");
    int saw[11] = {0};
    for (unsigned i = 0; i < D(0xf8) && i < 32; i++) for (int j = 0; j < 11; j++) if (GPA(tf[j]) == D(0x100 + 8 * i)) saw[j] = 1;
    check(!saw[5] && saw[6], "tf-mov-ss-suppresses-db", "trap after MOV SS (at tf_5) seen=%d, at tf_6 seen=%d", saw[5], saw[6]);
    check(1, "tf-after-sti(observed)", "trap after STI (at tf_2) seen=%d  <- AMD Zen5 hardware behavior", saw[2]);

    // T3: KVM single-step: shadow reported at each step.
    start(t_step, 0);
    struct kvm_guest_debug dbg = {.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP};
    if (ioctl(vcpu, KVM_SET_GUEST_DEBUG, &dbg)) die("GUEST_DEBUG");
    const uint8_t *st[] = {t_step, st_a, st_b, st_c, st_d, st_e, st_f};
    printf("INFO kvm-singlestep:");
    int shadow_a = -1, shadow_d = -1, steps = 0;
    for (; steps < 16; steps++) {
        ex = krun();
        if (ex != KVM_EXIT_DEBUG) break;
        r = regs(); struct kvm_vcpu_events e = events(); int k = -1;
        for (int j = 0; j < 7; j++) if (GPA(st[j]) == r.rip) k = j;
        printf(" [%c if=%d sh=%u]", k >= 0 ? "0abcdef"[k] : '?', (int)(r.rflags >> 9 & 1), e.interrupt.shadow);
        if (k == 1) shadow_a = e.interrupt.shadow;
        if (k == 4) shadow_d = e.interrupt.shadow;
        if (k == 6) break;
    }
    printf("\n");
    dbg.control = 0; ioctl(vcpu, KVM_SET_GUEST_DEBUG, &dbg);
    check(shadow_a > 0, "shadow-after-sti", "events.interrupt.shadow at st_a=%d (KVM: 1=MOV_SS 2=STI 3=SVM-undistinguished)", shadow_a);
    check(1, "shadow-after-mov-ss(obs)", "at st_d=%d (-1 => step not stopped there: #DB suppressed by MOV SS)", shadow_d);

    // T4: HLT with IF=1 exits to userspace; IRQ wakes it and returns after HLT.
    start(t_hlt, 0);
    ex = krun(); r = regs();
    check(ex == KVM_EXIT_HLT && r.rip == GPA(hlt_e) && (r.rflags & 0x200), "hlt-if1-exit",
          "exit=%d rip=%#llx (after hlt) if=%d", ex, r.rip, (int)(r.rflags >> 9 & 1));
    if (ioctl(vcpu, KVM_INTERRUPT, &irq)) die("KVM_INTERRUPT");
    ex = krun();
    check(ex == KVM_EXIT_HLT && D(0) == GPA(hlt_e), "hlt-wakeup-irq", "frame.rip=%#llx expect %#lx",
          (unsigned long long)D(0), GPA(hlt_e));

    // T5: #GP injection with error code (VMX 0x4016 type 3 + 0x4018).
    start(t_inj, 0);
    struct kvm_vcpu_events e = events();
    e.exception.injected = 1; e.exception.nr = 13; e.exception.has_error_code = 1; e.exception.error_code = 0x1234;
    e.flags = 0; set_events(&e);
    ex = krun();
    check(ex == KVM_EXIT_HLT && D(0x20) == 0x1234 && D(0x28) == GPA(t_inj), "inject-gp-errcode",
          "err=%#llx frame.rip=%#llx", (unsigned long long)D(0x20), (unsigned long long)D(0x28));

    // T6: #UD injection, no error code.
    start(t_inj, 0);
    e = events(); e.exception.injected = 1; e.exception.nr = 6; e.exception.has_error_code = 0; e.flags = 0; set_events(&e);
    ex = krun();
    check(ex == KVM_EXIT_HLT && D(0x30) == GPA(t_inj), "inject-ud", "frame.rip=%#llx", (unsigned long long)D(0x30));

    // T7: NMI: blocked inside handler until IRETQ (VMX 0x4824 bit3).
    start(t_inj, 0x200);
    if (ioctl(vcpu, KVM_NMI)) die("KVM_NMI");
    ex = krun(); r = regs(); e = events();
    int in_masked = e.nmi.masked;
    check(ex == KVM_EXIT_HLT && r.rip == GPA(nmi_hlt) + 1 && in_masked, "nmi-blocked-in-handler",
          "exit=%d rip=%#llx nmi.masked=%u", ex, r.rip, in_masked);
    ex = krun(); e = events();
    check(ex == KVM_EXIT_HLT && D(0x40) == GPA(t_inj) && !e.nmi.masked, "nmi-unblocked-after-iret",
          "frame.rip=%#llx nmi.masked=%u", (unsigned long long)D(0x40), e.nmi.masked);

    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
