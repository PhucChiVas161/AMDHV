// Small in-kernel text log, readable without root via `sysctl -n debug.amdhv.log`.
// (IOLog output does not reliably show up in `log show` on this system.)
#pragma once
#include <stddef.h>

struct sysctl_req;

namespace amdhv {
bool klogInit();
void klogFree();
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klogClear();
int  klogCopyOut(struct sysctl_req *req);
} // namespace amdhv
