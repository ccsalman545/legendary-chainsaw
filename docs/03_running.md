# Running and configuration

```sh
make && ./build/http_server
# browse to http://localhost:8080/
```
| Argument | Meaning | Default |
|---|---|---|
| `-a`, `--address ADDR` | bind address | `0.0.0.0` |
| `-p`, `--port PORT` | TCP port 1–65535 | `8080` |
| `-h`, `--help` | usage | — |

`BIND_ADDRESS`, `LISTEN_PORT`, and `CAMERA_DEVICE` configure address, port, and device. Priority is command-line (`-a/-p`) > environment > defaults; camera has environment > `/dev/video0`.

## Camera and network
```sh
v4l2-ctl --list-devices; v4l2-ctl --list-formats-ext -d /dev/video0
sudo usermod -aG video "$USER"   # log in again
CAMERA_DEVICE=/dev/video1 ./build/http_server
ip -br address
sudo ufw allow 8080/tcp             # or the equivalent firewall tool
```
Bind `127.0.0.1` for local-only, a LAN IP for one interface, or `0.0.0.0` for all interfaces. Avoid exposing it directly to the Internet.

## systemd example
```ini
[Unit]
After=network-online.target
[Service]
WorkingDirectory=/opt/legendary-chainsaw
ExecStart=/opt/legendary-chainsaw/build/http_server -a 0.0.0.0 -p 8080
Environment=CAMERA_DEVICE=/dev/video0
Restart=on-failure
User=camera
SupplementaryGroups=video
[Install]
WantedBy=multi-user.target
```

Console prints bind address, port, camera, LAN receiver URLs, client attach/detach, and errors. `0.0.0.0` means “listen on every interface”; the viewing PC must open a real LAN address printed at startup, not `0.0.0.0`. Ctrl+C/SIGTERM stops the loop and releases resources. The page connects to `ws://host:port/ws` using the page origin; `GET /` serves HTML, `GET /status` returns JSON such as `{"status":"online",...}`, and `/ws` carries binary frames. Multiple WebSocket receivers are supported at once; each frame is broadcast to every connected viewer. Multiple cameras require separate processes/ports/devices. Slow receivers drop frames instead of growing the send buffer until the socket dies.

## Configuration examples
```sh
# Same machine only
BIND_ADDRESS=127.0.0.1 LISTEN_PORT=8080 ./build/http_server
# LAN camera on a selected interface
./build/http_server --address 192.168.1.10 --port 8080
# Environment is useful for service managers
BIND_ADDRESS=0.0.0.0 LISTEN_PORT=9000 CAMERA_DEVICE=/dev/video2 ./build/http_server
```
Invalid command-line ports fail immediately. An invalid environment port falls back to the default. The program does not provide a command-line camera option; use `CAMERA_DEVICE`.

## Browser protocol behavior
The embedded page opens a WebSocket relative to the page origin, displays connection state, validates frame magic and dimensions, converts complete YUYV rows, and updates an HTML canvas. A direct `curl http://host:8080/` confirms HTTP but cannot consume the WebSocket stream. `/status` is deliberately a small informational response and is not an authentication or health guarantee for the camera.

## Service hardening suggestions
For a real deployment use a dedicated unprivileged user in `video`, `PrivateTmp=true`, a restrictive firewall, log rotation, and a reverse proxy providing TLS/authentication. Do not grant the binary unnecessary root privileges.
