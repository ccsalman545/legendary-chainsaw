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
