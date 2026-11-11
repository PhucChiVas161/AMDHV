#!/bin/zsh
# Show AMDHV state (no root needed).
kmutil showloaded --list-only 2>/dev/null | grep com.fixvm || echo "AMDHV not loaded"
echo "--- self-test: $(sysctl -n debug.amdhv.result 2>/dev/null || echo n/a)"
echo "--- takeover: $(sysctl -n debug.amdhv.takeover 2>/dev/null || echo n/a), kern.hv_support=$(sysctl -n kern.hv_support)"
echo "--- IORegistry"
ioreg -r -c AMDHV -l -w0 | grep -E '"(SVM|SelfTest)' | grep -v "SelfTest CPUs"
echo "--- per CPU (first 3)"
ioreg -r -c AMDHV -l -w0 | grep '"SelfTest CPUs"' | sed 's/},{/}\n{/g' | head -3
echo "--- AMDHV log (last 30 lines)"
sysctl -n debug.amdhv.log 2>/dev/null | tail -30
