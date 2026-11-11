# Giai đoạn 3: ABI ban đầu trên AppleHV 25G332

Nguồn: disassembly cục bộ `AppleHV-25G332.s`, đối chiếu log máy thật
`runs/0.2.2-phase2-verification.log`. Các địa chỉ dưới đây là địa chỉ trong
AppleHV disassembly, không phải địa chỉ kernel runtime. Chưa triển khai VM thật.

## vm_create: task trap 0

`TASK_TRAP_vm_create` tại 0x81c copyin **16 byte** (0x91a–0x923):

| Offset | Kích thước | Ý nghĩa |
|---|---|---|
| 0x00 | 8 | flags (default = 0 trong trace) |
| 0x08 | 8 | địa chỉ user nhận cấu hình interface |

`init` tại 0xf215 truyền qword thứ hai cho `init_vcpu_interface`.
Hàm này copyout **0x118 byte** tới địa chỉ đó (0xf134–0xf13f):

| Offset output | Kích thước | Ý nghĩa |
|---|---|---|
| 0x00 | 0x100 | bản sao `vcpu_if_config`; cần giải mã tiếp từng trường |
| 0x100 | 8 | địa chỉ user mapping interface RW |
| 0x108 | 8 | địa chỉ user mapping interface RO |
| 0x110 | 8 | địa chỉ user mapping hv clock/commpage |

Đây là lý do không thể chỉ tạo đối tượng VM rồi trả HV_SUCCESS.
Log stub hiện dump 64 byte nhưng vm_create chỉ copyin 16 byte: **48 byte sau
không thuộc cấu trúc đã xác nhận**, không được dùng để suy luận thêm trường ABI.

AppleHV kiểm tra entitlement trước copyin. Backend mới cần giữ kiểm tra quyền
và kiểm tra con trỏ qua copyin/copyout. Lỗi copyin của vm_create trả 0xfae94008.

## vm_map/unmap/protect: task trap 4/5/6

`vm_map` tại 0xd1c và `vm_unmap` tại 0xe48 copyin **0x28 byte**.
Từ register truyền cho `hv_vmx_space_t::map` (0xdf4–0xe04), kiểm tra giới hạn
host/GPA và nguồn `mach_vm_remap` trong `map` (0xcbc8):

| Offset | Kích thước | Ý nghĩa |
|---|---|---|
| 0x00 | 8 | address-space ID (AppleHV giới hạn <64) |
| 0x08 | 8 | host userspace address |
| 0x10 | 8 | guest physical address |
| 0x18 | 8 | size |
| 0x20 | 8 | memory flags |

Unmap lấy GPA + size từ 0x10/0x18. Cần đối chiếu thêm framework trước khi đóng
băng header ABI, nhất là quy tắc alignment/flags và nhánh protect.
NPT cần giữ backing pages hợp lệ suốt thời gian guest chạy, xử lý overflow,
quyền truy cập, overlap, unmap/protect đồng thời và flush TLB/ASID.

## vcpu_create: task trap 12

Tại 0x124c: copyin **16 byte**; qword đầu là user pointer nhận vCPU ID 32 bit
(copyout 4 byte tại 0x1306–0x130e); qword thứ hai là flags (AppleHV kiểm tra <=3).
Tối đa 64 slot; thread đã có target bị từ chối. Offset interface được tính:

- RW: base + id * **0xe18** (0x131e).
- RO: base + id * **0x6d8** (0x132c).

Các stride này chưa cung cấp layout thanh ghi/VMCS. Cần giải mã tiếp trước khi
framework được phép read/write register hoặc run.

## Vòng đời và callback

0.2.2 mới thay bảng trap; **callback AppleHV vẫn được đăng ký**. Trước khi
`hv_set_task_target`/`hv_set_thread_target` nhận đối tượng AMDHV, phải thay callback
bằng implementation tương ứng. Callback AppleHV task_destroy tại 0x1bf0 truy cập
trực tiếp layout/vtable AppleHV nên không được để nó nhận đối tượng backend mới.

Cần thiết kế giữ tham chiếu/khóa cho task exit, thread exit, destroy và trap đồng
thời; không tháo backend khi còn target/VM sống. Chưa coi các nhánh này là đã giải quyết.

## Thứ tự triển khai tiếp

1. Giải mã `vcpu_if_config` và accessors của framework; dựng header ABI có static_assert.
2. Cài callback/vòng đời VM và mapping interface RW/RO/commpage. Kiểm thử create/destroy,
   create lặp lại, con trỏ sai và process exit mà không destroy.
3. Map/unmap/protect với backing pages + NPT; xác nhận cleanup và rollback khi lỗi.
4. vCPU với VMCS ảo, register access; chạy guest CPUID → HLT trước khi thử boot Linux.

Chưa có vm_create/map/vcpu_create thật; giai đoạn 2 PASS không đồng nghĩa Docker/Podman chạy được.

## Cập nhật đã triển khai

0.3.0 đã PASS VM/NPT runtime. `hv_abi.hpp` chứa header cấu trúc đã kiểm tra với framework.
0.3.1 thêm bảng VMCS/register lookup, cache và vCPU; vẫn chờ runtime-test.

Sửa nhận định cũ: VMX capability mà `hv_vmx_read_capability` đọc nằm trong cấu hình interface
được copyout bởi vm_create (qword bắt đầu offset 0x28), khác với clock page ở output+0x110.
Trước vm_create vùng cấu hình này bằng 0; không suy ra rằng clock page chứa capability.
0.3.1 chưa công bố capability để QEMU khởi động; chỉ test guest real mode có giới hạn.

Tham khảo bit intercept và trạng thái SVM:
- AMD64 APM Volume 2: https://docs.amd.com/api/khub/documents/sD1_QL~h4Afq2_tvzxqqSQ/content
- Đối chiếu hằng số: https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/svm.h

Mã run mới giới hạn CPU mode/flags, dùng TF + exception intercept với budget 1024; POPF/IRET
bị intercept. CR0.TS/MP ngăn sửa FPU host trong bước tích hợp chưa có quản lý FPU.
Không coi đây là CPU emulator đầy đủ hoặc backend boot Linux.

## 0.3.1 verified / 0.3.2 CPU mode gate

0.3.1 passed hardware lifecycle, CPUID/HLT, guest RAM write, NPT unmapped/NX,
and bounded-loop checks. See the 0.3.1 run logs.

0.3.2 extends the pilot to flat CPL0 protected32 and low-half long64 with guest
4-level paging. These are constrained presets, not Linux/QEMU support. No FPU,
interrupt injection or general VMX controls are advertised. Segment attributes
must pack VMX bits 12..15 into SVM bits 8..11. CR3 always denotes guest physical
memory and hardware walks it through NPT; userspace never supplies host CR3.
`modeprobe` checks VA 0x100000 to GPA 0x8000, then removes GPA 0x4000 so the guest
PTE read at GPA 0x4800 faults in NPT. Hardware result remains pending reboot.

Architecture reference: [AMD APM volume 2](https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24593.pdf),
VMCB segment layout and long-mode consistency checks. Cross-check:
[Linux SVM definitions](https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/svm.h).

## Native Virtualization.framework observations (loaded 0.3.1)

`vzprobe --start` uses an isolated EFI variable store, 1 CPU and 512 MiB without a disk.
The Apple XPC VirtualMachine service passes vm_create flags=0, maps RAM (GPA 0,
size 0x20000000, flags7), then EFI ROM (GPA 0xfff00000, size 0x100000, flags5).
Start fails before any vcpu_create. Trap entry logging alone does not prove the return
value of either map. Independent romprobe confirms host PROT_READ backing mapped guest
RX returns HV_ERROR. 0.3.3 adds read-only pin fallback and rejects write permission
upgrades on such pins. Return-value logging will identify the next native startup gap.
No successful native EFI/Linux boot is claimed. XPC teardown can outlive probe process;
cleanup waits for debug.amdhv.vms=0 before releasing callbacks.

## 0.3.3 hardware / 0.3.4 pending

Host read-only ROM mappings now pass. Native VZ startup reaches vcpu_create flags=2,
identified as HV_VCPU_TSC_RELATIVE in the local hv_types.h SDK. hv.h defines relative
guest TSC as hv_tsc_clock()+offset. Framework set_tsc_relative writes RW+0xc78 and
sets word0 bits0x108; the kernel consumes it when bit0x100 is set. With the current
clock commpage tuple(0,0), VMCB TSC_OFFSET equals that relative offset. 0.3.4 implements
this for flag2 and allows RDTSC; RDTSCP, scaling and default-mode updates remain unsupported.

Mode hardware dumps show saved CS.G is omitted (c09b→409b, a09b→209b), even though
expanded limit remains ffffffff. Restoring G in userspace allows all mode assertions
to pass. 0.3.4 reconstructs G from expanded limit. Architecture cross-check:
https://github.com/torvalds/linux/blob/master/arch/x86/kvm/svm/svm.c (svm_get_segment).
Only architecture behavior was used; no Linux implementation was copied.
Logs: 0.3.3-vcpu-test.log (original failure), 0.3.3-vcpu-diagnostic.log (segment dump),
0.3.3-cs-repair-diagnostic.log (userspace workaround); 0.3.4 driver result pending reboot.

## Managed MSR ABI (0.3.4 observed, 0.3.5 implementation pending hardware)

VZ creates flag2 vCPU, then enables managed ID3 (CSTAR). `dump_msr.c` identifies
framework IDs1..5 = STAR/LSTAR/CSTAR/SFMASK/KERNEL_GS_BASE. Non-VMCS MSR values are
RW+0x7d0+id*8; dirty bits at RW+0xc50+(id/64)*8. RO+0x58+id is a byte slot index;
0xff means unbound. Bound slots reference 16-byte {address,value} entries in each
vCPU's RO MSR area. Reads prefer dirty RW values until consumed, then RO slots.
0.3.5 consumes/publishes only these 5 IDs. All guest MSR accesses still intercept;
managed operations are emulated, unmanaged RDMSR/WRMSR produce VMX exits31/32.
Invalid canonical addresses or SFMASK upper bits are rejected; #GP delivery remains missing.
VMCB offsets 0x600..0x620 hold these states; existing VMSAVE/VMLOAD restores host.
Architecture reference: https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/include/asm/svm.h
(vmcb_save_area layout), plus local framework/AppleHV disassembly and SDK hv.h.

