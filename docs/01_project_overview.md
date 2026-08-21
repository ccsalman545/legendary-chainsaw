# Project overview

legendary-chainsaw is a small, self-contained C11 Linux webcam-to-browser streamer. It captures V4L2 YUYV frames and sends them as binary WebSocket messages; the browser converts them to RGB and paints a canvas.

## Key features
| Feature | Description |
|---|---|
| Capture | V4L2 MMAP camera buffers, normally `/dev/video0` |
| Delivery | HTTP page plus WebSocket `/ws` |
| Format | Raw YUYV 4:2:2; browser-side conversion |
| Concurrency | Dedicated capture worker and network loop |
| Resource use | Bounded queue (capacity selected by server) drops stale work |
| Dependencies | C11/POSIX and vendored Mongoose; no runtime player |

It exists as a readable learning example and lightweight LAN camera. You do not need OpenCV, FFmpeg, GStreamer, a frontend toolchain, a database, or files outside the executable.

## Flow
```text
camera (/dev/video0) -> V4L2/MMAP -> worker -> bounded queue -> Mongoose HTTP/WS -> browser canvas
                                      ^                                  |
                                      +------ driver buffer returned -----+
```

## Requirements
Linux with V4L2, a compatible camera, pthreads, C11 compiler, GNU make, and a modern WebSocket-capable browser. Supported CPU families include x86-64 and ARM.

## Layout
```text
include/{frame,camera_v4l2,camera_worker,frame_queue,frame_stream,http_server}.h
src/{camera_v4l2,camera_worker,frame,frame_queue,frame_stream,http_server,http_main}.c
third_party/mongoose/  vendored networking library
camera_capture.c camera_sender.c  standalone/experimental programs
Makefile  README.md  docs/
```
