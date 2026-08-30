// SPDX-License-Identifier: GPL-3.0-only

#include <pthread.h>
#include <time.h>

// Qt 6.9 and newer waits for QThread with pthread_clockjoin_np(). Clang's
// ThreadSanitizer runtime does not intercept that GNU extension yet, so it
// retains the completed thread's pthread_t and aborts when glibc reuses the
// value. TSan test processes use the intercepted blocking join instead. CTest
// retains the outer timeout that a failed Qt worker shutdown needs.
extern "C" int pthread_clockjoin_np(const pthread_t thread, void** const result, const clockid_t,
                                    const timespec*) {
    return pthread_join(thread, result);
}
