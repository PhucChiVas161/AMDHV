#!/bin/zsh
# Repeatable pre-install gate. No sudo, takeover, guest execution or reboot.
set -euo pipefail
ROOT=${0:A:h:h}
cd "$ROOT"
make all check test-memory test-vcpu test-modes test-msr test-interrupt test-cr test-io test-fpu test-debug test-event test-xstate test-cpl
python3 -I -m unittest discover -s tools -p test_gen_kernsyms.py
for script in scripts/*.sh; do zsh -n "$script"; done
printf '%s\n' 'PASS: offline build/imports/ASan/UBSan/resolver/script checks; hardware results remain separate'
