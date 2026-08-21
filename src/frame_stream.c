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
 * YUYV; overflowing the send buffer closes the WebSocket. The limit
 * is applied per receiver so one slow viewer never blocks the others.
 */
#define FRAME_STREAM_MAX_BUFFERED (2u * 1024u * 1024u)

/*
 * Safety cap so a misbehaving client cannot make the receiver list
 * grow without bound. In normal use every accepted WebSocket is also
 * removed again on MG_EV_CLOSE.
 */
#define FRAME_STREAM_MAX_CLIENTS 64

/*
 * One connected WebSocket receiver.
 *
 * The list of receivers is what lets several viewing PCs/phones watch
 * the same camera at the same time. Previously FrameStream stored a
 * single client and closed ("drained") the previous one whenever a new
 * viewer connected; combined with the browser's auto-reconnect that
 * made two receivers fight forever, each kicking the other off, so in
 * practice no receiver could stay connected. We now keep every live
 * receiver and broadcast each frame to all of them.
 */
typedef struct FrameStreamClient {
    struct mg_connection *connection;
    struct FrameStreamClient *next;
} FrameStreamClient;

struct FrameStream {
    FrameStreamClient *head;
    size_t count;
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

    stream->head = NULL;
    stream->count = 0;

    return stream;
}


static FrameStreamClient *frame_stream_find(
    FrameStream *stream,
    struct mg_connection *connection)
{
    for (FrameStreamClient *c = stream->head; c != NULL; c = c->next) {
        if (c->connection == connection) {
            return c;
        }
    }

    return NULL;
}


void frame_stream_set_client(
    FrameStream *stream,
    struct mg_connection *connection)
{
    if (stream == NULL || connection == NULL) {
        return;
    }

    /*
     * Mongoose can deliver MG_EV_WS_OPEN more than once for the same
     * connection in some reconnect races; never add a duplicate.
     */
    if (frame_stream_find(stream, connection) != NULL) {
        return;
    }

    if (stream->count >= FRAME_STREAM_MAX_CLIENTS) {
        fprintf(
            stderr,
            "Frame stream client limit reached (%d); rejecting viewer\n",
            FRAME_STREAM_MAX_CLIENTS
        );
        connection->is_draining = 1;
        return;
    }

    FrameStreamClient *client = calloc(1, sizeof(*client));

    if (client == NULL) {
        fprintf(stderr, "Failed to allocate frame stream client\n");
        return;
    }

    client->connection = connection;
    client->next = stream->head;

    stream->head = client;
    stream->count++;

    printf(
        "Frame stream client attached (%zu viewer%s)\n",
        stream->count,
        stream->count == 1 ? "" : "s"
    );
}


void frame_stream_clear_client(
    FrameStream *stream,
    struct mg_connection *connection)
{
    if (stream == NULL) {
        return;
    }

    FrameStreamClient **link = &stream->head;

    while (*link != NULL) {
        if ((*link)->connection == connection) {
            FrameStreamClient *removed = *link;

            *link = removed->next;

            free(removed);

            stream->count--;

            printf(
                "Frame stream client detached (%zu viewer%s)\n",
                stream->count,
                stream->count == 1 ? "" : "s"
            );

            return;
        }

        link = &(*link)->next;
    }
}


/*
 * Drop and free any receiver whose connection is no longer a usable
 * WebSocket. MG_EV_CLOSE removes receivers already, but this also
 * catches connections that went away without a clean close event, so
 * the list cannot leak dead entries.
 */
static void frame_stream_prune(FrameStream *stream)
{
    FrameStreamClient **link = &stream->head;

    while (*link != NULL) {
        struct mg_connection *c = (*link)->connection;

        if (c == NULL ||
            c->is_closing ||
            c->is_draining ||
            !c->is_websocket) {

            FrameStreamClient *removed = *link;

            *link = removed->next;

            free(removed);

            stream->count--;

            continue;
        }

        link = &(*link)->next;
    }
}


int frame_stream_send(
    FrameStream *stream,
    const Frame *frame)
{
    if (stream == NULL || frame == NULL) {
        return -1;
    }

    if (frame->data == NULL || frame->size == 0) {
        return -1;
    }

    frame_stream_prune(stream);

    if (stream->head == NULL) {
        return -1;
    }

    size_t packet_size = sizeof(FramePacketHeader) + frame->size;

    /*
     * Build the packet once and reuse it for every receiver.
     *
     * One WebSocket binary message:
     *
     * [FramePacketHeader][Frame data]
     */
    unsigned char *packet = malloc(packet_size);

    if (packet == NULL) {
        fprintf(stderr, "Failed to allocate frame packet\n");
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

    memcpy(packet, &header, sizeof(header));

    memcpy(
        packet + sizeof(header),
        frame->data,
        frame->size
    );

    /*
     * Broadcast to every live receiver. Each socket has its own send
     * buffer; a slow viewer simply drops frames instead of blocking a
     * fast one or being forcibly disconnected.
     */
    int delivered = 0;

    for (FrameStreamClient *c = stream->head; c != NULL; c = c->next) {
        struct mg_connection *client = c->connection;

        if (client->send.len + packet_size + 16 >
                FRAME_STREAM_MAX_BUFFERED) {
            /*
             * This receiver is behind; skip this frame for it.
             */
            continue;
        }

        size_t sent = mg_ws_send(
            client,
            packet,
            packet_size,
            WEBSOCKET_OP_BINARY
        );

        if (sent != 0) {
            delivered++;
        }
    }

    free(packet);

    return delivered > 0 ? 0 : -1;
}


void frame_stream_destroy(
    FrameStream *stream)
{
    if (stream == NULL) {
        return;
    }

    FrameStreamClient *c = stream->head;

    while (c != NULL) {
        FrameStreamClient *next = c->next;

        free(c);

        c = next;
    }

    stream->head = NULL;
    stream->count = 0;

    free(stream);
}
