#include "http_server.h"
#include "mongoose.h"
#include "camera_v4l2.h"
#include "camera_worker.h"
#include "frame_queue.h"
#include "frame_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define HTTP_CORS_HEADERS \
    "Access-Control-Allow-Origin: *\r\n" \
    "Access-Control-Allow-Methods: GET, OPTIONS\r\n" \
    "Access-Control-Allow-Headers: *\r\n"

/*
 * Default camera device.
 * Overridden at runtime by the CAMERA_DEVICE environment variable.
 */
#define CAMERA_DEVICE_DEFAULT "/dev/video0"
#define CAMERA_WIDTH  640
#define CAMERA_HEIGHT 480
#define CAMERA_FPS    30

#define FRAME_QUEUE_CAPACITY 3

struct HttpServer {
    struct mg_mgr mgr;
    struct mg_connection *listener;

    Camera *camera;
    CameraWorker *worker;

    FrameQueue *queue;
    FrameStream *frame_stream;

    char camera_device[256];
    char listen_address[64];
    uint16_t port;

    bool running;
};


/*
 * Web interface.
 *
 * The browser receives YUYV frames through WebSocket.
 * JavaScript converts YUYV to RGB and displays it on canvas.
 */
static const char HTML_PAGE[] =
    "<!doctype html>"
    "<html>"
    "<head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Camera Server</title>"
    "<style>"
    "body{"
    "margin:0;"
    "padding:20px;"
    "background:#111;"
    "color:#eee;"
    "font-family:Arial,sans-serif;"
    "}"
    ".box{max-width:850px;margin:auto;}"
    "h1{margin-top:0;}"
    ".status{"
    "background:#222;"
    "padding:15px;"
    "border-radius:8px;"
    "margin-bottom:15px;"
    "}"
    "#video{"
    "display:block;"
    "width:100%;"
    "max-width:640px;"
    "height:auto;"
    "background:#000;"
    "border-radius:8px;"
    "margin-bottom:15px;"
    "}"
    "button{"
    "padding:12px 20px;"
    "font-size:16px;"
    "border:0;"
    "border-radius:6px;"
    "cursor:pointer;"
    "}"
    "#connect{background:#1683ff;color:white;}"
    "#connect.disconnect{background:#d33;}"
    ".hint{color:#aaa;font-size:13px;margin-top:8px;}"
    "</style>"
    "</head>"

    "<body>"
    "<div class=\"box\">"

    "<h1>Camera Server</h1>"

    "<div class=\"status\">"
    "<div>HTTP: <b>Online</b></div>"
    "<div id=\"wsstatus\">WebSocket: Disconnected</div>"
    "<div id=\"wsurl\"></div>"
    "<div id=\"frames\">Frames: 0</div>"
    "<div id=\"fps\">FPS: 0</div>"
    "<div class=\"hint\">"
    "From another machine, open this page using the camera PC's "
    "LAN IP (not 0.0.0.0 or 127.0.0.1)."
    "</div>"
    "</div>"

    "<canvas id=\"video\" width=\"640\" height=\"480\"></canvas>"

    "<button id=\"connect\">Connect WebSocket</button>"

    "</div>"

    "<script>"

    "const canvas=document.getElementById('video');"
    "const ctx=canvas.getContext('2d');"
    "const button=document.getElementById('connect');"
    "const status=document.getElementById('wsstatus');"
    "const framesLabel=document.getElementById('frames');"
    "const fpsLabel=document.getElementById('fps');"

    "let ws=null;"
    "let wantOpen=false;"
    "let reconnectTimer=null;"
    "let frameCount=0;"
    "let fpsFrames=0;"
    "let fpsTime=performance.now();"

    "function websocketUrl(){"
        "const protocol="
            "(location.protocol==='https:'?'wss://':'ws://');"
        "return protocol+location.host+'/ws';"
    "}"

    /*
     * Convert YUYV 4:2:2 to RGB.
     */
    "function yuyvToRgb(data,width,height){"

        "const image=ctx.createImageData(width,height);"
        "const out=image.data;"
        "let di=0;"

        "for(let i=0;i+3<data.length;i+=4){"

            "const y0=data[i];"
            "const u=data[i+1];"
            "const y1=data[i+2];"
            "const v=data[i+3];"

            "let c=y0-16;"
            "let d=u-128;"
            "let e=v-128;"

            "let r=(298*c+409*e+128)>>8;"
            "let g=(298*c-100*d-208*e+128)>>8;"
            "let b=(298*c+516*d+128)>>8;"

            "r=Math.max(0,Math.min(255,r));"
            "g=Math.max(0,Math.min(255,g));"
            "b=Math.max(0,Math.min(255,b));"

            "out[di++]=r;"
            "out[di++]=g;"
            "out[di++]=b;"
            "out[di++]=255;"

            "c=y1-16;"

            "r=(298*c+409*e+128)>>8;"
            "g=(298*c-100*d-208*e+128)>>8;"
            "b=(298*c+516*d+128)>>8;"

            "r=Math.max(0,Math.min(255,r));"
            "g=Math.max(0,Math.min(255,g));"
            "b=Math.max(0,Math.min(255,b));"

            "out[di++]=r;"
            "out[di++]=g;"
            "out[di++]=b;"
            "out[di++]=255;"
        "}"

        "ctx.putImageData(image,0,0);"
    "}"

    /*
     * Process one binary camera packet.
     *
     * Packet:
     *
     * 0  - 3   magic
     * 4  - 7   width
     * 8  - 11  height
     * 12 - 15  pixel format
     * 16 - 19  stride
     * 20 - 23  frame size
     * 24 - 27  sequence
     * 28 ...   YUYV
     */
    "function processFrame(buffer){"

        "if(buffer.byteLength<28){"
            "console.log('Packet too small');"
            "return;"
        "}"

        "const view=new DataView(buffer);"

        "const magic=view.getUint32(0,true);"

        "if(magic!==0x4652414d){"
            "console.log('Invalid frame magic:',magic.toString(16));"
            "return;"
        "}"

        "const width=view.getUint32(4,true);"
        "const height=view.getUint32(8,true);"
        "const frameSize=view.getUint32(20,true);"
        "const sequence=view.getUint32(24,true);"

        "if(28+frameSize>buffer.byteLength){"
            "console.log('Invalid frame size');"
            "return;"
        "}"

        "if(frameSize<width*height*2){"
            "console.log('Incomplete YUYV frame');"
            "return;"
        "}"

        "if(canvas.width!==width||canvas.height!==height){"
            "canvas.width=width;"
            "canvas.height=height;"
        "}"

        "const yuyv=new Uint8Array(buffer,28,frameSize);"

        "yuyvToRgb(yuyv,width,height);"

        "frameCount++;"
        "fpsFrames++;"

        "framesLabel.textContent="
            "'Frames: '+frameCount+' | Sequence: '+sequence;"

        "const now=performance.now();"

        "if(now-fpsTime>=1000){"

            "const fps=fpsFrames*1000/(now-fpsTime);"

            "fpsLabel.textContent='FPS: '+fps.toFixed(1);"

            "fpsFrames=0;"
            "fpsTime=now;"
        "}"
    "}"

    /*
     * WebSocket connection.
     */
    "function scheduleReconnect(){"
        "if(!wantOpen){return;}"
        "if(reconnectTimer){return;}"
        "reconnectTimer=setTimeout(function(){"
            "reconnectTimer=null;"
            "connectWebSocket();"
        "},1000);"
    "}"

    "function connectWebSocket(){"

        "if(ws&&ws.readyState===WebSocket.OPEN){"
            "wantOpen=false;"
            "if(reconnectTimer){"
                "clearTimeout(reconnectTimer);"
                "reconnectTimer=null;"
            "}"
            "ws.close();"
            "return;"
        "}"

        "if(ws&&ws.readyState===WebSocket.CONNECTING){"
            "return;"
        "}"

        "wantOpen=true;"

        "const url=websocketUrl();"

        "console.log('Connecting to '+url);"

        "document.getElementById('wsurl').textContent='URL: '+url;"

        "status.textContent='WebSocket: Connecting to '+url;"

        "try{"
            "ws=new WebSocket(url);"
        "}catch(error){"
            "console.error(error);"
            "status.textContent='WebSocket: Error '+error;"
            "scheduleReconnect();"
            "return;"
        "}"

        "ws.binaryType='arraybuffer';"

        "ws.onopen=function(){"

            "console.log('WebSocket connected');"

            "status.textContent='WebSocket: Connected';"

            "button.textContent='Disconnect WebSocket';"

            "button.classList.add('disconnect');"
        "};"

        "ws.onmessage=function(event){"

            "if(event.data instanceof ArrayBuffer){"

                "processFrame(event.data);"

            "}"
        "};"

        "ws.onerror=function(error){"

            "console.error('WebSocket error',error);"

            "status.textContent="
                "'WebSocket: Error connecting to '+url+'. '"
                "+'Use the camera LAN IP, bind 0.0.0.0, open firewall.';"
        "};"

        "ws.onclose=function(){"

            "console.log('WebSocket closed');"

            "status.textContent='WebSocket: Disconnected';"

            "button.textContent='Connect WebSocket';"

            "button.classList.remove('disconnect');"

            "ws=null;"

            "scheduleReconnect();"
        "};"
    "}"

    "button.onclick=connectWebSocket;"

    "window.addEventListener('load',function(){"
        "document.getElementById('wsurl').textContent="
            "'URL: '+websocketUrl();"
        "connectWebSocket();"
    "});"


    "</script>"
    "</body>"
    "</html>";



