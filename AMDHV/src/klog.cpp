#include "klog.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/sysctl.h>
#include <stdarg.h>

namespace amdhv {

static constexpr size_t kLogSize = 64 * 1024;
static char   *gBuf = nullptr;
static size_t  gLen = 0;
static IOLock *gLogLock = nullptr;

static void *zalloc(size_t n) {           // IOMallocZero is not exported to third-party kexts
    void *p = IOMalloc(n);
    if (p) bzero(p, n);
    return p;
}

bool klogInit() {
    gLogLock = IOLockAlloc();
    gBuf = static_cast<char *>(zalloc(kLogSize));
    gLen = 0;
    return gLogLock && gBuf;
}

void klogFree() {
    if (gBuf) IOFree(gBuf, kLogSize);
    if (gLogLock) IOLockFree(gLogLock);
    gBuf = nullptr;
    gLogLock = nullptr;
}

void klog(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;

    IOLog("AMDHV: %s", line);
    if (!gBuf || !gLogLock) return;

    lck_mtx_lock(IOLockGetMachLock(gLogLock));
    if (gLen + (size_t)n > kLogSize) {          // drop the older half
        size_t keep = gLen / 2;
        memmove(gBuf, gBuf + gLen - keep, keep);
        gLen = keep;
    }
    memcpy(gBuf + gLen, line, (size_t)n);
    gLen += (size_t)n;
    lck_mtx_unlock(IOLockGetMachLock(gLogLock));
}

void klogClear() {
    if (!gLogLock) return;
    lck_mtx_lock(IOLockGetMachLock(gLogLock));
    gLen = 0;
    lck_mtx_unlock(IOLockGetMachLock(gLogLock));
}

int klogCopyOut(struct sysctl_req *req) {
    if (!gBuf || !gLogLock) return SYSCTL_OUT(req, "", 1);
    // Copy under the lock into a temporary buffer, then copy out without holding it.
    char *tmp = static_cast<char *>(zalloc(kLogSize + 1));
    if (!tmp) return ENOMEM;
    lck_mtx_lock(IOLockGetMachLock(gLogLock));
    size_t len = gLen;
    memcpy(tmp, gBuf, len);
    lck_mtx_unlock(IOLockGetMachLock(gLogLock));
    tmp[len] = 0;
    int err = SYSCTL_OUT(req, tmp, len + 1);
    IOFree(tmp, kLogSize + 1);
    return err;
}

} // namespace amdhv
