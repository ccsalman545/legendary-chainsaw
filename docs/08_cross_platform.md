# Cross-platform details

| Platform | libc/CPU | Status |
|---|---|---|
| Debian | glibc/x86,ARM | supported |
| Ubuntu | glibc/x86,ARM | supported |
| Fedora | glibc/x86,ARM | supported |
| Arch | glibc/x86,ARM | supported |
| openSUSE | glibc/x86,ARM | supported |
| Alpine | musl/x86,ARM | supported |
| Raspberry Pi OS | glibc/ARM | supported with V4L2 |
| Gentoo | glibc | supported |
| Rocky/Alma | glibc | supported |
| Void | musl/glibc | supported |
| Linux Mint | glibc | supported |

| Architecture | Endian | Notes |
|---|---|---|
| x86-64, ARMv7, AArch64 | little | primary targets |
| big-endian systems | big | packet header/browser parser needs negotiation |

The code uses POSIX/V4L2, not Windows or macOS camera APIs. glibc and musl both provide required POSIX/V4L2 interfaces; install their development headers. The Makefile uses `CC ?= gcc`, `CFLAGS ?= -O2`, portable include paths, C11, and pthreads. It does not force `-include alloca.h`; source does not inject a global `#define`, improving strict libc compatibility.

| Header | Provider |
|---|---|
| `linux/videodev2.h` | kernel headers |
| `pthread.h`, `sys/mman.h` | libc |
| `mongoose.h` | vendored tree |

## Walkthroughs
On each distro install compiler, make, git, and v4l-utils using the commands in [building](02_building.md), then `make`, `v4l2-ctl --list-devices`, and `./build/http_server`. Runtime configuration is portable through `BIND_ADDRESS`, `LISTEN_PORT`, and `CAMERA_DEVICE`.

RPi cross build: `make CC=aarch64-linux-gnu-gcc`, ensure target kernel headers and a V4L2 camera, copy `build/http_server`, then run it on the Pi. Clang: `make clean && CC=clang make`.

Known limits: Linux only, V4L2-only capture, raw bandwidth, one client, native-endian protocol, and device drivers may not support requested YUYV dimensions.

## ABI and packaging notes
Use the same architecture and libc family for all dynamically linked dependencies. `sizeof(size_t)` affects internal frame bookkeeping but not the 28-byte wire header, whose fields are fixed-width integers. `uint64_t` metadata remains internal to the C `Frame`; the wire sequence is intentionally truncated to 32 bits. Big-endian support requires changing both sender serialization and browser parsing. Alpine containers need `/dev/video*` passed through and the container user granted the device permissions.