static void print_peer(const char *prefix, struct mg_connection *c)
{
    char addr[80];

    mg_snprintf(
        addr,
        sizeof(addr),
        "%M",
        mg_print_ip_port,
        &c->rem
    );

    printf("%s%s\n", prefix, addr);
}


static void configure_client_socket(struct mg_connection *c)
{
    int fd = (int) (size_t) c->fd;
    int on = 1;
    int sndbuf = 2 * 1024 * 1024;

    if (fd < 0) {
        return;
    }

    (void) setsockopt(
        fd,
        SOL_SOCKET,
        SO_KEEPALIVE,
        &on,
        sizeof(on)
    );

    (void) setsockopt(
        fd,
        IPPROTO_TCP,
        TCP_NODELAY,
        &on,
        sizeof(on)
    );

    (void) setsockopt(
        fd,
        SOL_SOCKET,
        SO_SNDBUF,
        &sndbuf,
        sizeof(sndbuf)
    );
}


static void print_receiver_urls(uint16_t port)
{
    struct ifaddrs *ifaddr = NULL;

    printf(
        "\nReceiver: do not use 0.0.0.0 as the URL.\n"
        "Open one of these on the viewing PC/phone:\n"
        "  http://127.0.0.1:%u/   (same machine only)\n",
        (unsigned) port
    );

    if (getifaddrs(&ifaddr) != 0) {
        printf(
            "  (could not list LAN addresses; run: ip -br address)\n\n"
        );
        return;
    }

    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in *sa;

        if (ifa->ifa_addr == NULL) {
            continue;
        }

        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if ((ifa->ifa_flags & IFF_UP) == 0) {
            continue;
        }

        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }

        sa = (struct sockaddr_in *) ifa->ifa_addr;

        if (inet_ntop(
                AF_INET,
                &sa->sin_addr,
                ip,
                sizeof(ip)
            ) == NULL) {
            continue;
        }

        printf(
            "  http://%s:%u/\n"
            "  ws://%s:%u/ws\n",
            ip,
            (unsigned) port,
            ip,
            (unsigned) port
        );
    }

    freeifaddrs(ifaddr);
    printf("\n");
}


