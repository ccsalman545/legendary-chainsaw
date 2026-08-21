# Architecture

```text
V4L2 camera -> Camera -> CameraWorker(pthread) -> FrameQueue(mutex) -> HttpServer(Mongoose loop) -> FrameStream -> browser
```

## Threads
```text
capture thread: camera_capture -> queue_push -> release buffer
network thread: mg_mgr_poll -> queue_pop -> frame_stream_send
```
Two threads keep blocking camera I/O from delaying network events. The queue bounds memory and latency.

## Data flow
1. V4L2 setup opens device, requests YUYV and MMAP buffers, queues them.
2. Capture dequeues a filled buffer, describes it as `Frame`, and the worker copies it.
3. Delivery pops frames, prepends the packet header, and sends one WS binary message.
4. JavaScript validates the header, converts YUYV to RGB, and draws canvas.

The driver owns MMAP data until release; the queue owns its `malloc` copy; the stream owns a temporary packet only during send; Mongoose owns the connection. `Frame` itself does not own `data` unless returned by a queue pop.

## FrameQueue
A circular array has head/tail/count. Push copies the frame; full queues reject/drop according to caller policy. Pop removes oldest, transfers its data pointer, and decrements count. Mutex protection makes producer/consumer access safe; there is no unbounded back-pressure.

## Packet and rendering
Header is seven native `uint32_t` values (28 bytes): magic, width, height, pixel format, stride, size, sequence. Payload is raw YUYV. For each pair: `C=Y-16`, `D=U-128`, `E=V-128`; `R=clamp((298C+409E+128)>>8)`, `G=clamp((298C-100D-208E+128)>>8)`, `B=clamp((298C+516D+128)>>8)`. Invalid camera/network states fail fast, log, and unwind; stale frames are preferable to memory growth.

Design choices: V4L2 is native and low overhead; bounded queues provide predictable latency; WebSocket supports browser binary messages; raw YUYV avoids a codec dependency and keeps the protocol transparent, at the cost of bandwidth.
