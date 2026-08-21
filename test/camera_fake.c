/*
 * Test-only synthetic camera.
 *
 * Implements the same camera_v4l2.h API as src/camera_v4l2.c but with
 * no V4L2 hardware dependency, so the multi-receiver test can run on any
 * machine (CI, sandbox, a dev laptop without a webcam). It produces a
 * steady stream of synthetic YUYV frames so the capture worker, frame
 * queue, and WebSocket broadcast path are all exercised exactly as they
 * are with a real camera.
 *
 * This file is NOT compiled into the normal build/http_server target.
 */
#define _POSIX_C_SOURCE 200809L

#include "camera_v4l2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* V4L2 fourcc for YUYV (mirrors the value the real camera uses). */
#define FAKECAM_YUYV 0x56595559u

#define FAKECAM_BUFFER_COUNT 4

struct CameraBuffer {
    uint8_t *start;
    size_t length;
};

struct Camera {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t stride;
    size_t frame_size;

    struct CameraBuffer buffers[FAKECAM_BUFFER_COUNT];

    int streaming;
    unsigned int next_index;
    uint64_t sequence;
};

static uint64_t fakecam_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000000ull +
           (uint64_t) ts.tv_nsec / 1000ull;
}

Camera *camera_open(
    const char *device,
    uint32_t width,
    uint32_t height,
    uint32_t fps)
{
    (void) device;

    if (width == 0 || height == 0 || fps == 0) {
        return NULL;
    }

    Camera *camera = calloc(1, sizeof(*camera));

    if (camera == NULL) {
        return NULL;
    }

    camera->width = width;
    camera->height = height;
    camera->fps = fps;
    camera->stride = width * 2;
    camera->frame_size = (size_t) width * height * 2;

    for (unsigned int i = 0; i < FAKECAM_BUFFER_COUNT; i++) {
        camera->buffers[i].length = camera->frame_size;
        camera->buffers[i].start = calloc(1, camera->frame_size);

        if (camera->buffers[i].start == NULL) {
            camera_close(camera);
            return NULL;
        }
    }

    printf(
        "[fakecam] opened %ux%u @ %u fps, frame size %zu bytes\n",
        width,
        height,
        fps,
        camera->frame_size
    );

    return camera;
}

int camera_start(Camera *camera)
{
    if (camera == NULL) {
        return -1;
    }

    camera->streaming = 1;

    return 0;
}

int camera_capture(Camera *camera, Frame *frame)
{
    if (camera == NULL || frame == NULL || !camera->streaming) {
        return -1;
    }

    /*
     * Roughly honour the requested frame rate so the test exercises the
     * server at a realistic ~30 fps instead of flooding it.
     */
    if (camera->fps > 0) {
        usleep(1000000u / camera->fps);
    }

    unsigned int index =
        (camera->next_index++) % FAKECAM_BUFFER_COUNT;

    uint8_t *pixels = camera->buffers[index].start;

    /*
     * Paint a YUYV pattern that changes with the sequence so a test can
     * tell successive frames apart if it wants to.
     */
    uint8_t y_base = (uint8_t) (camera->sequence & 255);

    for (size_t i = 0; i + 3 < camera->frame_size; i += 4) {
        pixels[i] = (uint8_t) (y_base + (i / 4));
        pixels[i + 1] = 128;
        pixels[i + 2] = (uint8_t) (255 - (y_base + (i / 4)));
        pixels[i + 3] = 128;
    }

    memset(frame, 0, sizeof(*frame));

    frame->width = camera->width;
    frame->height = camera->height;
    frame->pixel_format = FAKECAM_YUYV;
    frame->stride = camera->stride;
    frame->size = camera->frame_size;
    frame->sequence = ++camera->sequence;
    frame->timestamp_us = fakecam_now_us();
    frame->buffer_index = index;
    frame->data = camera->buffers[index].start;

    return 1;
}

void camera_release_frame(Camera *camera, const Frame *frame)
{
    (void) camera;
    (void) frame;
}

void camera_close(Camera *camera)
{
    if (camera == NULL) {
        return;
    }

    for (unsigned int i = 0; i < FAKECAM_BUFFER_COUNT; i++) {
        free(camera->buffers[i].start);
    }

    free(camera);
}