static void format_listen_url(
    char *url,
    size_t url_size,
    const char *listen_address,
    uint16_t port
)
{
    if (strchr(listen_address, ':') != NULL) {
        snprintf(
            url,
            url_size,
            "http://[%s]:%u",
            listen_address,
            (unsigned) port
        );
    } else {
        snprintf(
            url,
            url_size,
            "http://%s:%u",
            listen_address,
            (unsigned) port
        );
    }
}


/*
 * Send available camera frames to WebSocket client.
 */
static void http_send_frames(HttpServer *server)
{
    if (server == NULL) {
        return;
    }

    if (server->frame_stream == NULL) {
        return;
    }

    if (server->queue == NULL) {
        return;
    }

    /*
     * Drain all currently available frames.
     */
    for (;;) {

        Frame frame;

        memset(&frame, 0, sizeof(frame));

        int result =
            frame_queue_pop(
                server->queue,
                &frame
            );

        if (result <= 0) {
            break;
        }

        /*
         * Send:
         *
         * [28-byte packet header]
         * [YUYV frame]
         */
        frame_stream_send(
            server->frame_stream,
            &frame
        );

        /*
         * frame_queue_pop() gives ownership
         * of frame.data to us.
         */
        free(frame.data);

        frame.data = NULL;
    }
}


