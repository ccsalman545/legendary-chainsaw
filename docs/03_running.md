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

Console prints bind address, port, camera, endpoint, client attach/detach, and errors. Ctrl+C/SIGTERM stops the loop and releases resources. The page connects to `ws://host:port/ws`; `GET /` serves HTML, `GET /status` returns JSON such as `{"status":"online",...}`, and `/ws` carries binary frames. Only one active WebSocket client is retained; multiple cameras require separate processes/ports/devices.
