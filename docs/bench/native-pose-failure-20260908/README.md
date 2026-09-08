# User-session failure and Android shutdown workaround

A native-resolution Pico session on 2026-09-08 repeatedly rejected encoded
frames after its first image (`assembly warp params too large`). The NX Warp
encoder staging fix and native pose-transition regression are recorded in
[NX Warp](https://github.com/nerdrx/nx-warp/tree/main/bench/results/240fps-2026-09-08/native-pose-transition).

The session later entered a reconnect refusal loop. The server now includes
the compatibility refusal category in its log instead of discarding the reason.
The original logs did not identify the mismatched field, nor prove that frame
rejection caused the socket shutdown.

The client reported `Caught exception in application_thread: "Broken pipe"`.
Its final SIGSEGV was a null dereference in Android `RefBase::decStrong` during
`exit` → `__cxa_finalize`, after scoped application teardown. The Android-only
termination point now uses `std::_Exit(EXIT_SUCCESS)`: it preserves the existing
intent to terminate the process after Activity destruction, but skips global
finalizers, atexit callbacks and implicit stream flushing. Explicit scoped
application destruction and the Activity event-drain remain unchanged.

This is a targeted shutdown workaround, **not a reconnect repair** or proof of
live stability. It has compiled and been installed; the exact disconnect/exit
scenario has not yet been reproduced with the replacement on Pico.

The APK was packaged from the existing unsigned v4 APK with the rebuilt,
stripped native library, then aligned, signed and signature-verified. It keeps
the existing package ID and version name. Installed APK SHA-256:
`365ffff5548f3faef099405281ef9eb57dcedad1e4f40e887346594739821a76`.