/*
 * HTTP/WebSocket event handler.
 */
static void http_event_handler(
    struct mg_connection *c,
    int ev,
    void *ev_data
)
{
    HttpServer *server =
        (HttpServer *) c->fn_data;

    if (server == NULL) {
        return;
    }

    switch (ev) {

    case MG_EV_ACCEPT:
        print_peer("Client accepted: ", c);
        configure_client_socket(c);
        break;

    case MG_EV_ERROR:
        fprintf(
            stderr,
            "Connection error: %s\n",
            ev_data ? (const char *) ev_data : "?"
        );
        break;

    case MG_EV_HTTP_MSG: {

        struct mg_http_message *hm =
            (struct mg_http_message *) ev_data;

        if (mg_strcasecmp(hm->method, mg_str("OPTIONS")) == 0) {
            mg_http_reply(
                c,
                204,
                HTTP_CORS_HEADERS,
                ""
            );
            return;
        }

        /*
         * WebSocket endpoint.
         */
        if (mg_match(hm->uri, mg_str("/ws#"), NULL)) {

            print_peer("WebSocket upgrade requested from ", c);

            mg_ws_upgrade(
                c,
                hm,
                HTTP_CORS_HEADERS
            );

            return;
        }

        /*
         * Main web page.
         */
        if (mg_match(
                hm->uri,
                mg_str("/"),
                NULL
            )) {

            mg_http_reply(
                c,
                200,
                "Content-Type: text/html; charset=utf-8\r\n"
                HTTP_CORS_HEADERS,
                "%s",
                HTML_PAGE
            );

            return;
        }

        /*
         * Simple status endpoint.
         */
        if (mg_match(
                hm->uri,
                mg_str("/status"),
                NULL
            )) {

            mg_http_reply(
                c,
                200,
                "Content-Type: application/json\r\n"
                HTTP_CORS_HEADERS,
                "{"
                "\"status\":\"online\","
                "\"camera\":\"%s\","
                "\"format\":\"YUYV\","
                "\"width\":%u,"
                "\"height\":%u,"
                "\"fps\":%u,"
                "\"bind\":\"%s:%u\","
                "\"ws\":\"/ws\""
                "}",
                server->camera_device,
                (unsigned)CAMERA_WIDTH,
                (unsigned)CAMERA_HEIGHT,
                (unsigned)CAMERA_FPS,
                server->listen_address,
                (unsigned) server->port
            );

            return;
        }

        /*
         * Unknown path.
         */
        mg_http_reply(
            c,
            404,
            "Content-Type: text/plain\r\n"
            HTTP_CORS_HEADERS,
            "404 Not Found\n"
        );

        break;
    }


    /*
     * WebSocket connection opened.
     */
    case MG_EV_WS_OPEN:

        print_peer("WebSocket client connected: ", c);

        frame_stream_set_client(
            server->frame_stream,
            c
        );

        /*
         * Send a small text confirmation.
         */
        {
            const char *message =
                "Camera WebSocket connected";

            mg_ws_send(
                c,
                message,
                strlen(message),
                WEBSOCKET_OP_TEXT
            );
        }

        break;


    /*
     * WebSocket message received.
     */
    case MG_EV_WS_MSG: {

        struct mg_ws_message *wm =
            (struct mg_ws_message *) ev_data;

        printf(
            "WebSocket message received: %zu bytes\n",
            wm->data.len
        );

        /*
         * Echo text messages.
         *
         * Camera frames are sent independently
         * as binary WebSocket messages.
         */
        mg_ws_send(
            c,
            wm->data.buf,
            wm->data.len,
            WEBSOCKET_OP_TEXT
        );

        break;
    }


    /*
     * Connection closed.
     */
    case MG_EV_CLOSE:

        printf(
            "Client connection closed\n"
        );

        frame_stream_clear_client(
            server->frame_stream,
            c
        );

        break;


    default:
        break;
    }
}


