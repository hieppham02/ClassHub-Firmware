#pragma once
// OV7670 maximum native resolution. RGB565 is two bytes per pixel.
#define CAMERA_WIDTH 640
#define CAMERA_HEIGHT 480
#define CAMERA_FRAME_BYTES (CAMERA_WIDTH * CAMERA_HEIGHT * 2)
#define CAMERA_PACKET_BYTES (CAMERA_FRAME_BYTES + 16)
