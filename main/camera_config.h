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

/** Default JPEG stream resolution (FRAMESIZE_VGA = 640x480). */
#define CAM_FRAME_SIZE       FRAMESIZE_VGA
#define CAM_JPEG_QUALITY     15
#define CAM_FB_COUNT         3
/** Min ms between MJPEG frames (~15 fps cap; WiFi AP cannot sustain VGA faster). */
#define CAM_STREAM_FRAME_MS  66
