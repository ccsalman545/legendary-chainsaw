# Troubleshooting

## Build (all six common cases)
| Symptom | Fix |
|---|---|
| `gcc: not found` | install `build-essential`/`gcc make`/`base-devel`/`build-base` |
| `No rule ... mongoose` | run `make` at repository root and retain `third_party/` |
| missing `linux/videodev2.h` | install kernel/libc development headers |
| undefined pthread symbols | use the supplied Makefile; preserve `-pthread` |
| permission denied in build | fix checkout permissions; do not build as root |
| sanitizer link failure | pass sanitizer flags to both CFLAGS and LDFLAGS |

## Camera (six cases)
1. `open /dev/video0 failed`: check `ls -l /dev/video*`, connect camera, set `CAMERA_DEVICE`.
2. Permission denied: join `video` group and log in again (`sudo usermod -aG video $USER`).
3. Unsupported format: inspect `v4l2-ctl --list-formats-ext -d /dev/video0`; use a compatible mode/driver.
4. Device busy: close Zoom/cheese/ffmpeg and inspect `fuser /dev/video0`.
5. No frames: check USB power, `dmesg`, and `v4l2-ctl --stream-mmap`.
6. Wrong camera: enumerate devices and select `/dev/video1` or stable udev path.

## Network (four cases)
1. Address in use: `ss -ltnp | grep 8080`, stop old server or `-p 9090`.
2. Remote connection refused: bind to LAN/`0.0.0.0` and allow TCP port in firewall.
3. Wrong IP: `ip -br address`; use the server's reachable interface address.
4. WebSocket fails: use `http(s)` page's matching `ws(s)` URL and check `/ws`.

## Browser and performance
For a blank page, use a current browser and inspect DevTools console; for disconnected WS verify `/status` and firewall. If frames are rejected, reload and confirm camera format. High CPU/bandwidth is expected for raw YUYV: lower camera resolution/FPS or use a faster LAN. Lag means a slow client; the bounded queue intentionally drops stale frames.

## Diagnostics
```sh
v4l2-ctl --all -d /dev/video0; ls -l /dev/video*; dmesg | tail
ip -br address; ss -ltnp | grep 8080; curl -v http://127.0.0.1:8080/status
uname -a; id; getent group video
make clean && make info && make V=1
```

| Goal | Quick fix |
|---|---|
| choose camera | `CAMERA_DEVICE=/dev/video1` |
| choose port | `LISTEN_PORT=9090` or `-p 9090` |
| local-only | `-a 127.0.0.1` |
| LAN access | `-a 0.0.0.0`, open firewall |
| fresh rebuild | `make clean && make` |
