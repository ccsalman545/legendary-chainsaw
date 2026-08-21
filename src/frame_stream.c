#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "mongoose.h"
#include "frame_stream.h"

#define FRAME_PACKET_MAGIC 0x4652414D

/*
 * Drop frames rather than grow the Mongoose send buffer without
 * bound. A remote receiver on Wi-Fi cannot drain ~18 MB/s of raw
 * YUYV; overflowing the send buffer closes the WebSocket.
 */
#define FRAME_STREAM_MAX_BUFFERED (2u * 1024u * 1024u)

struct FrameStream {
    struct mg_connection *client;
};

typedef struct {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t stride;
    uint32_t frame_size;
    uint32_t sequence;
} FramePacketHeader;


FrameStream *frame_stream_create(void)
{
    FrameStream *stream = calloc(1, sizeof(*stream));

    if (stream == NULL) {
        fprintf(stderr, "Failed to allocate FrameStream\n");
        return NULL;
    }

    return stream;
}


void frame_stream_set_client(
    FrameStream *stream,
    struct mg_connection *connection)
{
    if (stream == NULL) {
        return;
    }

    if (stream->client != NULL && stream->client != connection) {
        printf("Replacing previous WebSocket client\n");
        stream->client->is_draining = 1;
    }

    stream->client = connection;

    printf("Frame stream client attached\n");
}


void frame_stream_clear_client(
    FrameStream *stream,
    struct mg_connection *connection)
{
    if (stream == NULL) {
        return;
    }

    if (stream->client == connection) {
        stream->client = NULL;

        printf("Frame stream client detached\n");
    }
}


int frame_stream_send(
    FrameStream *stream,
    const Frame *frame)
{
    if (stream == NULL || frame == NULL) {
        return -1;
    }

    if (stream->client == NULL) {
        return -1;
    }

    if (stream->client->is_closing ||
        stream->client->is_draining ||
        !stream->client->is_websocket) {
        return -1;
    }

    if (frame->data == NULL || frame->size == 0) {
        return -1;
    }

    size_t packet_size = sizeof(FramePacketHeader) + frame->size;

    if (stream->client->send.len + packet_size + 16 >
            FRAME_STREAM_MAX_BUFFERED) {
        return -1;
    }

    FramePacketHeader header;

    memset(&header, 0, sizeof(header));

    header.magic = FRAME_PACKET_MAGIC;
    header.width = frame->width;
    header.height = frame->height;
    header.pixel_format = frame->pixel_format;
    header.stride = frame->stride;
    header.frame_size = (uint32_t) frame->size;
    header.sequence = frame->sequence;

    /*
     * One WebSocket binary message:
     *
     * [FramePacketHeader][Frame data]
     */
    unsigned char *packet = malloc(packet_size);

    if (packet == NULL) {
        fprintf(stderr, "Failed to allocate frame packet\n");
        return -1;
    }

    memcpy(packet, &header, sizeof(header));

    memcpy(
        packet + sizeof(header),
        frame->data,
        frame->size
    );

    size_t sent = mg_ws_send(
        stream->client,
        packet,
        packet_size,
        WEBSOCKET_OP_BINARY
    );

    free(packet);

    if (sent == 0) {
        fprintf(stderr, "WebSocket frame send failed\n");
        return -1;
    }

    return 0;
}


void frame_stream_destroy(
    FrameStream *stream)
{
    if (stream == NULL) {
        return;
    }

    stream->client = NULL;

    free(stream);
}