## 0.3.5 verified / 0.3.6 pending

Managed syscall MSRs passed hardware tests including guest WRMSR/RDMSR, userspace
readback and disabled WRMSR exit. VZ next calls thread10 with 0x300000003 / 0x300000004,
then enables ID148. SDK hv.h/hv_types.h define READ=1,WRITE=2; trap packs ID low32 and
flags high32. ID148=FS_BASE and149=GS_BASE map to VMCS indices68/69 (encodings680e/6810),
not the RO MSR slots. 0.3.6 implements per-operation software permission masks under
permanent hardware MSR interception and aliases these bases to the VMCS/private/VMCB
state. Existing VMSAVE/VMLOAD saves/restores host FS/GS. Native Linux boot remains unproven.

## 0.3.6 native trace / SYSENTER state

All implemented managed MSR/access calls now succeed in VZ; next unsupported enable is
framework145, IA32_SYSENTER_CS. Framework146/147 represent SYSENTER_ESP/EIP. Their VMCS
aliases are482a/6824/6826 (indices55/79/80), VMCB offsets628/630/638. 0.3.7 uses the same
private alias path and software interception as FS/GS, validating CS upper32=0 and
canonical ESP/EIP before VMLOAD. Hardware SYSENTER transition is not claimed tested.
All 0.3.6 CPU/MSR regression assertions passed; 0.3.7 MSR state tests await reboot.


## 0.3.8 grouped native startup (hardware pending)

Static x86 call inventory is recorded in native-startup-25G332.json. Native XPC enables
11 MSRs, including TSC (framework144) after SYSENTER, and uses run_until(~0ull).
Framework TSC write checks vCPU flags at RO+0x18. Relative clock fields are RW+c68
(snapshot), c70 (published offset), c78 (pending offset), dirty flag100. TSC is special,
not an ordinary VMCB saved MSR: emulate using sampled host TSC + private guest offset;
WRMSR updates VMCB TSC_OFFSET, never host TSC. Descriptor VMCB offset0 must be skipped.
Trap1 with FOREVER shares bounded trap0 runner; finite deadlines remain unsupported.
FP write sets shared flags88; flag80 is retained as unsupported pending state so publish
cannot silently discard it. Entry rejects with full state diagnostics until FP support.
Native startup group probe on loaded0.3.6 reports exactly SYSENTER3 + TSC missing and
clean teardown. Static direct calls do not establish runtime success or indirect coverage.


## 0.3.8 runtime / 0.3.9 host cancellation

All11 native MSRs and new relative TSC/run_until tests passed on hardware0.3.8.
The native XPC subsequently calls task13 with mask1, receives unsupported, and exits
without run. This may be a startup kick or teardown; the trace alone does not prove root cause.
Local hv.h specifies immediate VMEXIT for hv_vcpu_interrupt. Local framework disassembly
7FFB13DF7DF0..7F39 packs IDs<64 into the syscall bitmap; count0 sends mask0.
0.3.9 uses a separate registry/mailbox mutex and atomic pending bits, so requests can arrive
while the bounded runner owns the main backend gate. The runner polls between single steps,
returning VMX external-interrupt exit1, zero instruction length/qualification, preserving RIP.
No guest interrupt injection or hardware IPI is claimed. Bitmap liveness is validated before
mutation and cleared on vCPU/VM slot reuse. Registry teardown follows gate→interruptGate;
request path never acquires gate. The runner does not acquire sleeping locks with IRQs disabled.
Tests cover coalescing, batch atomicity, reuse and10k concurrent producer/consumer handshakes;
new hardware probe remains pending reboot. ThreadSanitizer test also passed.


## Native reset entry observed on0.3.9 / implementation0.3.10

Native run_until now reaches the kernel. After one pending cancellation exit, entry is:
CR0=0x30, CR4=0x2000, CR0 mask/shadow=e0000020/60000010,
CR4 mask/shadow=2000/0, primary=200080, CS=f000/ffff0000/ffff/9b, RIP=fff0.
0.3.10 supports this bounded real-mode profile. VMXE is kept in private VMX state,
never AMD CR4; MOV CR reads use (state & ~mask) | (shadow & mask). A masked-bit change
returns VMX MOV-CR exit28 and leaves RIP unchanged; no-op writes are consumed.
Unmasked changes fail closed until general transition validation exists.
TPR-shadow is allowed only in real mode with zero threshold, no guest IRQ injection
or virtual APIC controls. CR8 is not executable there; protected/long TPR-shadow stays
unsupported. This does not implement an APIC or advertise APIC capabilities.
SVM decode-assist format checked against Linux cr_interception (CR_VALID bit63,
register low4), using NRIP for length; absent/invalid assist fails closed:
https://github.com/torvalds/linux/blob/master/arch/x86/kvm/svm/svm.c
Unit and probe cover masks, unchanged writes, invalid decode, high-ROM reset entry;
hardware0.3.10 pending. Native stop error delegate added because completion may have
nil error while state is already Error; probe must continue to reject that result.


## 0.3.10 executes native firmware / 0.3.11 descriptor group

Native EFI reaches CS.baseffff0000:RIPff4c, exit6b GDTR_WRITE, NRIPff51.
0.3.11 clears only descriptor table intercept bits6/7/10/11 of misc1 (SGDT/SIDT/LGDT/LIDT);
LLDT/LTR remain intercepted. Guest GDTR/IDTR bases/limits now load/save roundtrip through
VMCB and VMCS cache; memory operands use guest translation/NPT. Host tables are part of
VMRUN/VMEXIT state swap (AMD APM Vol2 chapter15 and appendixB):
https://docs.amd.com/api/khub/documents/sD1_QL~h4Afq2_tvzxqqSQ/content
Intercept and exit numbering cross-checked with Linux:
https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/svm.h
https://github.com/torvalds/linux/blob/master/arch/x86/include/uapi/asm/svm.h
Only CR0.PE0→1 is newly allowed, with unchanged paging/EFER and no other bit changes.
The driver updates private CR0 and VMCB CR0 (preserving forced TS/MP). Protected legacy
validation accepts cached16-bit segments after PE and ring0 GDT16/32-bit segments after
far jump/segment loads. No PE1→0/paging transitions/general FPU/interrupt support is claimed.
New descriptorprobe exercises full transition and an unmapped LGDT operand; hardware pending.


## 0.3.11 native trace / 0.3.12 legacy controls

Native VMM handles MOV CR0 exit28 at RIPff57 and resumes atff5a with VMCS CR0=23,
CR4=2000, CR0 mask/shadow=e0000020/40000023, GDTR=ffffff80/27. VZG11.fd reset code
confirms MOV EAX,40000023; MOV CR0,EAX; far jump; MOV CR4,640. Later prepares CR3=800000
and CR4.PAE. Audit file records firmware SHA256 and disassembled blocks, not runtime proof.
VMX treats CR0.ET as fixed1 even if its VMCS field is0 (Intel SDM vol3C, VM entry controls):
https://cdrdv2-public.intel.com/825750/326019-sdm-vol-3c.pdf
0.3.12 forces ET only in physical CR0 and guest read composition; VMCS state may retain0
until a guest write. Host TS/MP remain private. Masked bits use shadow as before.
Unpaged CR3 staging is page-aligned and48-bit bounded; legacy CR4 permits MCE/OSFXSR/
OSXMMEXCPT/PAE, plus host-owned VMXE filtered out of SVM. FPU remains blocked.
Step-budget exhaustion is now a resumable host exit1, not an unsupported instruction;
other unsupported exits remain errors. New hardware control/long-loop probe pending reboot.


## 0.3.12 native EFER preparation / 0.3.13 long entry

Native firmware now completes scanning and page-table fill using bounded yields. EFER
RDMSR/WRMSR exit31/32 are handled by userspace, which resumes atfffffe5f with EFER100,
CR0=23, CR3=800000, CR4=2660 and CSARc09b. New validation distinguishes LME armed while
paging off, IA-32e compatibility entry (PG/PE/PAE/LMA), and64-bit code after far jump.
VMX entry bit9 is accepted only for EFER500; old direct-SVM preset tests may leave it0.
Native GDT selectors need not match the original8/16 preset. CPL0/GDT selection and
segment/base/limit constraints remain validated. Masked PG changes still return exit28;
userspace must supply coherent CR0/EFER/shadow/entry state before resuming.
TPR-shadow control is only tolerated in the bounded execution subset: CR8 access remains
unsupported and nonzero threshold rejected. No APIC/IRQ support is claimed.
longtransitionprobe covers the userspace PG update, compatibility→64-bit far jump,
full-width RAM write, resumeHLT and NPT page-walk fault. Hardware0.3.13 pending.


## 0.3.13 native64-bit IO / 0.3.14 IO translation

Native EFI reaches RIPfffe150e in64-bit mode, SVM IOIO7b info1=3fb0210,
EXITINFO2=fffe150f: OUT byte DX, port3fb. All ports remain intercepted.
AMD IOIO fields (APM Vol2 section15.10.2): port31:16, segment12:10, address size9:7,
operand size6:4, REP3, string2, direction0; EXITINFO2 contains next RIP.
https://docs.amd.com/api/khub/documents/sD1_QL~h4Afq2_tvzxqqSQ/content
Intel VMX IO qualification: size2:0 (bytes-1), IN3, string4, REP5, immediate6, port31:16.
String instruction-info carries address-size9:7 and segment17:15.
https://cdrdv2-public.intel.com/812396/326019-sdm-vol-3c.pdf
0.3.14 verifies instruction bytes and SVM metadata before publishing exit30, unchanged RIP.
Instruction fetch happens with host state/interrupts restored, using pinned descriptor
readBytes under the backend gate; read-only translation checks guest paging/NPT permissions.
Never uses guest PA as host pointer or performs host I/O. Invalid fetch/decode returns
unsupported. New unit tests passed; actual IO probe and native continuation pending reboot.

