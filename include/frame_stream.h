#ifndef FRAME_STREAM_H
#define FRAME_STREAM_H

#include "frame.h"

struct mg_connection;

typedef struct FrameStream FrameStream;

/*
 * Create a frame streaming context.
 */
FrameStream *frame_stream_create(void);

/*
 * Attach a WebSocket receiver.
 *
 * Several receivers can watch the camera at once; every live
 * connection is kept and each frame is broadcast to all of them.
 * The connection is owned by Mongoose; FrameStream stores only a
 * non-owning pointer.
 */
void frame_stream_set_client(
    FrameStream *stream,
    struct mg_connection *connection
);

/*
 * Detach the WebSocket client.
 */
void frame_stream_clear_client(
    FrameStream *stream,
    struct mg_connection *connection
);

/*
 * Send one Frame as one WebSocket binary message to every attached
 * receiver.
 *
 * Packet format:
 *
 * +----------------------+------------------+
 * | FramePacketHeader    | Camera frame     |
 * | 28 bytes             | frame->size      |
 * +----------------------+------------------+
 *
 * Returns:
 *   0  delivered to at least one receiver
 *  -1 failure / no receiver
 */
int frame_stream_send(
    FrameStream *stream,
    const Frame *frame
);

/*
 * Destroy the frame-stream context.
 */
void frame_stream_destroy(
    FrameStream *stream
);

#endif
