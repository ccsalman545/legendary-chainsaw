# legendary-chainsaw — V4L2 Camera → HTTP/WebSocket Streaming Server

A portable, dependency-light **C11** application that captures live video from a
Linux **V4L2** camera (a USB webcam on `/dev/video0`) and streams it to a web
browser in real time over **HTTP + WebSocket**. The browser receives raw **YUYV
4:2:2** frames, converts them to RGB in JavaScript, and draws them onto an HTML5
`<canvas>`.

The repository also contains the earlier TCP-sender prototypes that led up to
the final WebSocket implementation, kept for reference.

---

## Table of Contents

1. [Overview](#overview)
2. [Features](#features)
3. [Architecture](#architecture)
4. [Repository Layout](#repository-layout)
5. [Requirements](#requirements)
6. [Building](#building)
7. [Running](#running)
8. [HTTP & WebSocket API](#http--websocket-api)
9. [Frame Wire Protocol](#frame-wire-protocol)
10. [Component Reference](#component-reference)
11. [Legacy TCP Prototypes](#legacy-tcp-prototypes)
12. [Network Setup](#network-setup)
13. [Troubleshooting](#troubleshooting)
14. [Limitations & Future Work](#limitations--future-work)
15. [License](#license)

---

## Overview

The program runs on a Linux PC with an attached UVC webcam and:

1. Opens the camera with the **V4L2** API, negotiating **YUYV 4:2:2**,
   **640×480 @ 30 FPS**, using **memory-mapped (MMAP)** capture buffers.
2. Runs a dedicated **capture worker thread** that continuously dequeues frames
   from the driver and pushes copies of them into a bounded **frame queue**.
3. Runs a **Mongoose**-based HTTP server that:
   - serves a self-contained HTML page at `/`,
   - exposes a JSON status endpoint at `/status`,
   - upgrades connections at `/ws` to WebSockets,
   - pushes captured frames to the connected WebSocket client as **binary**
     messages.
4. The browser page decodes each binary frame (a 28-byte header followed by the
   raw YUYV payload), converts YUYV → RGB, and displays the live video on a
   canvas with a frame counter and FPS readout.

This design cleanly separates the hardware capture layer (V4L2), the
concurrency layer (worker thread + queue), the transport layer (TCP or
WebSocket), and the presentation layer (HTML/JS in the browser).

---

## Features

- **Native V4L2 capture** — no GStreamer, OpenCV, or ffmpeg dependencies.
- **YUYV 4:2:2** streaming at 640×480 / 30 FPS (driver-negotiated).
- **Zero-copy camera reads** — frames come straight from MMAP buffers; only the
  queued copy is heap-allocated.
- **Multithreaded pipeline** — capture happens on a separate pthread so the
  network loop never blocks on the camera.
- **Bounded frame queue** — drops the oldest-unconsumed frames gracefully when
  a client cannot keep up (fixed capacity of 3).
- **Embedded web UI** — a single static HTML string with no external assets.
- **Clean modular C API** — opaque structs and header-only interfaces per module.
- **Strict compiler flags** — `-Wall -Wextra -Wpedantic` on C11.
- **Zero third-party build step** — Mongoose is vendored as a single `.c`/`.h`.

---

## Architecture

```text
                        ┌─────────────────────────────────────────────┐
                        │                Browser (client)             │
                        │  YUYV → RGB → <canvas>  |  frame/FPS HUD    │
                        └──────────────────▲──────────────────────────┘
                                           │ WebSocket (binary frames)
                                           │ HTTP (HTML page / status)
┌──────────────────────────────────────────┴───────────────────────────┐
│                        HttpServer  (Mongoose 7.23)                   │
│   mg_mgr event loop ──► http_event_handler                           │
│        │                                                             │
│        │   /          /status     /ws (upgrade)                      │
│        │   HTML page   JSON        │                                 │
│        │                           ▼                                 │
│        │                   FrameStream (28-byte header + YUYV)       │
│        ▲                                                             │
│        │ drains queue each poll (10 ms)                              │
│        │                                                             │
│   FrameQueue (capacity 3, circular, owns copies)                     │
│        ▲                                                             │
│        │ push (copy frame)                                           │
│  ┌─────┴──────────────────────────────────────────────────────────┐  │
│  │            CameraWorker (pthread)                              │  │
│  │   camera_capture() ─► copy into queue ─► release V4L2 buffer   │  │
│  └─────▲──────────────────────────────────────────────────────────┘  │
│        │ VIDIOC_DQBUF / VIDIOC_QBUF                                  │
│  ┌─────┴──────────────────────────────────────────────────────────┐  │
│  │               Camera (V4L2, MMAP, /dev/video0)                 │  │
│  │   YUYV 4:2:2 · 640×480 · 30 FPS · 4 mmap buffers               │  │
│  └─────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

**Data flow**

1. `camera_capture()` waits on the V4L2 file descriptor with `select()`, dequeues
   a filled buffer (`VIDIOC_DQBUF`), and returns a `Frame` whose `data` pointer
   refers directly into a driver MMAP buffer.
2. `CameraWorker` copies that frame into the `FrameQueue` (so the queue owns its
   own buffer), then immediately returns the MMAP buffer to the driver
   (`VIDIOC_QBUF`).
3. The server's event loop polls Mongoose every 10 ms, then drains every pending
   frame from the queue and hands it to `FrameStream`.
4. `FrameStream` prepends a 28-byte `FramePacketHeader` and sends the whole
   packet as **one** WebSocket binary message.
5. The browser parses the header, validates the magic, converts the YUYV payload
   to RGB, and paints the canvas.

---

## Repository Layout

```text
legendary-chainsaw/
├── Makefile                       # Build system (gcc, C11)
├── .gitignore                     # Excludes build/, *.yuyv, old binaries
├── README.md                      # This file
│
├── include/                       # Public module interfaces
│   ├── camera_v4l2.h             #   Camera (V4L2) API
│   ├── camera_worker.h           #   Capture worker-thread API
│   ├── frame.h                   #   Frame struct + luminance helper
│   ├── frame_queue.h             #   Bounded frame queue API
│   ├── frame_stream.h            #   WebSocket frame-packet API
│   ├── http_server.h             #   HTTP/WebSocket server API
│   └── transport_tcp.h           #   Raw TCP client API (legacy path)
│
├── src/                           # Implementation
│   ├── http_main.c               #   Entry point for the HTTP/WS server
│   ├── http_server.c             #   Mongoose integration + embedded web UI
│   ├── http_server_http_only.c   #   (Checkpoint) HTTP-only server, not built
│   ├── http_server_ws_checkpoint.c#  (Checkpoint) HTTP+WS+WebRTC signaling
│   ├── camera_v4l2.c             #   V4L2 capture (open/start/capture/release)
│   ├── camera_worker.c           #   Capture thread implementation
│   ├── frame.c                   #   frame_average_luminance()
│   ├── frame_queue.c             #   Circular buffer queue
│   ├── frame_stream.c            #   Frame packet framing + WebSocket send
│   ├── main.c                    #   (Legacy) TCP sender entry point
│   └── transport_tcp.c           #   (Legacy) TCP client implementation
│
├── third_party/
│   └── mongoose/                 # Mongoose 7.23 (vendored)
│       ├── mongoose.c
│       └── mongoose.h
│
├── docs/
│   └── 04_http_websocket_test.md #   Stage 4 notes (network topology)
│
└── camera_capture.c              # (Prototype) capture-to-file, not built
    camera_sender.c               # (Prototype) raw-TCP sender, not built
```

> `http_server_http_only.c`, `http_server_ws_checkpoint.c`, `main.c`,
> `camera_capture.c`, and `camera_sender.c` are **not** part of the current
> `Makefile` build. They are earlier development stages kept for reference.

---

## Requirements

- **Linux** with the V4L2 kernel API (any modern desktop/server distro).
- A **UVC webcam** exposed at `/dev/video0` supporting `V4L2_CAP_VIDEO_CAPTURE`
  and `V4L2_CAP_STREAMING`, and accepting **YUYV** output.
- **GCC** (or a compatible C11 compiler) and **GNU Make**.
- **pthread** (linked automatically via `-pthread`).
- A modern web browser (for viewing the stream).

No external libraries are required — Mongoose is vendored in-repo.

---

## Building

From the repository root:

```sh
make            # builds build/http_server (the only default target)
```

That compiles the HTTP/WebSocket server from these sources:

```text
src/http_main.c
src/http_server.c
src/frame_stream.c
src/frame_queue.c
src/camera_v4l2.c
src/camera_worker.c
third_party/mongoose/mongoose.c
```

Useful targets:

| Target  | Effect                                             |
|---------|----------------------------------------------------|
| `make`  | Build `build/http_server` (alias of `make http`).  |
| `make http` | Build the HTTP/WebSocket server.               |
| `make clean` | Remove the `build/` directory.                |

Compilation flags applied (see `Makefile`):

```text
-std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2
```

---

## Running

```sh
./build/http_server
```

On startup the server:

1. Opens and configures `/dev/video0` (YUYV, 640×480, 30 FPS, 4 MMAP buffers).
2. Starts the capture worker thread.
3. Listens for HTTP on `http://192.168.1.10:8080`.

Expected console output (abridged):

```text
Camera: <device name>
Resolution : 640x480
Pixel format: YUYV
Frame size : 614400 bytes
Frame rate : 30 FPS
Buffer 0 mapped: 614400 bytes
...
HTTP server started successfully
HTTP endpoint: http://192.168.1.10:8080/
WebSocket endpoint: ws://192.168.1.10:8080/ws
```

Then open **http://192.168.1.10:8080/** in a browser on the receiver machine and
click **“Connect WebSocket”**. Live video should appear on the canvas with a
running frame count and FPS meter.

Stop the server with **Ctrl+C**.

> **Hardcoded configuration** (edit `src/http_main.c` / `src/http_server.c`):
>
> | Setting            | Value             | Where                        |
> |--------------------|-------------------|------------------------------|
> | Listen address     | `192.168.1.10`    | `src/http_main.c`            |
> | Port               | `8080`            | `src/http_main.c`            |
> | Camera device      | `/dev/video0`     | `src/http_server.c`          |
> | Width × Height     | `640 × 480`       | `src/http_server.c`          |
> | Frame rate         | `30` FPS          | `src/http_server.c`          |
> | Frame queue size   | `3`               | `src/http_server.c`          |

---

## HTTP & WebSocket API

### `GET /`

Returns the embedded HTML page (UTF-8). It contains a canvas, a
**Connect/Disconnect WebSocket** button, and live status labels
(WebSocket state, frame count, FPS). All YUYV→RGB conversion happens
client-side in JavaScript.

### `GET /status`

Returns a static JSON summary:

```json
{
  "status": "online",
  "camera": "/dev/video0",
  "format": "YUYV",
  "width": 640,
  "height": 480,
  "fps": 30
}
```

### `GET /ws` (WebSocket upgrade)

Upgrades the connection to a WebSocket.

- **On open** — the server sends one text message:
  `Camera WebSocket connected`.
- **Text messages from the client** — are echoed back as text.
- **Binary messages from the server** — each is one camera frame packet
  (see [Frame Wire Protocol](#frame-wire-protocol)).
- The server supports **one** attached streaming client at a time; a second
  `MG_EV_WS_OPEN` replaces the first.

Anything else returns `404 Not Found`.

---

## Frame Wire Protocol

Each frame is sent as a single **WebSocket binary message** composed of a
28-byte header followed by the raw YUYV payload:

```text
+------------------------------+---------------------------+
| FramePacketHeader  (28 bytes)| YUYV frame data           |
+------------------------------+---------------------------+
                                └── frame->size bytes
```

### `FramePacketHeader` layout (7 × `uint32_t`, little-endian on x86)

| Offset | Field         | Description                                      |
|--------|---------------|--------------------------------------------------|
| 0      | `magic`       | Always `0x4652414D` (ASCII `FRAM`)               |
| 4      | `width`       | Frame width in pixels                            |
| 8      | `height`      | Frame height in pixels                           |
| 12     | `pixel_format`| V4L2 pixel format (`V4L2_PIX_FMT_YUYV`)          |
| 16     | `stride`      | Bytes per line (driver-negotiated)               |
| 20     | `frame_size`  | Payload length in bytes                          |
| 24     | `sequence`    | V4L2 buffer sequence number                      |

The browser validates `magic == 0x4652414D`, checks
`28 + frame_size <= byteLength`, and requires
`frame_size >= width * height * 2` before decoding.

### YUYV 4:2:2 payload

Two pixels per 4 bytes — `Y0 U0 Y1 V0`:

```text
Y0 U0 Y1 V0 Y2 U1 Y3 V1 ...
```

The JavaScript decoder applies standard BT.601 coefficients:

```text
R = (298·(Y-16) + 409·(V-128) + 128) >> 8
G = (298·(Y-16) - 100·(U-128) - 208·(V-128) + 128) >> 8
B = (298·(Y-16) + 516·(U-128) + 128) >> 8
```

---

## Component Reference

### `Camera` (`camera_v4l2.c`)

Owns the V4L2 device. Responsibilities:

- `camera_open()` — opens `/dev/video0` (non-blocking), verifies capture +
  streaming capability, negotiates YUYV/640×480/30 FPS, requests **4 MMAP
  buffers**, and maps them into user space.
- `camera_start()` — queues all buffers and issues `VIDIOC_STREAMON`.
- `camera_capture()` — waits up to 2 s on the fd with `select()`, dequeues a
  filled buffer (`VIDIOC_DQBUF`), and populates a `Frame`.

  **Return codes:** `1` = frame ready · `0` = no frame / interrupted ·
  `-1` = error (or timeout).

- `camera_release_frame()` — returns the frame's buffer to the driver
  (`VIDIOC_QBUF`).
- `camera_close()` — `STREAMOFF`, unmaps buffers, closes the fd, frees state.

> Frames returned by `camera_capture()` point directly into a driver MMAP
> buffer and remain valid only until `camera_release_frame()` is called.

### `Frame` (`frame.h`)

Plain struct carrying width, height, pixel format, stride, byte size, V4L2
sequence number, a `gettimeofday()`-based microsecond timestamp, the source
buffer index, and a `uint8_t *data` pointer.

`frame_average_luminance()` averages every second byte of a YUYV buffer (the Y
samples) — used by the legacy TCP sender for logging.

### `FrameQueue` (`frame_queue.c`)

A fixed-capacity **circular buffer** (`capacity = 3`). Every pushed frame is
deep-copied (`malloc` + `memcpy`), so the queue owns its image data independent
of the driver's MMAP buffer. `pop()` **transfers ownership** of `frame->data` to
the caller, who must `free()` it. A full queue rejects the push and the worker
drops the frame.

> The queue has **no locking**. It is intended for the single-producer
> (capture thread) / single-consumer (server loop) pattern used here. Do not
> share it between multiple producers or consumers without adding a mutex.

### `CameraWorker` (`camera_worker.c`)

A pthread wrapper around the capture loop:

1. `camera_capture()` → on success, `frame_queue_push()` a copy.
2. `camera_release_frame()` the original MMAP buffer.
3. On error/empty, sleep 10 ms to avoid a tight loop.

Lifecycle: `create → start → stop → join → destroy`.

### `FrameStream` (`frame_stream.c`)

Tracks the currently attached WebSocket connection (a non-owning Mongoose
pointer). `frame_stream_send()` builds the 28-byte header + YUYV payload and
sends it with `mg_ws_send(..., WEBSOCKET_OP_BINARY)`.

### `HttpServer` (`http_server.c`)

The orchestrator. It owns the Mongoose manager, the camera, the worker, the
queue, and the frame stream. Its `run()` loop:

```c
while (server->running) {
    mg_mgr_poll(&server->mgr, 10);   // network events, 10 ms
    http_send_frames(server);        // drain queue → WebSocket
}
```

It also embeds the entire web UI as a single C string (`HTML_PAGE`).

### `transport_tcp` (`transport_tcp.c`, legacy path)

A minimal blocking IPv4 TCP client: `tcp_connect()` + `tcp_send_all()` (loops on
`send()` until the whole buffer is transmitted, handling `EINTR`).

---

## Legacy TCP Prototypes

Before the WebSocket server, the project streamed raw YUYV frames over plain TCP.
These files remain in the tree for reference and are **not** built by `make`.

| File               | Purpose                                                                  |
|--------------------|--------------------------------------------------------------------------|
| `camera_capture.c` | Standalone capture: grabs 100 frames, saves the first to `frame_000.yuyv`. |
| `camera_sender.c`  | Standalone capture + raw TCP sender to `192.168.1.20:5000`.              |
| `src/main.c`       | Modular rework of the sender using `camera_v4l2` + `transport_tcp`; sends 100 frames to `192.168.1.20:5000`. |

These send **bare YUYV bytes** with no header/framing, so they expect a matching
receiver that already knows the resolution.

---

## Network Setup

From the project's stage notes (`docs/04_http_websocket_test.md`):

| Device    | IP Address    | Role                        |
|-----------|---------------|-----------------------------|
| Fedora PC | `192.168.1.10`| Camera + HTTP/WebSocket server |
| Ubuntu PC | `192.168.1.20`| Receiver / browser client    |

Endpoints:

- HTTP page: `http://192.168.1.10:8080/`
- Status: `http://192.168.1.10:8080/status`
- WebSocket: `ws://192.168.1.10:8080/ws`

If the machines are on a different subnet, edit the address in `src/http_main.c`
(or bind to `0.0.0.0` to listen on all interfaces) and adjust the browser URL
accordingly. Ensure the firewall allows TCP/8080.

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `Cannot open camera /dev/video0: No such file or directory` | Wrong device node. Find it with `ls -l /dev/video*` or `v4l2-ctl --list-devices`, then update `CAMERA_DEVICE`. |
| `Device does not support video capture` | The node is not a capture device (e.g. it's a metadata node). |
| `Camera did not accept YUYV format` | The camera lacks YUYV. Try a different format or camera. Check with `v4l2-ctl --list-formats-ext`. |
| `Camera capture timeout` | No frames arriving — device busy (used by another process) or unsupported resolution/framerate. |
| `Failed to start HTTP server` | Address not available on this host. Use `0.0.0.0` or a local IP; check `ip addr`. |
| Browser shows “WebSocket: Error” | Wrong host/IP or the port is blocked by a firewall. |
| Video freezes / low FPS | The frame queue (capacity 3) drops frames when the client is slow — expected back-pressure behavior. |
| `Frame queue full; dropping frame` spam | The consumer isn't draining (no client connected, or slow network). Frames are dropped intentionally. |

Quick camera diagnostics (if `v4l-utils` is installed):

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
v4l2-ctl -d /dev/video0 --set-fmt-video=width=640,height=480,pixelformat=YUYV
```

---

## Limitations & Future Work

- **Single WebSocket client.** `FrameStream` stores one connection; a second
  client silently replaces the first.
- **No authentication / TLS.** The server is plain HTTP+WS, intended for a
  trusted LAN.
- **Unlocked frame queue.** Relies on the single-producer/single-consumer
  design; a mutex/condition variable would make it safe for multiple consumers.
- **Host-order wire header.** The 28-byte header is written with `memcpy` from a
  host-endian struct and decoded as little-endian in the browser — correct on
  x86/ARM little-endian, but would need explicit serialization for portability.
- **Hardcoded configuration.** Device, resolution, FPS, and bind address are
  compile-time constants; command-line flags or a config file would be a natural
  next step.
- **No audio, no compression.** Raw YUYV is bandwidth-hungry
  (~640×480×2×30 ≈ 18.4 MB/s); MJPEG/H.264 would be more efficient.
- **Checkpoint files** (`http_server_http_only.c`, `http_server_ws_checkpoint.c`)
  could be removed or moved to a `docs/` archive.

---

## License

- The application code in this repository is provided for educational purposes
  without an explicit license; add one before redistribution.
- The vendored **Mongoose 7.23** library
  (`third_party/mongoose/`) is © Cesanta Software Limited and is
  **dual-licensed**: **GPL-2.0-only** or a **commercial license**
  (https://www.mongoose.ws/licensing/). If you distribute binaries that link
  Mongoose under terms other than GPLv2, you must obtain a commercial license
  from Cesanta.