### 0.3.15 FP-state subset and host preservation

Hypervisor-25G332 disassembly at hv_vmx_vcpu_read_fpstate/write_fpstate confirms
RO slot+18, RW slot+20, size slot+28, and dirty flag80 (write OR88). Our advertised
FP stride remains512: legacy FXSAVE64 layout only, not a full host XSAVE feature set.
Pull snapshots the shared bytes before checking MXCSR against conservativeFFBF;
rejection persists through publish until a valid explicit write. Both RO and RW are
published to retain baseline for partial writes; vCPU reuse resets both buffers.

The GIF-disabled assembly saves host legacy components with XSAVE64 mask3 and restores
with XRSTOR64 mask3. Guest uses FXSAVE64/FXRSTOR64. Resident pages8/9 are private guest
FP/hostXSAVE buffers; no user pointer reaches these instructions. Host CR0.TS is cleared
only inside the window and restored exactly. CR0.EM must be0, CR4.OSXSAVE/OSFXSR and
XCR0 bits0/1 must already be enabled. EFER.FFXSR is cleared during the bounded runner.
No guest AVX instruction is permitted (OSXSAVE rejected); host extended vector state
is untouched. Physical guest CR0.NE is forced to prevent legacy external FERR/IRQ13;
all guest exceptions still terminate unsupported rather than being injected.

`tools/tests/fpu_switch_test.S` shares the actual transition macros and tests dirty
and initial host states, x87 values/metadata, all XMM/YMM-upper and XSTATE_BV.
`fpprobe` also wraps the actual thread trap with host preservation checks, pending
0.3.15 hardware activation. Passing userspace macros does not prove kernel/VMRUN paths.

References used for the design: Apple XNU fpu.c (XSAVE/FXSAVE64 layouts and xstate
handling): https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/i386/fpu.c
and Linux xstate definitions: https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/fpu/xstate.h
The implementation is a bounded x87/SSE subset, not XNU/Linux FPU-owner integration.

### 0.3.16 active paging control writes

0.3.15 native trace reaches RAM1fffxxxx and rejects MOV CR0 at1fff0d2c. Extracted
DxeIpl has the sequence MOV RAX,CR0 / OR RAX,10000 / MOV CR0,RAX at RVA1d23..1d2c;
loadbase1ffef000 is inferred from the address match, not observed by a memory dump.
See `efi-paging-25G332.txt` for hashes and `tools/audit_efi_paging.py` for reproduction.

Private CR0 now accepts WP/AM in addition to PE/MP/ET/NE; PG remains mode-controlled.
CR4 accepts PSE/PGE in addition to the existing subset. MOV CR3 accepts aligned
48-bit roots in nonpaged/LME-prepared/active IA32e states. Every VMRUN still uses
TLB_CONTROL=1 to invalidate all cached translations, including guest global pages.
The handler checks masks before changing CR0/CR4, refuses direct PG changes and
PE1->0, and requires PE+PAE+EFER500 for paging. Unsupported controls, CR3 low bits,
PCID/no-flush, and guest exception delivery are not implemented by this change.

Hardware test pending: two global huge-page roots with distinct data backing,
active CR updates, and supervisor write permission with WP0 versus WP1. Unit tests
cover accepted states and rejection without mutation. Architectural reference:
https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24593.pdf
(CR0/CR3/CR4 and paging). This is a bounded subset, not general paging emulation.

### 0.3.17 NXE is independent of the paging mode

Native0.3.16 reaches RDMSR/WRMSR1fff0d98/1fff0d9f and userspace resumes with
EFER=d00, CR0=80010033, CR3=800000, CR4=2660, entry-control200. The old exact500
EFER comparisons reject this state. `pagingEfer` now removes only NXE for mode
classification; actual EFER retains NXE when loaded into the VMCB. SCE/SVME and
reserved bits still fail classification, and existing PG/PE/PAE rules remain.

The instruction reader refuses NX at every page-table level regardless of NXE:
with NXE set it forbids execution; without NXE it is a reserved-bit fault. Other
reads/writes are executed by hardware and may access NX data pages when permitted.
New nxprobe hardware coverage (pending activation) includes the native WRMSR→VMM
update path, IO fetch, NX data, parent/leaf NX, 2MiB/1GiB pages and NXE-off reserved NX.
Paging control tests also run with NXE enabled. Guest page-fault delivery remains
unsupported: negative hardware cases expect a stopped run, not exception injection.
Reference: AMD APM Vol2, No Execute bit and page-fault rules:
https://docs.amd.com/api/khub/documents/sD1_QL~h4Afq2_tvzxqqSQ/content

### 0.3.18 flags instruction execution and per-quantum TLB scope

0.3.17 passed the complete NXE gate. The 15s/120s native lifecycle tests stopped
cleanly without boot proof; a 300s observation reached PUSHF at1fb401ad (exit70,
NRIP1fb401ae). `vzprobe --start --seconds N` now permits1..300 for further diagnosis.

PUSHF/POPF cannot run with the debug TF exposed. The runner temporarily clears TF,
disables their intercepts and enables an execution breakpoint at the validated
linear NRIP using guest DR7=401 and a temporary DR0. These instructions cannot
branch; faults remain intercepted. On B0 #DB at NRIP, synthetic RF is removed and
internal TF restored. Known pre-execution intercepts at that same next boundary
are forwarded through their normal handlers. Unknown boundaries fail closed.
POPF flags outside the supported arithmetic/DF subset stop before another instruction;
this is not IF/IOPL/debug/exception virtualization. Stack memory and paging A/D
updates are performed by hardware, not by a software page walker.

DR0 is changed only under GIF0 and restored before STGI. Metadata offsets in the
full assembly frame: savedDR0=0,breakpoint=8,CR0=16,hostFP=24,guestFP=32,GPRs=40,
guestPA=48,hostPA=56. The runner also checks host DR0 at quantum exit and restores
it defensively before host DR7 if needed. Hardware validation of this path is pending.

TLB_CONTROL=1 starts every run quantum, and follows handled CR writes (including
same-value CR3) or INVLPG. Internal steps use0 while the gate and IRQ-disabled CPU
pinning exclude NPT mutations and ASID reuse. All new calls flush even after cancellation.
INVLPG is intercepted and conservatively flushes all; guest INVLPGA stays unsupported.
No unbounded run mode was introduced. New hardware probes test real global PTE changes,
NPT replacement/protection, flags widths/stack faults and 100000 bounded iterations.
References: AMD APM Vol2 ASID/TLB/VMRUN semantics and Intel PUSHF/POPF instruction rules:
https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24593.pdf
https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf

### 0.3.19 PAUSE handling

0.3.18 passed the hardware flags/TLB/VM gates. Native VZ advanced past PUSHF,
then stopped at SVM PAUSE exit77 (RIP1fb401d8, NRIP1fb401da). Static evidence in
`efi-pause-25G332.txt` matches a port-polling spin loop, not a runtime memory dump.
0.3.19 retains the intercept and consumes the hint using validated NRIP without
changing guest flags/GPRs. It counts against the existing1024-step budget and
cancellation checks. PAUSE is also a valid pre-execution boundary following a
hardware PUSHF/POPF step. VMX PAUSE-exiting controls remain unsupported.
New hardware probes check1536hints over two quanta, an infinite PAUSE/JMP loop,
state preservation, and adjacent PUSHF/POPF→PAUSE in16/32/64-bit modes.
Offline checks pass; hardware results pending reboot. Linux boot is not achieved.

### 0.3.20 interrupt readiness, without event delivery

Guest IF is allowed; VMCS interruptibility accepts0/1/2, forbids STI shadow with IF0.
VMCB INT_SHADOW is restored at run entry and published as IF?1:2 when set. The
single SVM bit cannot encode all VMX debug/NMI distinctions. With primary bit2,
software checks IF and shadow at each bounded instruction boundary and returns
VMX7 when ready; it does not arm V_IRQ or inject a dummy event. Emulated retired
CR/MSR/PAUSE/INVLPG instructions clear shadow. Unconsumed exits preserve it.
EVENTINJ and NMI state/window remain unsupported. The earlier Linux proposal
omitted an important delivery issue: internal TF can leak into the saved guest
interrupt frame, and interrupt gates clear TF, defeating bounded handler entry.
Event delivery needs a separate bounded mechanism before enabling it.
Reference for INT_SHADOW/V_INTR_MASKING layout:
https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/include/asm/svm.h
KVM observations are in linux/KVM-INTERRUPT-REFERENCE.md; they are not AMDHV gates.

### 0.3.21 disabled guest debug-register virtualization

Native0.3.20 passed IF/shadow gates and stopped on MOV RAX,DR0 in a firmware
CPU-state save routine. DR0..3/6/7 accesses are decoded after host IRQ restoration
and emulated in private guest state, returning hostexit1 after one consumed move.
DR0..3 never load host debug registers; DR6 cannot expose internal single-step/B0
status. DR7 accepts disabled-breakpoint state only (no enable bits or GD).
Primary bit23 instead returns VMX29 without consuming the instruction. SLDT/STR
execute on fixed guest LDTR/TR selectors; LLDT/LTR remain unsupported.
VMX-register IDs41..44 are guestDR0..3;47 is guestDR6; DR7 lives inVMCS681a.
DR4/5 aliases and active guest debugging remain unsupported. New debugprobe covers
16/32/64 modes; hardware gate pending. Instruction reference:
https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf

### 0.3.22 software external IRQ entry and IRETQ subset