/*
 * Start HTTP/WebSocket server and camera pipeline.
 */
HttpServer *http_server_start(
    const char *listen_address,
    uint16_t port
)
{
    if (listen_address == NULL || port == 0) {
        fprintf(
            stderr,
            "Invalid HTTP server configuration\n"
        );

        return NULL;
    }

    HttpServer *server =
        calloc(1, sizeof(*server));

    if (server == NULL) {
        perror("calloc");
        return NULL;
    }

    /*
     * Initialize Mongoose.
     */
    mg_mgr_init(
        &server->mgr
    );


    /*
     * Create frame queue.
     */
    server->queue =
        frame_queue_create(
            FRAME_QUEUE_CAPACITY
        );

    if (server->queue == NULL) {

        fprintf(
            stderr,
            "Failed to create frame queue\n"
        );

        mg_mgr_free(
            &server->mgr
        );

        free(server);

        return NULL;
    }


    /*
     * Create WebSocket frame stream.
     */
    server->frame_stream =
        frame_stream_create();

    if (server->frame_stream == NULL) {

        frame_queue_destroy(
            server->queue
        );

        mg_mgr_free(
            &server->mgr
        );

        free(server);

        return NULL;
    }


    /*
     * Determine camera device.
     * Use CAMERA_DEVICE environment variable if set,
     * otherwise use the default.
     */
    const char *camera_device =
        getenv("CAMERA_DEVICE");

    if (camera_device == NULL) {
        camera_device = CAMERA_DEVICE_DEFAULT;
    }

    /*
     * Store in server struct for status endpoint.
     */
    snprintf(
        server->camera_device,
        sizeof(server->camera_device),
        "%s",
        camera_device
    );

    /*
     * Open camera.
     */
    server->camera =
        camera_open(
            camera_device,
            CAMERA_WIDTH,
            CAMERA_HEIGHT,
            CAMERA_FPS
        );

    if (server->camera == NULL) {

        fprintf(
            stderr,
            "Failed to open camera %s\n",
            camera_device
        );

        frame_stream_destroy(
            server->frame_stream
        );

        frame_queue_destroy(
            server->queue
        );

        mg_mgr_free(
            &server->mgr
        );

        free(server);

        return NULL;
    }


    /*
     * Start V4L2 streaming.
     */
    if (camera_start(
            server->camera
        ) != 0) {

        fprintf(
            stderr,
            "Failed to start camera streaming\n"
        );

        camera_close(
            server->camera
        );

        frame_stream_destroy(
            server->frame_stream
        );

        frame_queue_destroy(
            server->queue
        );

        mg_mgr_free(
            &server->mgr
        );

        free(server);

        return NULL;
    }


    /*
     * Create camera worker.
     */
    server->worker =
        camera_worker_create(
            server->camera,
            server->queue
        );

    if (server->worker == NULL) {

        fprintf(
            stderr,
            "Failed to create camera worker\n"
        );

        camera_close(
            server->camera
        );

        frame_stream_destroy(
            server->frame_stream
        );

        frame_queue_destroy(
            server->queue
        );

        mg_mgr_free(
            &server->mgr
        );

        free(server);

        return NULL;
    }


    /*
     * Start camera worker thread.
     */
    if (camera_worker_start(
            server->worker
        ) != 0) {

        fprintf(
            stderr,
            "Failed to start camera worker\n"
        );

        camera_worker_destroy(
            server->worker
        );

        camera_close(
            server->camera
        );

        frame_stream_destroy(
            server->frame_stream
        );

        frame_queue_destroy(
            server->queue
        );

        mg_mgr_free(
            &server->mgr
        );

        free(server);

        return NULL;
    }


    snprintf(
        server->listen_address,
        sizeof(server->listen_address),
        "%s",
        listen_address
    );
    server->port = port;

    /*
     * Build Mongoose URL.
     */
    char url[128];

    format_listen_url(
        url,
        sizeof(url),
        listen_address,
        port
    );


    printf(
        "Starting HTTP server on %s\n",
        url
    );


    /*
     * Start HTTP listener.
     */
    server->listener =
        mg_http_listen(
            &server->mgr,
            url,
            http_event_handler,
            server
        );

    if (server->listener == NULL) {

        fprintf(
            stderr,
            "Failed to start HTTP server\n"
        );

        camera_worker_destroy(
            server->worker
        );

        camera_close(
            server->camera
        );

        frame_stream_destroy(
            server->frame_stream
        );

        frame_queue_destroy(
            server->queue
        );

        mg_mgr_free(
            &server->mgr
        );

        free(server);

        return NULL;
    }


    server->running = true;


    printf(
        "HTTP server started successfully\n"
    );

    printf(
        "Listen address: %s:%u  (0.0.0.0 means all interfaces)\n",
        listen_address,
        (unsigned int) port
    );

    print_receiver_urls(port);

    printf(
        "Camera: %s\n",
        camera_device
    );

    printf(
        "Camera format: %ux%u YUYV @ %u FPS\n",
        CAMERA_WIDTH,
        CAMERA_HEIGHT,
        CAMERA_FPS
    );

    return server;
}


