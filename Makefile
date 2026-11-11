# Build AMDHV.kext with Command Line Tools only (no Xcode project).
SDK      := $(shell xcrun --show-sdk-path)
KHEADERS := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
CLANGRT  := $(shell clang -print-resource-dir)/lib/darwin/libclang_rt.cc_kext.a

BUILD    := build
KEXT     := $(BUILD)/AMDHV.kext
BIN      := $(KEXT)/Contents/MacOS/AMDHV
HVPROBE  := $(BUILD)/hvprobe

CFLAGS   := -arch x86_64 -mkernel -fno-builtin -fno-common -nostdinc -O2 -g \
            -mmacosx-version-min=26.5 -Wall -Wextra -Wno-unused-parameter \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
            -isystem $(KHEADERS) -isysroot $(SDK)
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions
LDFLAGS  := -arch x86_64 -mmacosx-version-min=26.5 -nostdlib -Xlinker -kext \
            -Xlinker -export_dynamic -isysroot $(SDK) -L$(SDK)/usr/lib \
            -lkmodc++ -lkmod $(CLANGRT)

SRC_CPP  := $(wildcard AMDHV/src/*.cpp)
SRC_C    := $(wildcard AMDHV/src/*.c)
SRC_S    := $(wildcard AMDHV/src/*.S)
OBJS     := $(SRC_CPP:AMDHV/src/%.cpp=$(BUILD)/obj/%.o) \
            $(SRC_C:AMDHV/src/%.c=$(BUILD)/obj/%.o) \
            $(SRC_S:AMDHV/src/%.S=$(BUILD)/obj/%.o)

.PHONY: all clean
all: $(KEXT) $(HVPROBE) vmprobe vcpuprobe modeprobe romprobe vzprobe tscprobe msrprobe nativeinitprobe interruptprobe resetprobe descriptorprobe controlprobe longtransitionprobe ioprobe fpprobe pagingcontrolprobe nxprobe flagsprobe tlbprobe budgetprobe debugprobe eventprobe fastprobe systemprobe faultprobe xsaveprobe cplprobe specprobe memoryprobe cacheprobe apresetprobe aptransitionprobe aplongprobe nmiprobe smpprobe

.PHONY: smpprobe
smpprobe: $(BUILD)/smpprobe
$(BUILD)/smpprobe: tools/hvprobe/smpprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: aplongprobe
aplongprobe: $(BUILD)/aplongprobe
$(BUILD)/aplongprobe: tools/hvprobe/aplongprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: aptransitionprobe
aptransitionprobe: $(BUILD)/aptransitionprobe
$(BUILD)/aptransitionprobe: tools/hvprobe/aptransitionprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: cacheprobe
cacheprobe: $(BUILD)/cacheprobe
$(BUILD)/cacheprobe: tools/hvprobe/cacheprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: apresetprobe
apresetprobe: $(BUILD)/apresetprobe
$(BUILD)/apresetprobe: tools/hvprobe/apresetprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: memoryprobe
memoryprobe: $(BUILD)/memoryprobe
$(BUILD)/memoryprobe: tools/hvprobe/memoryprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: specprobe
specprobe: $(BUILD)/specprobe
$(BUILD)/specprobe: tools/hvprobe/specprobe.cpp AMDHV/src/speculation_policy.hpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: cplprobe
cplprobe: $(BUILD)/cplprobe
$(BUILD)/cplprobe: tools/hvprobe/cplprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: xsaveprobe
xsaveprobe: $(BUILD)/xsaveprobe
$(BUILD)/xsaveprobe: tools/hvprobe/xsaveprobe.cpp tools/tests/fpu_switch_test.S AMDHV/src/fpu_switch.inc tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -DAMDHV_TEST_RUN_TRAP -o $@ tools/hvprobe/xsaveprobe.cpp tools/tests/fpu_switch_test.S -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: faultprobe
faultprobe: $(BUILD)/faultprobe
$(BUILD)/faultprobe: tools/hvprobe/faultprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: systemprobe
systemprobe: $(BUILD)/systemprobe
$(BUILD)/systemprobe: tools/hvprobe/systemprobe.cpp tools/hvprobe/entitlements.plist AMDHV/src/segment_attributes.hpp tools/hvprobe/segment_diagnostic.hpp
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

$(BUILD)/obj/%.o: AMDHV/src/%.cpp AMDHV/src/*.hpp AMDHV/src/kernsyms.h
	@mkdir -p $(dir $@)
	clang++ $(CXXFLAGS) -c $< -o $@
$(BUILD)/obj/%.o: AMDHV/src/%.c
	@mkdir -p $(dir $@)
	clang $(CFLAGS) -c $< -o $@
$(BUILD)/obj/%.o: AMDHV/src/%.S AMDHV/src/fpu_switch.inc
	@mkdir -p $(dir $@)
	clang -arch x86_64 -c $< -o $@

$(KEXT): $(OBJS) AMDHV/Info.plist
	@mkdir -p $(KEXT)/Contents/MacOS
	clang++ $(LDFLAGS) $(OBJS) -o $(BIN)
	cp AMDHV/Info.plist $(KEXT)/Contents/Info.plist
	codesign -f -s - $(KEXT)

clean:
	rm -rf $(BUILD)

KDK ?= /Library/Developer/KDKs/KDK_26.7.2_25G332.kdk
BOOT_KC ?= /System/Library/KernelCollections/BootKernelExtensions.kc
.PHONY: check kernsyms
check: $(KEXT)
	tools/check_imports.sh $(BIN) $(KDK)

# Regenerate after updating macOS or rebuilding the Boot Kernel Collection.
kernsyms:
	python3 -I tools/gen_kernsyms.py $(KDK)/System/Library/Kernels/kernel AMDHV/src/kernsyms.h $(BOOT_KC)

# User-space probe for Hypervisor.framework
.PHONY: hvprobe
hvprobe: $(HVPROBE)
$(HVPROBE): tools/hvprobe/hvprobe.c tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang -arch x86_64 -O1 -Wall -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: vmprobe test-memory
vmprobe: $(BUILD)/vmprobe $(BUILD)/vmprobe-noent
$(BUILD)/vmprobe: tools/hvprobe/vmprobe.cpp tools/hvprobe/entitlements.plist AMDHV/src/hv_abi.hpp
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -IAMDHV/src -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@
$(BUILD)/vmprobe-noent: $(BUILD)/vmprobe
	cp $< $@
	codesign -f -s - $@
test-memory:
	@mkdir -p $(BUILD)
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -Itools/tests/stubs -IAMDHV/src tools/tests/memory_test.cpp AMDHV/src/guest_memory.cpp -o $(BUILD)/memory_test
	$(BUILD)/memory_test

.PHONY: vcpuprobe test-vcpu
vcpuprobe: $(BUILD)/vcpuprobe
$(BUILD)/vcpuprobe: tools/hvprobe/vcpuprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@
test-vcpu:
	@mkdir -p $(BUILD)
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/vcpu_state_test.cpp -o $(BUILD)/vcpu_state_test
	$(BUILD)/vcpu_state_test

.PHONY: test-modes modeprobe test-xstate test-cpl
test-cpl:
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/cpl_policy_test.cpp -o $(BUILD)/cpl_policy_test
	$(BUILD)/cpl_policy_test
test-xstate:
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/xstate_policy_test.cpp -o $(BUILD)/xstate_policy_test
	$(BUILD)/xstate_policy_test
test-modes:
	@mkdir -p $(BUILD)
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/vcpu_mode_test.cpp -o $(BUILD)/vcpu_mode_test
	$(BUILD)/vcpu_mode_test
modeprobe: $(BUILD)/modeprobe
$(BUILD)/modeprobe: tools/hvprobe/modeprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: vzprobe
vzprobe: $(BUILD)/vzprobe
$(BUILD)/vzprobe: tools/vzprobe/vzprobe.m tools/vzprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang -arch x86_64 -fobjc-arc -O1 -Wall -Wextra -o $@ $< -framework Foundation -framework Virtualization
	codesign -f -s - --entitlements tools/vzprobe/entitlements.plist $@

.PHONY: romprobe
romprobe: $(BUILD)/romprobe
$(BUILD)/romprobe: tools/hvprobe/romprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: tscprobe
tscprobe: $(BUILD)/tscprobe
$(BUILD)/tscprobe: tools/hvprobe/tscprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: msrprobe
msrprobe: $(BUILD)/msrprobe
$(BUILD)/msrprobe: tools/hvprobe/msrprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: nativeinitprobe
nativeinitprobe: $(BUILD)/nativeinitprobe
$(BUILD)/nativeinitprobe: tools/hvprobe/nativeinitprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: test-msr
test-msr:
	@mkdir -p $(BUILD)
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/msr_emulation_test.cpp -o $(BUILD)/msr_emulation_test
	$(BUILD)/msr_emulation_test

.PHONY: interruptprobe test-interrupt
interruptprobe: $(BUILD)/interruptprobe
$(BUILD)/interruptprobe: tools/hvprobe/interruptprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@
test-interrupt:
	@mkdir -p $(BUILD)
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/vcpu_interrupt_test.cpp -o $(BUILD)/vcpu_interrupt_test
	$(BUILD)/vcpu_interrupt_test

.PHONY: resetprobe test-cr
resetprobe: $(BUILD)/resetprobe
$(BUILD)/resetprobe: tools/hvprobe/resetprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@
test-cr:
	@mkdir -p $(BUILD)
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/cr_emulation_test.cpp -o $(BUILD)/cr_emulation_test
	$(BUILD)/cr_emulation_test

.PHONY: descriptorprobe
descriptorprobe: $(BUILD)/descriptorprobe
$(BUILD)/descriptorprobe: tools/hvprobe/descriptorprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: controlprobe
controlprobe: $(BUILD)/controlprobe
$(BUILD)/controlprobe: tools/hvprobe/controlprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: longtransitionprobe
longtransitionprobe: $(BUILD)/longtransitionprobe
$(BUILD)/longtransitionprobe: tools/hvprobe/longtransitionprobe.cpp tools/hvprobe/entitlements.plist AMDHV/src/segment_attributes.hpp tools/hvprobe/segment_diagnostic.hpp
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: test-io
test-io:
	@mkdir -p $(BUILD)
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/io_exit_test.cpp -o $(BUILD)/io_exit_test
	$(BUILD)/io_exit_test

.PHONY: ioprobe
ioprobe: $(BUILD)/ioprobe
$(BUILD)/ioprobe: tools/hvprobe/ioprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: test-fpu
test-fpu: $(BUILD)/fpu_switch_test $(BUILD)/fpu_switch_init_test $(BUILD)/xsave_switch_test $(BUILD)/xsave_switch_init_test
	$(BUILD)/fpu_switch_test
	$(BUILD)/fpu_switch_init_test
	$(BUILD)/xsave_switch_test
	$(BUILD)/xsave_switch_init_test
$(BUILD)/xsave_switch_test: tools/tests/fpu_switch_test.cpp tools/tests/fpu_switch_test.S AMDHV/src/fpu_switch.inc AMDHV/src/vcpu_state.hpp AMDHV/src/xstate_policy.hpp
	clang++ -arch x86_64 -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -DAMDHV_TEST_EXTENDED_FP -Itools/tests/stubs -IAMDHV/src tools/tests/fpu_switch_test.cpp tools/tests/fpu_switch_test.S -o $@
$(BUILD)/xsave_switch_init_test: tools/tests/fpu_switch_test.cpp tools/tests/fpu_switch_test.S AMDHV/src/fpu_switch.inc AMDHV/src/vcpu_state.hpp AMDHV/src/xstate_policy.hpp
	clang++ -arch x86_64 -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -DAMDHV_TEST_EXTENDED_FP -DAMDHV_TEST_FP_INIT -Itools/tests/stubs -IAMDHV/src tools/tests/fpu_switch_test.cpp tools/tests/fpu_switch_test.S -o $@
$(BUILD)/fpu_switch_test: tools/tests/fpu_switch_test.cpp tools/tests/fpu_switch_test.S AMDHV/src/fpu_switch.inc AMDHV/src/vcpu_state.hpp AMDHV/src/xstate_policy.hpp
	clang++ -arch x86_64 -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Itools/tests/stubs -IAMDHV/src tools/tests/fpu_switch_test.cpp tools/tests/fpu_switch_test.S -o $@

.PHONY: fpprobe
fpprobe: $(BUILD)/fpprobe
$(BUILD)/fpprobe: tools/hvprobe/fpprobe.cpp tools/tests/fpu_switch_test.S AMDHV/src/fpu_switch.inc tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -DAMDHV_TEST_RUN_TRAP -o $@ tools/hvprobe/fpprobe.cpp tools/tests/fpu_switch_test.S -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

$(BUILD)/fpu_switch_init_test: tools/tests/fpu_switch_test.cpp tools/tests/fpu_switch_test.S AMDHV/src/fpu_switch.inc AMDHV/src/vcpu_state.hpp AMDHV/src/xstate_policy.hpp
	clang++ -arch x86_64 -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -DAMDHV_TEST_FP_INIT -Itools/tests/stubs -IAMDHV/src tools/tests/fpu_switch_test.cpp tools/tests/fpu_switch_test.S -o $@

.PHONY: pagingcontrolprobe
pagingcontrolprobe: $(BUILD)/pagingcontrolprobe
$(BUILD)/pagingcontrolprobe: tools/hvprobe/pagingcontrolprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: nxprobe
nxprobe: $(BUILD)/nxprobe
$(BUILD)/nxprobe: tools/hvprobe/nxprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: budgetprobe
budgetprobe: $(BUILD)/budgetprobe
$(BUILD)/budgetprobe: tools/hvprobe/budgetprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: flagsprobe
flagsprobe: $(BUILD)/flagsprobe
$(BUILD)/flagsprobe: tools/hvprobe/flagsprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: tlbprobe
tlbprobe: $(BUILD)/tlbprobe
$(BUILD)/tlbprobe: tools/hvprobe/tlbprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: debugprobe test-debug
debugprobe: $(BUILD)/debugprobe
$(BUILD)/debugprobe: tools/hvprobe/debugprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@
test-debug:
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/debug_emulation_test.cpp -o $(BUILD)/debug_emulation_test
	$(BUILD)/debug_emulation_test

.PHONY: eventprobe test-event
eventprobe: $(BUILD)/eventprobe
$(BUILD)/eventprobe: tools/hvprobe/eventprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@
test-event:
	clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -IAMDHV/src tools/tests/event_emulation_test.cpp -o $(BUILD)/event_emulation_test
	$(BUILD)/event_emulation_test

.PHONY: fastprobe
fastprobe: $(BUILD)/fastprobe
$(BUILD)/fastprobe: tools/hvprobe/fastprobe.cpp tools/hvprobe/entitlements.plist
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@

.PHONY: nmiprobe
nmiprobe: $(BUILD)/nmiprobe
$(BUILD)/nmiprobe: tools/hvprobe/nmiprobe.cpp tools/hvprobe/entitlements.plist
	@mkdir -p $(BUILD)
	clang++ -arch x86_64 -std=c++17 -O1 -Wall -Wextra -o $@ $< -framework Hypervisor
	codesign -f -s - --entitlements tools/hvprobe/entitlements.plist $@