0.3.21 hardware gates passed; native EFI reached VMX7 and requested external
vector0x40 throughVMCS4016=80000040. Native log: runs/0.3.21-vz-observe300.log.
Supported entry is same-ring64bit, sameCS, flat accessed GDT code descriptor,
IST0, interrupt/trap gate, IF1 withoutshadow. A bounded software memory transaction
aligns RSP16 and creates the five-qword SS/RSP/RFLAGS/CS/RIP frame before internal
TF is introduced. Hardware EVENTINJ remains unused. IRQ handler execution keeps
the normal bounded runner. IRETQ is separately emulated for sameCS/SS and allowed
flags; all state commits follow validation. No guestTF in either savedframe or
handler-visible PUSHF. VMCS entry event is cleared after successful consumption.
The transaction reads guest paging tables, enforces supervisor WP/NX/reserved bits,
preflights pinned live writable NPT backing for frame and A/D updates, then commits.
Missing mappings or unsupported delivery/return cases fail with HV_UNSUPPORTED;
exception synthesis, IDT-vectoring and delivery NPF retry are not implemented.
This deliberately limited emulation does not support NMI, IST or privilege changes.
AMD APM Vol2 long-mode stack switching reference:
https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24593.pdf
Hardware eventprobe pending reboot; KVM observations do not replace that gate.

### 0.3.23 opt-in host-IRQ execution and CR8/TPR

0.3.22 IRQ/IRET hardware gates passed. Native EFI progressed to MOVfromCR8 at
RIP1fa59417, NRIP1fa5941b. CR8 now maps to framework register49 (APIC TPR byte),
with explicit VMX28 load/store control exits. Defaultstep mode remains in use.
Experimental debug.amdhv.fast selects nointernalTF, native PUSHF/POPF/IRET and
external hardwareEVENTINJ. HostIF is enabled only withinGIF0, far beforeVMRUN;
CLI is first afterVMRUN, beforeSTGI/restored hostIRQ enable. Physical INTR/NMI
exits return VMX1, with32handled-exit quantum cap. Timer-based preemption must
pass hardware tests; the exit count is not an instruction/time deadline.
Details/references/limitations: external/NO-TF-RUNNER.md. No NVMM engine was loaded.
Hardware0.3.23 results pending reboot. Linux/container goal remains incomplete.

### 0.3.23 hardware results / 0.3.24 CR4.DE

Step/VM-ROM/fast gates PASS. Finite timer test observed22 hostIRQ exits,
worst call5.739ms; infinite IF0/IF1, VINTR, CR8 and hardware IRQ/IRET passed.
Native EFI then failed on CR4=668 at RIP1fa59425 (old CR4=660), i.e. DEbit3.
0.3.24 permits DE with existing all-MOVDR interception and disabled guestDR7.
DR4/DR5 and exception synthesis remain unsupported; enabled breakpoints/GD
remain rejected. No host debug addresses are loaded. Tests cover mode re-entry,
readback/clear and masked writes. New step/fast hardware gates pending reboot.
AMD CR4.DE reference: AMD APM Vol2, CR4 and reserved debug registers sections:
https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24593.pdf
Linux/container boot still unproven. See runs/0.3.23-{vcpu-test,vm-test,fast-test,vz-fast300}.log
and runs/0.3.24-offline-checks.log.

### 0.3.24 hardware results / 0.3.25 CR2

Step, VM/ROM and fast hardware gates passed, including CR4.DE. Native EFI
advanced to MOV RAX,CR2 at RIP1fa5942d, SVM exit2, NRIP1fa59430 (CR4=668).
Static IRQ dispatcher analysis also found MOV CR2,RAX in its return path;
see efi-cr2-save-25G332.txt. No Linux boot proof.
0.3.25 emulates CR2 using framework register38, seeds VMCB_CR2 on entry,
updates it after intercepted writes and snapshots it on exit. Host CR2
continues to be saved/restored before host IRQ re-enable. CR2 payloads
are register values only, never host pointers. Legacy writes use32 bits;
IA-32e uses64. Guest exceptions remain intercepted and unsupported; this
does not add #PF delivery. Unit/offline gates passed; new hardware gates
remain pending reboot. AMD APM Vol2 CR2 register reference:
https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2

### 0.3.26 system descriptors and Linux null long-mode data caches

0.3.25 step/VM/fast gates PASS. EFI used ResetSystemRuntimeDxe guest shutdown;
reason remains unresolved. VZ direct Linux boot reached LLDT at10000f5, followed
statically by LTR10000fd, PG entry and clearing five data selectors in code64.
0.3.26 clears SVM LLDT/LTR intercepts while retaining task-switch/exception traps;
VMLOAD/VMSAVE guest LDTR/TR caches are seeded and published each run. Existing
GIF0 host VMLOAD restores host system segments before host IRQ re-enable.
Validation permits null LDTR/LDT/busy32-64TSS subset with canonical bases and
appropriate attributes/limits, and null code64 data segments with canonical FS/GS.
No null CS, ring3 or general exception delivery was added. Native GDT busy-bit
updates pass through NPT. New hardware gates pending reboot; offlinePASS.
Scope/evidence/remaining gaps: FIRMWARE-AUDIT-0.3.26.md.

### 0.3.27 SVM cache metadata normalization

Normal0.3.26 gate failed after native LTR: saved TR89 omitted busy although guest
GDT had busy set. Explicit userspace repair revealed null SS4000 (DB retained,
not present). Normalize saved caches before publishing VMX: TR type9→11 only
for present system32/64TSS, not-present data/LDT→unusable, present data accessed,
retain expanded-limit G reconstruction. Input validation remains strict.
The shared pure helper passed unit tests and explicit diagnostic hardware probes
on loaded0.3.26, including long transition and fast mode. Those probes amend VMCS
through public APIs and do not validate unmodified0.3.26 or loaded0.3.27.
A fixture incorrectly expected NPF for a guest-nonpresent GDT PTE; now checks
both guest#PF rejection and guest-present/NPT-absent separately. No #PF delivery
or ring3 support added. Normal0.3.27 hardware gates still pending reboot.
See SEGMENT-AUDIT-0.3.27.md and Linux/KVM svm_get_segment for primary reference.

### 0.3.28 high canonical code/stack and fast EFER.SCE

Normal0.3.27 step/VM/fast gates passed. Native VZ Linux reached kernel CPUID
ffffffff810441b1, then lower-half validation rejected re-entry atffffffff810441b3.
Audit of the exact bzImage ELF also found imminent SCE enable in that same kernel
head block. Allow canonical48 code64 RIP/RSP, flags/PAUSE targets and software
IRQ/IRET handler/stack/return addresses; retain hole/wrap/mode/ownership checks.
SCE is separate from paging mode and is allowed only in host-IRQ fast execution
with active IA-32e, never in step mode where SYSCALL/SFMASK can clear internalTF.
Guest managed syscall MSRs still use VMCB/VMLOAD and existing validation. No ring3,
SYSRET validation or full exception delivery added. High-alias event/fetch/SCE
unit and new hardware probes prepared; offlinePASS, hardware pending reboot.
See HIGH-ENTRY-AUDIT-0.3.28.md for evidence and limits.

### 0.3.29 synchronous fault delivery and VMX exception ABI

Normal0.3.28 gates PASS, including high-address and SYSCALL. Native Linux reached
guest#PF atffffffff82e3c985 (REP MOVSD), faultVAffff888000001000/error0.
Fast code64 now permits the selected architectural synchronous faults through
the guest IDT, respecting exception bitmap and constant(mask0) PF mask/match.
Intercepted faults become VMX0 with type/vector/error fields; hardware exception
entry accepts the selected type3 vectors and packs4018 into EVENTINJ high32.
Direct#PF VMX exit preserves pre-entry CR2; native handler MOV CR2 reads current
VMCB state. RF is accepted only in fast mode. Delivery-NPF remains VMX48 with
vectoring metadata; guest shutdown/triplefault becomes VMX2. Step, debug, NMI,
MC, ring3, nonconstant PF filters and LA57 remain gated. New faultprobe pending
hardware; offlinePASS. See EXCEPTION-AUDIT-0.3.29.md for precise scope/evidence.

### 0.3.29 hardware results / 0.3.30 native CPU-init group

Normal0.3.29 step/VM-ROM/fast gates PASS, including the full new faultprobe.
Linux advanced from boot-data PF into CPU initialization and rejected CR4=100b0.
Diagnostic guest command-line feature clearing exposed SMEP, OSXSAVE and Linux's
INT3 self-test before staging another kext. No diagnostic result is a normal boot
success. See CPU-INIT-AUDIT-0.3.30.md and its exact ELF/log evidence.

0.3.30 adds physical-CPUID-gated FSGSBASE/SMEP/SMAP/UMIP CR4 bits only to fast
code64. CR masks/shadows retain priority. Native VMLOAD/VMSAVE and existing managed
MSR synchronization preserve FS/GS/KERNELGSBASE; no host MSR write path is added.
Fast code64 retains RF/AC; STAC/CLAC and guest permissions execute in hardware.
The intercepted-code walker additionally rejects effective U/S1 fetches with SMEP,
including huge pages. Native unmasked INT3 delivery preserves architectural trap
RIP; bitmap3 interception and VMX type6 injection remain unsupported.

OSXSAVE/XCR0 extended state is a known separate blocker, still rejected before
entry; guest512-byte FXSAVE and host legacy-mask XSAVE are unchanged. No CPL3,
PCID/LA57/PKRU/CET, full debug/NMI or full software-interrupt ABI is claimed.
OfflinePASS; new feature hardware gates await loaded0.3.30 and reboot.

### 0.3.30 hardware / 0.3.31 bounded XSAVE

Normal0.3.30 step/VM-ROM/fast gates PASS after correcting only the userspace SMEP
PFEC expectation15→11; CPL0 protection-fetch PF has P+I/D, not U/S. Native CPU
features and INT3 are proven in those gates. Default Linux rejects OSXSAVE;
explicit noxsave diagnosis prints1130 console bytes and reaches a CPL3 context
before validation rejects it. No complete Linux/container lifecycle is claimed.