/*
 * Run HTTP event loop.
 */
void http_server_run(
    HttpServer *server
)
{
    if (server == NULL) {
        return;
    }

    printf(
        "HTTP server event loop started\n"
    );

    while (server->running) {

        /*
         * Process network events.
         */
        mg_mgr_poll(
            &server->mgr,
            10
        );

        /*
         * Send queued camera frames.
         */
        http_send_frames(
            server
        );
    }
}


void http_server_poll(
    HttpServer *server,
    int timeout_ms
)
{
    if (server == NULL) {
        return;
    }

    mg_mgr_poll(
        &server->mgr,
        timeout_ms
    );

    http_send_frames(server);
}


int http_server_send_frame(
    HttpServer *server,
    const Frame *frame
)
{
    if (server == NULL || frame == NULL) {
        return -1;
    }

    return frame_stream_send(
        server->frame_stream,
        frame
    );
}


/*
 * Stop server.
 */
void http_server_stop(
    HttpServer *server
)
{
    if (server == NULL) {
        return;
    }

    server->running = false;
}


/*
 * Destroy server.
 */
void http_server_destroy(
    HttpServer *server
)
{
    if (server == NULL) {
        return;
    }

    server->running = false;


    /*
     * Stop and destroy camera worker.
     */
    if (server->worker != NULL) {

        camera_worker_destroy(
            server->worker
        );

        server->worker = NULL;
    }


    /*
     * Close camera.
     */
    if (server->camera != NULL) {

        camera_close(
            server->camera
        );

        server->camera = NULL;
    }


    /*
     * Destroy frame queue.
     */
    if (server->queue != NULL) {

        frame_queue_destroy(
            server->queue
        );

        server->queue = NULL;
    }


    /*
     * Destroy frame stream.
     */
    if (server->frame_stream != NULL) {

        frame_stream_destroy(
            server->frame_stream
        );

        server->frame_stream = NULL;
    }


    /*
     * Free Mongoose.
     */
    mg_mgr_free(
        &server->mgr
    );

    free(server);
}
