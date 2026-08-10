#pragma once

#include "esp_camera.h"

/**
 * GPIO mapping for common ESP32-S3-CAM boards (OV2640, N16R8, keyestudio MB0184 / Freenove style).
 * If camera init fails with 0x105, verify pins against your board silkscreen or vendor docs.
 */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2      8
#define CAM_PIN_D1      9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

/**
 * Largest frame the stream can be switched to at runtime (`?quality=high`), and
 * therefore the size the frame buffers are allocated for at init.
 * FRAMESIZE_VGA = 640x480.
 */
#define CAM_FRAME_SIZE       FRAMESIZE_VGA

/**
 * Default streaming resolution (FRAMESIZE_QVGA = 320x240).
 *
 * Small frames are what make the feed usable *while driving*: they encode and
 * clear the air faster, so video does not soak the same Wi-Fi the motor
 * commands need. Ask for `?quality=high` when detail matters more than latency.
 */
#define CAM_STREAM_FRAME_SIZE FRAMESIZE_QVGA

/**
 * JPEG quality, inverted: higher number = more compression = smaller frames.
 * 16 keeps a QVGA frame around 5-8 KB. Frame *size* is the lever that decides
 * whether video leaves any airtime for motor commands, so this is deliberately
 * not set to a "nice" low number.
 */
#define CAM_JPEG_QUALITY     16

/**
 * Two buffers, grabbed newest-first. More buffers do NOT mean smoother video —
 * they mean a queue of stale frames between the lens and the screen, which is
 * exactly the lag that makes driving by camera feel unresponsive.
 */
#define CAM_FB_COUNT         2

/**
 * Min ms between MJPEG frames (50 ms = 20 fps ceiling).
 *
 * Counter-intuitive but measured: running this uncapped made the video look
 * *slower*, not faster. The ESP32 pushed frames faster than the air could carry
 * them, so they piled up in the TCP and Wi-Fi queues and the browser ended up
 * displaying frames that were seconds old — and command packets queued behind
 * that backlog too (an 11.7 s gap between two commands sent 96 ms apart).
 *
 * Capping the rate keeps total bitrate under what the link can absorb, which
 * keeps queues empty. Empty queues are what "realtime" actually requires: fewer
 * frames per second, but each one arrives ~immediately. Raise this number
 * (lower fps) if video and controls still fight; lower it only if you have
 * airtime to spare.
 */
#define CAM_STREAM_FRAME_MS  50