0.3.31 adds fast-code64 OSXSAVE/XCR0=1/3/7 with a private320-byte XSAVE tail beside
the existing512-byte legacy API payload. Physical AVX layout must be576/256,
host XCR0 exactly7 and host XSS/XFD0. All-components mask7 save/restore in GIF0
prevents host YMM exposure across guest XCR0 changes; host XCR0 is restored before
saving guest state, restoring host FP, CR0 and STGI. Stack offsets are unchanged.
Guest XSETBV is intercepted, validates CPL0/OSXSAVE/index/value/NRIP before commit;
invalid requests still return Unsupported rather than full architectural GP.
Step remains on the legacy switch; CPL3/AVX512/PKRU/CET/supervisor state stay gated.
Four actual FP data-transfer macro variants pass1000-run offline preservation
tests. Privileged XCR0 switching and new xsaveprobe hardware gates await reboot.
See XSTATE-AUDIT-0.3.31.md for scope, compiled transition and pending hardware cases.

### 0.3.31 hardware / 0.3.32 extended user xstate

Normal0.3.31 gates all PASS, including real bounded XCR0/AVX/XSAVE tests and host
restoration. Default Linux reaches an unsupported XSETBV; clearing pku alone
still fails, clearing avx512f+pku keeps XSAVE enabled and produces console before
CPL3 rejection. The old log lacks the exact XCR0 operand; no full boot is claimed.

0.3.32 validates physical CPUID.D layouts for x87/SSE/AVX, the complete AVX512
group and PKRU, with physical-subset/dependency/overlap/supervisor/end<=3968 checks.
The shared FP window remains512 bytes; a private3584-byte tail retains full state.
Within GIF0, temporary XCR0 expansion allows all selected physical host state to
be saved/restored, including components disabled in the original host XCR0.
Original host XCR0 is restored before STGI. Metadata4064/4072/4088 is outside the
state image; stack offsets unchanged. PKE is gated by physical capability and
PKRU preservation; native PKRU survives guest OSXSAVE/XCR0[9] being disabled.
XSS/XFD0 guards remain. CPL3/supervisor/AMX/CET/unknown layouts stay unsupported.
Actual dynamic-mask FP data helpers pass1000-run userspace tests with mask7;
new AVX512/PKRU hardware gates await0.3.32. See EXTENDED-XSTATE-AUDIT-0.3.32.md.

## 0.3.32 hardware PASS và CPL3 0.3.33 (2026-11-11)

- Loaded0.3.32; gate thường vCPU --modes / VM --rom / fast-test đềuPASS,
  gồm AVX-512 ZMM0/ZMM31/K7, full-layout XSAVE, PKRU/PKE, reset/isolation và
  hostFP/XCR0restore. Logs `research/runs/0.3.32-{vcpu-test,vm-test,fast-test}.log`.
- DefaultLinux không featureclear đạt1130bytesconsole, rồi validator chặn
  re-entry CPL3 tại7fb6ebdb3125, CS33/a0fb, SS2b/c0f3, XCR0=e7; fail0.446s.
  `research/runs/0.3.32-vz-linux60.log`. Chưa Linuxboot hoàn tất/container.
- Rà nhóm CPL, segmentcache, SYSCALL/SYSRET/SWAPGS, IRET, INTn/DPL, TSS RSP0/IST,
  privilegedintercepts, XSETBV, IOPL/IOPB và userpage/SMEP/NPT trước khi cài.
  Báo cáo `research/CPL3-AUDIT-0.3.33.md`; firmwarehash không đổi/70PEinventory;
  audit executableELF của musl/libcrypto/busybox, không suy process từ ASLRRIP.
- 0.3.33 mở fast64 CPL0/3 có cache nhất quán/TSS, explicit VMCB.CPL mỗientry,
  publish SS.DPL theo actualCPL. Native INTn checksDPL; userXSETBV injectGP hoặc
  directbitmapexit, không commitXCR0; guard interceptđặcquyền; sửa userfetch/SMEP.
  Step/legacy/rings1/2/usernullSS/LDTselector/IOPL vẫn chặn.
- Offline0.3.33 PASS: build/imports339/ASan/UBSan/CPL+userfetchunits/fourFPvariants/
  resolver6/scriptsyntax; `research/runs/0.3.33-offline-checks.log`.
- Newcplprobe build/sign/staticdisassembly PASS; hardware pendingreboot:
  IRET3/re-entry/TLS/SYSCALL/SYSRET/SWAPGS/GP/UMIP/XSETBV/IOPB/userPF/NPT/
  IRQ/IST/INT80/DPLdenial/INT3/stackNPFvectoring vàcleanup.
- Sau reboot: gate thường vCPU --modes → VM --rom → fast-test (cplprobe cuối),
  chỉ khi toàn bộPASS mới defaultLinux. APIC/SMP/fullNMI/debug/compatuser32/
  interruptedtype4INTreinjection và containerlifecycle chưa hoàn tất.

- Cài0.3.33 PASS: installscript exit0, kmutil exit28 yêu cầu reboot; AuxKC đúng
  version/path. `research/runs/0.3.33-install.log`. Backup0.3.32 tại
  `research/kext-backups/AMDHV-0.3.32.kext`. Loaded vẫn0.3.32, takeover/vms/
  fast/hv_support đều0. Dừng ở bước reboot để kích hoạt và test hardwaremới.

## 0.3.33 hardware PASS và speculation controls0.3.34 (2026-11-11)

- Loaded0.3.33; vCPU --modes630 / VM --rom29 / fast-test172 PASS lines,
  khôngFAIL. CPL3/TLS/SYSCALL/SYSRET/TSS/IST/INTn/DPL/NPT đãhardwarePASS.
  Logs `research/runs/0.3.33-{vcpu-test,vm-test,fast-test}.log`.
- Sửa2lỗi userspacefixture trên cùngkext: HV_VM_DEFAULT và STAR userselectorRPL3
  theoAMD, không thaykernel/normalizationđểPASS. Loggốc giữfixture-options/star.
- DefaultLinux chạy /init và fork/SYSCALL, nhưng Oops switch_mm_irqs_off+4a9:
  WRMSR ECX49 EDX:EAX1 (PRED_CMD/IBPB). Lifecycle60sPASS nhưng guestfault,
  console5157bytes. `0.3.33-vz-linux60.log`; KHÔNGcoi làboot thành công.
- Diagnosticmitigations=off 10s tới AlpineInit/loaddrivers/emergencyrecoveryshell
  do không cóbootmedia: `0.3.33-vz-diag-mitigations-off.log`. Khônglàdefaultproof.
- vzprobe nay phát hiệnOops/Kernelpanic/pagefault từconsole và returnfailure;
  replaydefault1s đúngexit1: `0.3.33-vz-linux1-consolecheck.log`.
- Rà nhómIBPB/SBPB/SPEC_CTRL/IBRS/STIBP/SSBD/PSFD/L1D_FLUSH/legacyVIRT_SSBD/
  EFER.AUTOIBRS/modelMSRs, physicalCPUID, kernelELF và91MSRsitesfirmware70PE
  hashkhôngđổi. `research/SPECULATION-AUDIT-0.3.34.md` ghi phạmvi vàgiới hạn.
- 0.3.34 nativefast64: capability-gated realPRED_CMD/L1Dcommands; SPEC_CTRL
  guestprivateVMCB6e0, hardwareORhost, không cài guestvalue lênhost; hostvalue
  được đối chiếu sauquantum; GPcho reserved/unavailable/write-onlyreads,
  bitmap/nativepipeline, hardwareNRIP trướcsideeffect, state/re-entry/reset.
  Chưa mởmodel-specificMSRs/AUTOIBRS/legacyVIRT_SPEC_CTRL hoặcbitskhôngbiết.
- Offline0.3.34 PASS khôngwarning: build/imports339/ASan/UBSan/MSRcapabilityunits/
  fourFPvariants/resolver6/scriptsyntax. `research/runs/0.3.34-offline-checks.log`.
- Newspecprobe signed/staticbuildPASS; hardwarepending0.3.34. Sau reboot:
  vCPU --modes → VM --rom → fast-test (specprobe cuối), chỉ nếu tấtcảPASS mới
  defaultLinux khôngmitigationsoff. Chưa normalrootfs/container/APIC/SMPgoal.

- Cài0.3.34 PASS: installscript exit0; kmutil exit28 yêu cầu reboot;
  AuxKC đúng0.3.34 tại/Library/Extensions/AMDHV.kext.
  `research/runs/0.3.34-install.log`. Backup0.3.33 tại
  `research/kext-backups/AMDHV-0.3.33.kext`. Loaded vẫn0.3.33;
  takeover/vms/fast/hv_support đều0. Dừng tại reboot, hardwaremới chưa test.

## 0.3.34 hardware PASS, first diagnostic containers,0.3.35 EFI/memory (2026-11-11)

- Loaded0.3.34; gates vCPU --modes630 / VM --rom29 / fast-test203 PASS lines,
  zeroFAIL, gồmSPEC_CTRL/IBPB/SBPB/L1D/hostrestore. Logs0.3.34-{vcpu-test,vm-test,fast-test}.
- DefaultAlpine khôngfeatureclear/mitigationsoff tớiInit/emergencyrecoveryshell.
  vfkitinteractive thực thiuname/id/25forkexec/ghi+băm1MiB rồiguestpoweroffsạch.
  `research/runs/0.3.34-vfkit-linux-shell.log`. Chưa installedrootfs/defaultproviderproof.
- CloneAPFS riêng từstoppedPodmandisk, EFIvars riêng; sourceinode165146/mtime1793953935
  khôngđổi. EFI2GiB bịcap pin1GiB; EFI512MiB tớiGRUB CPUID rồi validatorchặn
  RFLAGS.ID200202. GRUBhelper10da9 thực sựtoggleID và giữnó, CPUIDRVA10e35.
- Rà đủpinbudget/NPTnodecap/partialunmap/rollback/globalaccounting; rerun70PEfirmware
  inventoryidentical vàscanthêmBOOTX64/GRUB. `research/EFI-MEMORY-AUDIT-0.3.35.md`.
- DIAGNOSTICONLY: tạm sửahelperGRUB trênclone đểrestoreID, khôngsửakext/gatethường.
  512MiB thiếuinitramfs/libsystemd;896MiB bootFedoraCoreOS44, kernel7.2.7 tới
  multi-user.target/Ready/SSH running, dùngNAT/RNG/vsock1025 và3disposableshares.
