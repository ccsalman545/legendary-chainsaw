# C API reference

Headers live in `include/`; opaque types hide implementation state. Return conventions are documented below.

## Frame (`frame.h`)
`Frame { uint32_t width,height,pixel_format,stride; size_t size; uint64_t sequence,timestamp_us; uint32_t buffer_index; uint8_t *data; }`. `double frame_average_luminance(const Frame*)` computes average Y (NULL/invalid input is not useful). It reads, does not own, and is safe for concurrent readers when the frame is immutable.

## Camera (`camera_v4l2.h`)
| Function | Signature / result | Ownership and safety |
|---|---|---|
| open | `Camera *camera_open(const char*,uint32_t,uint32_t,uint32_t)`; NULL on failure | allocates camera; caller closes; setup before threads |
| start | `int camera_start(Camera*)`; 0 success, -1 error | starts streaming; not concurrent with close |
| capture | `int camera_capture(Camera*,Frame*)`; 0 success, -1/error | fills borrowed MMAP pointer; caller must release |
| release_frame | `void camera_release_frame(Camera*,const Frame*)` | returns driver buffer; before reuse |
| close | `void camera_close(Camera*)` | stops/unmaps/frees; joins caller first |

## Worker (`camera_worker.h`)
`camera_worker_create(Camera*,FrameQueue*)` returns owned worker or NULL; `camera_worker_start()` returns 0/-1 and creates its pthread; `camera_worker_stop()` requests termination; `camera_worker_join()` waits; `camera_worker_destroy()` frees after join. Camera and queue are borrowed. Stop/join are intended from the controlling thread.

## Queue (`frame_queue.h`)
`frame_queue_create(size_t)` returns owned queue; `frame_queue_push(q,const Frame*)` copies pixels and returns 0/-1; `frame_queue_pop(q,Frame*)` returns 1 frame, 0 empty, -1 invalid and transfers `data` ownership to caller (free it); `frame_queue_size()` reports count; `clear()` frees queued data; `destroy()` frees queue. Push/pop/size/clear are mutex-protected; caller must not destroy during use.

## Stream (`frame_stream.h`)
`frame_stream_create()` allocates; `frame_stream_set_client(s,mg_connection*)` stores a non-owning Mongoose pointer; `frame_stream_clear_client()` detaches only if matching; `frame_stream_send(s,const Frame*)` returns 0/-1 and creates/frees a packet temporary; `destroy()` frees context, not connection. Calls belong to the Mongoose event-loop thread.

## HTTP server (`http_server.h`)
`http_server_start(const char*,uint16_t)` returns owned server or NULL; `http_server_run()` blocks in the event loop; `http_server_poll(server,int)` polls once; `http_server_send_frame()` returns 0/-1; `http_server_stop()` requests loop stop; `http_server_destroy()` releases Mongoose, worker, queue, camera, and server state. These lifecycle calls are controller-thread operations.

## Lifecycle
```text
start -> server_start -> camera/queue/worker setup -> worker_start -> run/poll
stop signal -> worker_stop -> worker_join -> destroy -> camera_close
```
