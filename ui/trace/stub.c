/* SPDX-License-Identifier: MIT */
/* Link-time stand-ins for the device's glibc libc.so.6 and libdl.so.2: diskos-trace.so is linked against these only
 * to record DT_NEEDED and the symbol names; at run time the real glibc libraries supply them. Never shipped. */
#ifdef STUB_LIBDL
void *dlsym(void *h, const char *n) { (void)h; (void)n; return 0; }
#else
int *__errno_location(void) { return 0; }
#endif