- FIRSTDIAGNOSTICCONTAINERS: MacPodmanCLI5.8.9 → explicitSSHengine5.8.9 trong
  vfkit/AppleVZguest, khôngQEMU. Alpine chạy; DNS/defaultbindreadwrite/volume giữa
  containers/buildimage+run PASS riênglẻ. PublishedportFAIL thiếugvproxygateway
  vớiNATfixture; workloadexit126. Chưađạtdefaultpodmanmachine/SMP/Dockergoal.
  `research/runs/0.3.34-macos-podman-diag-workload.log` vàbáocáođầyđủ.
- Guestpoweroffsạch; diagnosticGRUB đãrestoreoriginal/hashmatch. Clonegiữnamedvolume
  fixvm-diagnostic-data vớiFIXVM_VOLUME_PERSIST_OK đểtestpersistence sau normalboot.
- 0.3.35 preserve/admitID choallPilotpaths, vẫnchặnTF/IOPL/NT/VM/VIF/VIP;
  perVMpreparedpins8GiB/global16GiB underexistinggate, chargewholepreparedregion,
  commit/release/rollback đúng; NPTnodeceiling8192. Khônglazy/hugemapping/SMPmới.
- Offline0.3.35 PASS khôngwarning/imports339/ASan/UBSan/actualmemory2x8GiBstubmaps/
  modeID+reservedflags/fourFPvariants/resolver6/scriptsyntax. Newmemoryprobe>4GiB,
  step16/32/64 vàfastIDtoggle/re-entry hardwarepending0.3.35.
- Sau reboot gates vCPU --modes → VM --rom (memoryprobe) → fast-test; nếuPASS
  bootcloneGRUBgốc2GiB1CPU rồitestvolume vàtiếpapplehv/gvproxy/SMP/Docker.

- Cài0.3.35 PASS: installscript exit0, kmutil exit28 yêu cầu reboot; AuxKC đúng
  version0.3.35/path. `research/runs/0.3.35-install.log`. Backup0.3.34 tại
  `research/kext-backups/AMDHV-0.3.34.kext`. Loaded vẫn0.3.34; takeover/vms/
  fast/hv_support đều0, khôngvfkitVM/loopdiskcònchạy. GRUBcloneđãrestoregốc;
  sourcePodmandiskinode/size/mtime khôngđổi. Dừng ởreboot đểtesthardware0.3.35.

## 0.3.35 native Podman PASS; 0.3.36 cache/AP audit (2026-11-11)

- Loaded0.3.35: normal vCPU --modes / VM --rom / fast-test đều PASS, gồm >4GiB
  memory và IDflags. Logs `research/runs/0.3.35-{vcpu-test,vm-test,fast-test}.log`.
- Native Podman applehv/vfkit+gvproxy, GRUB gốc, 1CPU/2GiB: machine start/info/stop,
  container/DNS/virtiofs bind read-write/volume persistence/build-run/localhost18080
  nginx/API _ping OK. `research/runs/0.3.35-native-podman-workload-v3.log` PASS.
  Profile XDG và disk/EFI/shares clone riêng; chưa clean machine init. Build dùngcache.
  Global Docker socket alias vẫn trỏ defaultmachine cũ, phát pingwarning; actual
  isolatedAPI OK. Chưa DockerDesktop/SMP/performance hoặc fullgoal.
- Workloadv1/v2 fail do fixture httpd thiếu/existingvolume; giữ log, sửa fixture v3.
- SMP2 EFI AP #GP CSbase86000/IP10000 chưa rõnguyênnhân. DirectLinux SMP2 chặn
  cụthể WBINVD exit89 tại AP IP1/NRIP3. Hai lỗi chưa được chứngminh cùngnguyênnhân.
- AP slot1/separatethread/relativeTSC synthetic probes PASS trên35: realmode nonzeroCS,
  segments/CR0/GDT/IDT/PE/farjump32. Không chứngminh INIT/SIPI/fullSMP.
- Rà nhóm firmware/cache/AP và inventory70PE trong
  `research/CACHE-SMP-AUDIT-0.3.36.md`; không suyđoán BSPexit41 là TFleak.
- 0.3.36 consume WBINVD/WBNOINVD coherentRAM/noassignedDMA với MFENCE,
  validatedNRIP, shadow/RFretirement và boundedquantum; INVD forwardVMX13,
  exactlength/RIPunchanged. Không physicalhostcacheflush, secondarycontrolsvẫnreject.
  Thêm firstAPentrybytes/state diagnostic và unsupportedexitfullstate.
- Offline36 PASS imports339/ASan/UBSan/fourFPvariants/resolver6/scripts; compiled
  kernel khônghost INVD/WBINVD. Cacheprobe16/32/64, CPL3GP, CLI;WBINVD APfixture
  đãbuild/sign, hardwarepending36. Sau reboot chạy lại đủ3gates trước integration.

- Cài0.3.36 thànhcông: install exit0, kmutil exit28 yêu cầu reboot; AuxKC đúng
  0.3.36 tại /Library/Extensions/AMDHV.kext. `research/runs/0.3.36-install.log`.
  Backup35: `research/kext-backups/AMDHV-0.3.35.kext`. Loaded vẫn0.3.35;
  takeover/vms/fast/hv_support đều0, khôngvfkit/gvproxy/VM cònchạy. Isolatedprofile
  đã trả về1CPU/2GiB vàstopped; sourcePodmandisk inode165146/size99857989632/
  mtime1793953935 khôngđổi. Inventory36 identical35/70PE. Dừng ởreboot;
  hardwarecache36, SMP và DockerDesktop cònpending.

## 0.3.36 hardware/native PASS; 0.3.37 AP LME preparation (2026-11-11)

- Loaded36: gates vCPU --modes669 / VM --rom37 / fast-test240 PASS lines,
  zeroFAIL, exit0; cache16/32/64/CPL3GP/AP WBINVD/EFI APprefix PASS.
- Normal isolated nativePodman1CPU/2GiB workload PASS trên36, gồm build --no-cache,
  persistence sau hostreboot, bind/DNS/localhost18080/API _ping và machine stop.
  `research/runs/0.3.36-native-podman-workload.log`. Chưa cleaninit/globalalias/
  DockerDesktop/performance/fullgoal; khôngQEMU.
- Cả directAlpine SMP2 và PodmanEFI SMP2 vượt cache/firmware tới LinuxAP, dừng
  cùng EFER901/CR050033/CR420b0 hoặc20f0/RIP9a05d. Bộvalidator chỉ cho SCE
  activeIA32e nên chặn trước MOV CR0.PG. SMP vẫnFAIL. EFI #GP35 chưa táihiện,
  chưa kếtluận cănnguyên. Logs36-vfkit-linux-smp2 và36-native-podman-smp2.
- Rà actualAlpineELF trampoline16/32/64, CR/PAE/EFER/PG/compatfarjump, cache,
  CpuDxe/GDT/IDT; inventory70PE37 identical36. Báocáo
  `research/AP-LONGMODE-AUDIT-0.3.37.md` vàlinux/aptrampolineaudit.
- 37 cho SCE ở preparedLME ring0 chỉ trongfast; vẫnrejectstep/missingPE/PAE/
  barelegacy/invalidEFER. KhôngthayhostEFER, locks, FP/faultpolicy hoặc VMXABI.
  New longtransitionprobe --sce kiểm EFER901→d01/PG/compat64/TSS/NPT, addedfastgate.
- Probe mới trênloaded36 táihiệninitialreject (negativeexit1), nhưng explicitPG/LMA
  sauđó vẫnfarjump64/marker/HLT/NPT PASS; giữlog36-sce-preparation-negative,
  khôngcoi toànprobePASS. Hardware37 cònpending.
- Sửa diagnostic APentry16byte vốnluônbị helper15bytereject: nayread15/label15,
  khôngsuy unreadable36 là guestmappingfault. Khôngthayguestexecution.
- Offline37 PASS build/sign/imports339/ASan/UBSan/fourFPvariants/resolver6/scripts;
  sourcePodmandisk inode/size/mtimekhôngđổi, isolatedprofile đãtrả1CPU/2GiB/stopped.

- Cài37 PASS: installscript exit0; kmutil exit28 yêu cầu reboot, AuxKC đúng0.3.37
  tại /Library/Extensions/AMDHV.kext; codesign verify PASS. Backup36 tại
  `research/kext-backups/AMDHV-0.3.36.kext`, log `research/runs/0.3.37-install.log`.
  Loaded vẫn36, takeover/vms/fast/hv_support đều0. Dừng tại reboot để kíchhoạt37;
  chạy lại3gates (fast gồm --sce) rồi mới directLinux/Podman SMP2. Không tựreboot.

## 0.3.37 hardware and SMP integration (2026-11-11, ongoing)

- Loaded37; all three gates PASS, including prepared EFER901→d01 transition,
  WBINVD/AP slot1 and readable first-AP diagnostic. No new kext staged yet.
- Direct Alpine SMP2 boots to /init/recovery shell (no root media fixture), CPUonline0-1,
  processorcount2, both LAPIC timers/IPIs/TLBshootdowns, four parallel8MiB hashes match,
  guestpoweroff clean. `research/runs/0.3.37-vfkit-linux-smp2.log` exit0.
- Normal Podman applehv/vfkit SMP2: CLI/engine5.8.9, container/DNS/bind/volume
  persistence/uncachedbuild/localhost18080/actualAPI PASS, normalstop/idle0.
  `research/runs/0.3.37-native-podman-smp2-workload.log` exit0.
- Fresh Podman init fixvm-clean4CPU/4GiB from original cached base image PASS init;
  first boot NOT PASS: Podman Desktop auto-started original default12CPU concurrently.
  Clean guest reported HPET watchdog/softlockup; backend rejected NMI entry80000202.
  Original start canceled and Stop→HardStop through vfkit REST; Podman Desktop quit
  to prevent repeated auto-start. Original raw inode/size/mtime still unchanged.
