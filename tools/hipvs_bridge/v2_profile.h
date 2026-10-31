/* V2 performance instrumentation.
 *
 * Compiled in ONLY with -DOB_V2_PROFILE. Without that define this header
 * expands to nothing and the baseline behaviour is bit-identical to the frozen
 * V5 source.
 *
 * Output: one line per measured stage, appended to the file named by the
 * OB_V2_PROFILE_FILE environment variable (default /tmp/ob_v2_profile.log).
 *
 *   <mono_us> <stage> wall_us=<n> gpu_us=<n> a=<n> b=<n> note=<s>
 *
 * wall_us : host wall-clock duration of the measured section (CLOCK_MONOTONIC).
 * gpu_us  : GPU elapsed time between two hipEvents recorded on the default
 *           stream around the section. This is the GPU-busy part only.
 *           -1 means "not measured".
 * a, b    : stage-specific integers (dimensions, counts, byte sizes).
 *
 * The difference (wall_us - gpu_us) is the host-side and synchronisation
 * overhead of the section. The two are never summed naively, because a
 * section may overlap GPU execution with host work.
 */
#ifndef OB_V2_PROFILE_H
#define OB_V2_PROFILE_H

#ifdef OB_V2_PROFILE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

static inline long v2_prof_now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

/* Append-only fd; O_APPEND keeps concurrent writers from interleaving lines. */
static inline int v2_prof_fd(void) {
  static int fd = -2;
  if (fd == -2) {
    const char *path = getenv("OB_V2_PROFILE_FILE");
    if (path == NULL || path[0] == 0) { path = "/tmp/ob_v2_profile.log"; }
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { fd = -1; }
  }
  return fd;
}

static inline void v2_prof_emit(const char *stage, long wall_us, long gpu_us,
                                 long a, long b, const char *note) {
  int fd = v2_prof_fd();
  if (fd < 0) { return; }
  char buf[512];
  int n = snprintf(buf, sizeof(buf),
                   "%ld %s wall_us=%ld gpu_us=%ld a=%ld b=%ld note=%s\n",
                   v2_prof_now_us(), stage, wall_us, gpu_us, a, b,
                   (note && note[0]) ? note : "-");
  if (n > 0) {
    ssize_t ignored = write(fd, buf, (size_t)n);
    (void)ignored;
  }
}

#else  /* !OB_V2_PROFILE */

/* Zero-overhead stubs so the unguarded call sites in the adaptors and the
 * bridge compile with the default configuration (OB_V2_PROFILE=OFF):
 * no clock_gettime, no GPU events, no file writes. The emit args are still
 * voided (not swallowed) so the surrounding _v2_*_t0 stage variables stay
 * "used" and -Wall stays quiet; everything folds away at -O2. */
#define v2_prof_now_us() (0L)
#define v2_prof_emit(stage, wall_us, gpu_us, a, b, note) \
  do { (void)(stage); (void)(wall_us); (void)(gpu_us); \
       (void)(a); (void)(b); (void)(note); } while (0)

#endif /* OB_V2_PROFILE */

#endif /* OB_V2_PROFILE_H */
