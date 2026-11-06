# SoftFloat 3e (vendored)

Source: `ucb-bar/berkeley-softfloat-3`, Release 3e (master snapshot downloaded
2026-10-27). License: BSD-3-Clause, see `COPYING.txt`.

Only the IEEE arithmetic core is used. It is the reference implementation for
the guest x87, not a replacement for any game code. The `8086-SSE`
specialization is used because its NaN and tininess behaviour matches the x86
target that Warcraft III was built for.

Build notes for Android/ARM64:

* define `SOFTFLOAT_FAST_INT64` and `LITTLEENDIAN`;
* include `source/8086-SSE` for `specialize.h`;
* define `THREAD_LOCAL` as `_Thread_local` (each guest CPU thread has its own
  rounding mode, precision and exception flags);
* compile `source/*.c`; no host floating point is used by the extF80 paths.

The local reference checkout is `E:\projects\refs\softfloat`; this copy is the
one the project builds.