- Retrying same interrupted cleanimage alone failed Ignition useradd group.lock left
  from interruption. Preserve both consolelogs and failed profile; not a cleaninitPASS.
  `0.3.37-clean-smp4-{concurrent,retry}-console.log`; workloadlogs retain exit125.
- Installed official Docker Desktop amd64 4.99.0/build245822, CLI29.9.3; archive/hash
  research/assets/Docker-amd64.dmg(.sha256), app codesign verify PASS. Accepted installer
  license; initial settings backed up, AutoStartfalse, 2CPU/4096MiB/VZ/virtiofs and
  disposable share only. No prior Docker app/data (containerdir4KiB).
- Docker native engine linux/virtualization-framework ready ~5.3s, actual
  com.docker.virtualization process2CPU/4096MiB + AppleVZ XPC; normalstop exit0,
  backendidle restored. `research/runs/0.3.37-docker-native-start.log`.
  Workload/restart/hostreboot acceptance still pending; no fullgoal claim.

## 0.3.37 native Podman/Docker milestone; 0.3.38 NMI (2026-11-11)

- Gates37 exit0:669/37/255 PASS lines, zeroFAIL. DirectLinux SMP2 CPUonline0-1,
  timer/IPI/TLBshootdown, four8MiB hashes and guestpoweroff PASS.
- Podman clone SMP2 full workload PASS; fresh machine init fixvm-clean2 2CPU/4GiB/
  disk16GiB from pristinecachedimage + Ignition + fullcontainer/DNS/bind/volume/
  uncachedbuild/localhost/API/start-stop PASS exit0. Chưa hostrebootproof mới.
- DockerDesktop4.99.0/build245822 installed officialamd64, CLI/engine29.9.3,
  actual AppleVZ/com.docker.virtualization, Linux7.0.14. Native2CPU fullworkload
  PASS; native4CPU strict fullworkload + CPU0..3 affinity32MiB hashesallmatch,
  normalstop exit0. `0.3.37-docker-native-smp4-strict.log`. NoQEMU.
- Dockervolume survived engine restart; newPodmanvolume proof read afterrestart,
  nhưng fullrepeat bịhủy exit143 do latency nặng/extraDockerstartup, khôngfullPASS.
  Earlytaskset fixture false completion corrected; 0.49/3.13s là INVALID CPUresults;
  strict4.70s gồm APK/network/container overhead, khôngCPUbenchmark.
- Docker4 startup33–52s vs2CPU~5.3s; timing/concurrency/stability vẫnopen.
  Podman4EFI GP/PF compatibilitymode RIPbfa55064/505d/CR3bfc01000 hoặc0 unresolved.
  Không coi lỗiEFI đãfix. Fresh4concurrentfirstboot rejected NMI80000202; retry
  interrupted disk bịgroup.lock, giữfailedimages/logs. PodmanDesktop auto-started
  default12CPU đãdừng/quit; originalraw inode/size/mtime vẫnunchanged.
- Harness now isolates runtime TMPDIR too: XDG-only shared gvproxy.pid withdefault
  machine. Newshortprivate tempdir avoids socketlength collisions; syntaxPASS,
  integrationpendingnextcycle. DockerAutoStartfalse, explicitengine sockets.
- 38 explicit NMI entry onlyfastnative64 + BOTHphysicalvGIF16/vNMI25; VINTRvGIFopen/
  vNMI, NMIblockingbit12↔VMXbit3, IRET hardwareunblock, IRQallowedwhileNMIblocked.
  No pendingqueue/NMIwindow/32modeNMI/newhostIRQ/scheduler or FPpolicychanges.
  Newnmiprobe + ASanstateunits; hardwarepending38. Morecode/GPR/farptr/GDT diagnostics
  forEFI fault, no guestrepair. Broadaudit70PEidentical and NMI/IRQ/IRET/vGIFgroup:
  `research/NATIVE-NMI-AUDIT-0.3.38.md`.
- Offline38 build/sign/imports339/ASan/UBSan/fourFPvariants/resolver6/scripts PASS.
  Newhardwarecases pendingreboot; fullprojectgoal NOT complete.

- Cài38 PASS: install exit0, kmutil exit28/reboot required, AuxKC đúng0.3.38 tại
  /Library/Extensions/AMDHV.kext, codesignverify PASS; backup37 tại
  research/kext-backups/AMDHV-0.3.37.kext. Loaded vẫn37. takeover/vms/fast/hv_support
  đều0; khôngvfkit/Dockerbackend/PodmanDesktop cònchạy. Clean2 đãtrả2CPU/4GiB/stopped;
  Docker4CPU/4GiB/AppleVZ/virtiofs/AutoStartfalse. Khôngtựreboot.
- Điểmnối `research/REBOOT-CHECKPOINT-0.3.38.md`: sau kíchhoạt38 chạyđủ3gates/NMI,
  read-only exactvolumeproof TRƯỚC workload ghi, rồi từngbackend riêng vàEFI4faultdump.
  Chưa hardwareNMI38, chưa hostrebootproof mới, chưa fullgoal/performance/stability.

## 0.3.38 hardware NMI PASS (2026-11-11, ongoing)

- Loaded38, initialidle0. Gates vCPU --modes669 / VM --rom37 / fast-test267 PASS
  lines, zeroFAIL, exit0. New NMI IF0/one-shot/blocked nested rejection/IRETQ/second
  NMI/IRQinsideNMI/IRQIRET unblocking all PASS on hardware.
  `research/runs/0.3.38-{vcpu-test,vm-test,fast-test}.log`.
- Docker4CPU/4GiB actualAppleVZ engine + read-only namedvolume proof afterhostreboot
  PASS exact FIXVM_DOCKER_PERSIST_OK before any write; cachedimage --pull=never,
  volumeinspectfirst. Normalstop exit0, gatesrestored0.
  `research/runs/0.3.38-docker-reboot-volume.log`.
- Podman clean2 read-only proof and EFI4 diagnosis continuing, fullgoal stillopen.


## 0.3.38 verified; staged0.3.39 (2026-11-11)

- Podman clean2/2CPU/4GiB read-only exact hostreboot volume proof PASS before writes,
  normalstop exit0. Both Podman and Docker now pass hostreboot persistence.
  `research/runs/0.3.38-podman-reboot-volume.log`.
- First defaultPodman4 run reachedLinux, TSC watchdog226495ns HPET vs197440ns TSC
  switched toHPET, rootUUID wait. Docker auto-opened11:21:25 and overlapped; CLIstop
  timedout then identifiedbackend TERM stoppedDocker. Podman canceled/hardstopped,
  gates0. This is NOT clean isolated default4 acceptance and EFI fault notprovenfixed.
  `0.3.38-podman-efi-smp4*` logs/console retain evidence.
- DiagnosticONLY: rpm-ostree queued tsc=reliable in disposable clean2,4CPU provider
  booted, currentclock=tsc. Prior exactreadonlyproof + fullcontainer/DNS/bind/volume/
  uncachedbuild/localhost/API and allCPU0..3 strictaffinity hashes PASS, normalstop,
  exit0. `0.3.38-podman-tsc-compare4.log`.6.33s includespackage/network/containeroverhead,
  notCPUbenchmark. This supports clock investigation, not a final default compatibility claim.
- Removed diagnosticargument beforeworkload; then booted2CPU default and verified
  BOTH /proc/cmdline and queued rpm-ostree args lacktsc=reliable. Exactproof/normalstop
  PASS exit0, `0.3.38-podman-default-restored.log`. Clean2 left2CPU/4GiB/stopped.
- HostinvariantTSC CPUID80000007.EDX6799;240000 serializedreads/24threads overAPIC
  bitmapfff0fff no backwards.4391.727MHz againstmachclock, whichmayitselfderiveTSC;
  notindependentfrequency/guestSMPskewproof. `0.3.38-host-tsc-sample.log`.
- Newaplongprobe:3independentAPslots1..3/threads+keeper0,64transitions each through
  real->PE->PAE/LME/PG->indirectFF2Ffar64; alternatingNXE/SCE, fullmarker/CR0/3/4/EFER
  assertions. Finalhardware38run4PASS,exit0; actualfirmware/sharedexchange race remains
  unproven. `0.3.38-aplong-test.log`; includedin39fastgate.
-39: sample successfulfast exits(first32/powers2), alwayslogexception/shutdown and
  unsupporteddiagnostics; newatomic runtime debug.amdhv.trace_exits fulltrace switch,
  releasecounters. ExistingunboundedIOLog+logmutex observed inspinloop; benefit notyetmeasured.
  No clock/timer/lock/scheduler/FP/NMIexecution orassemblypolicychanges.
- Broadaudit re-extracted70PE inventory39 exactlyequals38; reviewedAP/segments/paging/
  TSC/PAUSE/cancel/logginggroups. `research/CLOCK-TRACE-AUDIT-0.3.39.md`.
  Offline39build/signature/imports339/ASan/UBSan/FPvariants/resolver6 PASS exit0.
  FinalAPbinary rebuilt/hardwarePASS; script syntaxPASS; disassembly onlyMFENCE,
  nohostINVD/WBINVD. Hardware39trace/timing stillpending.
- Install39exit0: kmutil exit28/rebootrequired, AuxKC version0.3.39 atLibraryExtensions,
  installedcodesignverifyPASS; backup38 `research/kext-backups/AMDHV-0.3.38.kext`.
  Loadedstill38. takeover/fast/vms/hv_support all0; no testengines left. Noautomaticreboot.
  Next: `research/REBOOT-CHECKPOINT-0.3.39.md`. Fullgoal remainsopen.


## Hardware39 verified; staged40 parallel experiment (2026-11-11)

- Loaded39 gates exit0: vCPU669 / VM37 / fast271 PASS lines, zeroFAIL. New trace
  switch and AP real->64/NMI hardware PASS. Logs research/runs/0.3.39-*-test.log.
