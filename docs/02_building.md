# Building

## Supported distributions
| Distro | Packages |
|---|---|
| Debian/Ubuntu | `build-essential git v4l-utils` |
| Fedora | `gcc make git v4l-utils` |
| Arch | `base-devel git v4l-utils` |
| openSUSE | `gcc make git v4l-utils` |
| Alpine | `build-base git v4l-utils` |

## Prerequisites and install
```sh
# Debian/Ubuntu
sudo apt update && sudo apt install -y build-essential git v4l-utils
# Fedora
sudo dnf install -y gcc make git v4l-utils
# Arch
sudo pacman -S base-devel git v4l-utils
# openSUSE
sudo zypper install gcc make git v4l-utils
# Alpine
sudo apk add build-base git v4l-utils
```

## Source and builds
```sh
git clone https://github.com/ccsalman545/legendary-chainsaw.git
cd legendary-chainsaw
make                 # default: build/http_server
make info
make clean
CC=clang CFLAGS='-O0 -g' make
```
`CC`, `CFLAGS`, and `LDFLAGS` are overridable; the Makefile adds include paths, C11/POSIX, warnings, `_DEFAULT_SOURCE`, and `-pthread`. Debug example: `make clean && CFLAGS='-O0 -g -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined' make`.

## Make reference
| Target | Meaning |
|---|---|
| `all`/`http` | link server |
| `info` | print compiler and source configuration |
| `clean` | remove `build/` |

Compiled sources are `src/http_main.c`, `http_server.c`, `frame_stream.c`, `frame_queue.c`, `camera_v4l2.c`, `camera_worker.c`, plus `third_party/mongoose/mongoose.c`. `src/frame.c`, `main.c`, transport helpers, and alternate server checkpoints are not in this target.

## Output and portability
```text
build/http_server
build/src/*.o
build/third_party/mongoose/mongoose.o
```
Cross compile with a toolchain prefix, for example `make CC=aarch64-linux-gnu-gcc`; add target-specific `CFLAGS`/`LDFLAGS` as needed. Verify with `make info`, `file build/http_server`, and `./build/http_server -h`.

| Error | Fix |
|---|---|
| compiler not found | install the distro package above |
| `mongoose.h` missing | build from repository root; do not omit vendored tree |
| pthread link failure | retain `-pthread` in LDFLAGS |
| V4L2 header missing | install kernel/libc development headers |
| permission denied writing build | use a writable checkout, not `sudo make` |
| sanitizer link errors | put sanitizer flags in both CFLAGS and LDFLAGS |

## What `make` actually does
The pattern rule creates the corresponding directory under `build/`, compiles each translation unit with `-Iinclude -Ithird_party/mongoose -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic`, then links all objects with `-pthread`. `CFLAGS ?=` means an existing environment/command-line value is preserved, but `FINAL_CFLAGS` still appends project flags. `LDFLAGS` is appended after the required pthread flag.

For a verbose rebuild:
```sh
make clean
make --debug=b
make -n
```
The executable is a dynamically linked Linux ELF. Use `ldd build/http_server` to inspect runtime libraries and `readelf -h build/http_server` to inspect the target ABI. `strip` is optional for deployment; retain symbols in debug builds.

## Cross compilation checklist
A cross compiler alone is not enough: the sysroot must contain libc headers, Linux V4L2 headers, pthread support, and a linker. Build on the host, copy the binary and any required dynamic libraries (or use a matching target rootfs), then run `file` on the target. Camera device nodes and kernel support are target runtime concerns, not host build concerns.
