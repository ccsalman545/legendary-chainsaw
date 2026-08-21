#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/*
 * Default network settings.
 *
 * These can be overridden by:
 *   1. Command-line arguments (highest priority)
 *   2. Environment variables
 *   3. These defaults (lowest priority)
 */
#define DEFAULT_BIND_ADDRESS "0.0.0.0"
#define DEFAULT_PORT         8080

static HttpServer *g_server = NULL;

static void signal_handler(int sig)
{
    (void)sig;

    printf("\nReceived signal, shutting down...\n");

    if (g_server != NULL) {
        http_server_stop(g_server);
    }
}

static void print_usage(const char *program)
{
    printf("Usage: %s [OPTIONS]\n\n", program);
    printf("Options:\n");
    printf("  -a, --address ADDR   Bind address "
           "(default: %s)\n", DEFAULT_BIND_ADDRESS);
    printf("  -p, --port PORT      Listen port "
           "(default: %d)\n", DEFAULT_PORT);
    printf("  -h, --help           Show this help\n");
    printf("\nEnvironment variables:\n");
    printf("  BIND_ADDRESS         Bind address "
           "(overridden by -a)\n");
    printf("  LISTEN_PORT          Listen port "
           "(overridden by -p)\n");
    printf("  CAMERA_DEVICE        Camera device path "
           "(default: /dev/video0)\n");
}

int main(int argc, char *argv[])
{
    const char *bind_address = NULL;
    uint16_t port = 0;

    /*
     * Parse command-line arguments.
     */
    for (int i = 1; i < argc; i++) {

        if ((strcmp(argv[i], "-a") == 0 ||
             strcmp(argv[i], "--address") == 0) &&
            i + 1 < argc) {

            bind_address = argv[++i];

        } else if ((strcmp(argv[i], "-p") == 0 ||
                    strcmp(argv[i], "--port") == 0) &&
                   i + 1 < argc) {

            long val = strtol(argv[++i], NULL, 10);

            if (val <= 0 || val > 65535) {
                fprintf(stderr,
                        "Invalid port: %s\n",
                        argv[i]);
                return EXIT_FAILURE;
            }

            port = (uint16_t)val;

        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {

            print_usage(argv[0]);
            return EXIT_SUCCESS;

        } else {
            fprintf(stderr,
                    "Unknown option: %s\n",
                    argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /*
     * Fall back to environment variables,
     * then to defaults.
     */
    if (bind_address == NULL) {
        bind_address = getenv("BIND_ADDRESS");
    }

    if (bind_address == NULL) {
        bind_address = DEFAULT_BIND_ADDRESS;
    }

    if (port == 0) {
        const char *port_env = getenv("LISTEN_PORT");

        if (port_env != NULL) {
            long val = strtol(port_env, NULL, 10);

            if (val > 0 && val <= 65535) {
                port = (uint16_t)val;
            }
        }
    }

    if (port == 0) {
        port = DEFAULT_PORT;
    }

    printf("Bind address: %s\n", bind_address);
    printf("Listen port:  %u\n", (unsigned)port);

    const char *cam_dev = getenv("CAMERA_DEVICE");

    printf("Camera device: %s\n",
           cam_dev ? cam_dev : "/dev/video0");

    HttpServer *server =
        http_server_start(
            bind_address,
            port
        );

    if (!server) {
        fprintf(
            stderr,
            "Failed to start HTTP server\n"
        );

        return EXIT_FAILURE;
    }

    g_server = server;

    /*
     * Handle Ctrl+C gracefully.
     */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Press Ctrl+C to stop the server.\n");

    http_server_run(server);

    http_server_destroy(server);

    return EXIT_SUCCESS;
}