- Docker4CPU/4GiB AppleVZ fullworkload + all4affinityhashes + exactreadonlyhostreboot
  volumeproof PASS, normalstop exit0. Engine ready16.436s, fullworkload23.47s,
  nestedaffinity5.13s includespackage/container/network overhead, notCPUbenchmark.
  No meaningful startup speedup established versus38's16.611s proof run.
  research/runs/0.3.39-docker-smp4.log.
- Podman clean2 DEFAULT4CPU/4GiB/no tsc=reliable: currentclocktsc atworkloadentry,
  exactreadonlyhostrebootproof + fullcontainer/DNS/bind/volume/uncachedbuild/port/API
  and strictCPU0..3hashes PASS, normalstop exit0. Workload40.37s excludesstartup;
  nestedaffinity7.08s includesoverhead. WatchdogHPET/remoteCPU timeouts stilloccur.
  research/runs/0.3.39-podman-default-smp4.log and console. Singlepass notstable.
- Immediate default4repeat stalledbeforeengine, seriallogempty, highLinuxRIP spin
  quanta, no otherVM/providerinterference observed. RequestedStop, then ownedvfkit
  RESTHardStop; intentionalcancel exit125 (vfkit unexpectedly0), cleanupgates0.
  Preserve 0.3.39-podman-default-smp4-repeat*; no secondPASS/EFIrootcauseclaim.
  Clean2 restored2CPU/4GiB/stopped. Originaldefault12CPU profile nottesttarget.
- Newsmpprobe39: two guests mutuallyset/waitflags, noPAUSE; BOTHfirstcallsCPUID
  overlap0/64/errors0 confirmsserializedbaseline. Activeguest data-pageprotect/
  unmap/remap8rounds + normalthread/pincleanup PASS. Finalprobeexit0.
  research/runs/0.3.39-smp-serialization-baseline.log.
-40 opt-in debug.amdhv.parallel=0 bydefault, changeonlytakeover0/vms0. GlobalRWgate:
  ONLY fast run traps0/1 take sharedlifetime/mappinglease; allstate/map/lifecycle
  mutators exclusive; step stays exclusive. No target pointers acrossunlock.
  AllvCPU/privateFP slots remainownedthread; mailbox atomic, lockorderunchanged;
  ASID1 CPUlocal/fullflush everyquantum, noNPTedit whileactive. ThreadSafeIOmemory
  descriptors forparallelreadonlyfetch; unexpectedfastIRET failsclosed insteadof
  stepPTE/framewrites. HostIRQ/asm/clock/FP/NMIexecutionpolicyunchanged.
- fast-test serial and --parallel modes testpolicyguards, hostIRQbounds, positive
  overlappingguesthandshake(onlyparallel), mappingwriterprogress andallregressions.
  Harnesses explicitly FIXVM_PARALLEL=0/1 beforetakeover, restoreflag; nowrequire40.
  Podman start records externalelapsedtime includingproviderreadiness. Cancompare
  bothmodes onSAME40 afteractivation with guestrestarts, noextra hostrebootfortoggle.
- Broadaudit70PE matches39 SHA/instructiongroups afternormalizingorder/path;
 5extractionpaths differ, firmwarebytesunchanged. AP/segments/paging/atomics/
  IRQ/NMI/IRET/TSC/hostswitch/lifetime/descriptor groups reviewed together.
  research/PARALLEL-RUN-AUDIT-0.3.40.md; normalizedcomparisonlog retained.
- Offline40 exit0: build/sign/imports345/ASan/UBSan/FPvariants/resolver6 PASS.
  Finalsmpprobe rebuilt/testedon39 andallscripts syntaxPASS. DisassemblyMFENCEonly,
  nohostINVD/WBINVD. Parallel40 hardware/benefit stillPENDING, nofullgoalclaim.
- Installed40 exit0, kmutil28/rebootrequired; AuxKC0.3.40 at/Library/Extensions,
  codesignverifyPASS, backup39 research/kext-backups/AMDHV-0.3.39.kext.
  Loadedstill39, takeover/fast/trace/vms/hv_support all0; noenginesleft. NoautoReboot.
  Continue research/REBOOT-CHECKPOINT-0.3.40.md: serialgates thenparallelgates before
  providers, exactproofbeforewrites, repeateddefault4/timing; restore2CPUifunstable.


## 0.3.40 VM validation and Docker handoff (2026-11-11)

- Loaded40, gates669/37/273/273 zeroFAIL, exit0, both before and after hardrestart.
  Parallel smpprobe64/64 overlapping first calls, mapping writer/teardown PASS;
  physicalselftest24/24. Two VM tasks simultaneously peak2, SMP63/64 and AP3slots
  each64/64, no guesterrors, teardown0VMs. All hardware logs retained.
- Docker4CPU/4GiB actualAppleVZ:8 completeworkload runs(2serial/6parallel) including
  exactreadonlyproof BEFORE writes, CPU0..3 hashes, build/DNS/network/ports/bind/
  volume/API. Warm comparison serial8.401s startup/13.97sworkload versusparallel
  1.853s/6.02s. Parallelstartup samples1.853..2.399s; bounded local measurements.
  Extra4container CPU-pinned stress256correcthashes/8GiB aggregate cachedreads,
  diskwrite/sync/read + exactpersistedproof PASS,6.62s includescontainer/APIoverhead.
- Podman clean2 default2CPU one fullPASS, default4CPU3consecutivefullcycles + one
  afterhardrestart PASS; no tsc=reliable argument. Clocktsc atworkloadentry and no
  collectedunstablefallback/softlockup/Oops pattern. FullstrictCPUhash/build/network/
  storage/port/API/readonlypriorproof and normalstop allPASS. Leftstopped4CPU/4GiB.
  Original12CPUprofile notqualified. CLIstart30.45..31.54s includesglobalAPIalias
  pingtimeout20s inupstream5.8.9; isolatedAPIvalidatedseparately. Notpureguestboottime.
- Harnessrepeat3 racedoldDockerbackend single-instance socket whilevmsalready0;
  canceled143 beforeVMstartup, notkernel/workloadPASS. Addedstartprocessguard and
  waitforbackendmonitor/servicesexit afterteardown. Repeat3retry/repeat4 andall
  recovery/warm/finalruns PASS, explicitshutdownmarker. HeadlessCLIstop sometimes
  timesoutbeforemonitorends; ownedlauncherTERM+processdrain completescleanup.
- Host IdleSleep12:10:22 interrupted original serialwarmcomparison; userhardrestart
  newboot13:17. That logNOTPASS; plannedparallelfinal/readydidnotexecute then.
  Savedpmset/unifiedpowerlogs; cause ofwakefailure notestablished. Userlatestasks
  DEFERsleep/waketesting, prioritizeVM. No sleeps/resume tests performed or planned
  in this turn. Temporarycaffeinate mitigatesidleSleepduringtesting, notafix.
- Re-ranall4gates and BOTH exactvolumeproofs/fullworkloads afterhardreset PASS,
  thencompletedwarmserial/parallelcomparison. Per-probe caffeinate -i -w PID now
  holdsassertionforprobelifetime, observedinpmset and releasedafterexit. No power
  settings change/kernelversion/reboot needed for mitigation or remainingtests.
- Newmanual scripts/desktop-backend.sh start/stop/status verifiedstart+stop; final
  start succeeded. Loaded/installed/source remain40. HandoffINTENTIONAL:
  fast1/parallel1/takeover1/hv_support1, trace0, vms0; noDocker/vfkit/PodmanDesktop
  running. User may openDockerDesktop GUI now, CLIcontextdesktop-linux. Testengine
  configured4CPU/4GiB/AppleVZ/virtiofs, disposableUsersshare, AutoStartfalse.
- User-level detached idle-sleep guardPID2664 started13:28:36 for7200s, observed
  inpmset, expires~15:28local; onlytemporarytrialguard. See user-trial-awake.log.
  Sleep/wake deferred, longtermstability/broaderconfigs/dailyactivation stillopen;
  nofullgoalcompletionclaim. Reports research/NATIVE-ACCEPTANCE-0.3.40.md,
  research/USER-DOCKER-0.3.40.md, research/SLEEP-WAKE-INCIDENT-0.3.40.md.

- Latest handoff update: normal Docker backend started13:29:29 while documentation
  was being finalized. Read-only API ping OK, engine29.9.3/4CPU/Desktop; cached
  ephemeral GUI-backend smoke container exit0 prints x86_64,4,FIXVM_DOCKER_GUI_OK.
  Currentvms1 is the running Docker instance, not an orphan from the probe. Leave
  it running for the user. fast/parallel/takeover/hv_support1,trace0; no reboot.
  User priority: defer sleep/wake, focus working VMs. Trialawakeguard2664 active.


## Original Podman default machine recovery (2026-11-11)

User requested starting/fixing podman-machine-default. Docker was already stopped
(vms0/no Docker backend). Original12CPU/3814MiB applehv guest failed Ignition:
guest firstboot but host LastUp nonzero/Startingtrue omitted1024configvsock.
APFS-cloned pre-task93GiB disk and metadata/EFI/Ignition before repair. Restored
firstboot metadata, supplied existing config/identity. Initial tool-shell cleanup
interrupted provisioning; preserved intermediate disk and restored pre-task clone
before successful detached firstboot. No machine deletion or replacement image.
Also created missing host registries.conf with comment only; original Ansible
playbook rerun ok2/changed1/failed0, zero failed systemd units.
Two full default-machine12CPU workloads PASS, second after normal stop/start:
container/DNS/read-only bind/dedicatedvolume/uncachedbuild/CPU0..11 exacthashes/
localhost18081/API. Exact volume proof checked before second-run writes. Restart
cmdline no ignition.firstboot/no tsc=reliable. Collected restart dmesg no matching
unstable/softlockup/BUG/Oops pattern. Normalstop reachedvms0, restartvms1.
Final INTENTIONAL user handoff: defaultPodman RUNNING12CPU, API through both
private socket and /var/run/docker.sock OK; Docker stopped; fast/parallel/
takeover1 trace0, loaded40. No kext update/host reboot or sleep/wake test.
See research/PODMAN-DEFAULT-0.3.40.md and 0.3.40-podman-user-default-* logs.
