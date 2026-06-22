#pragma once
// M0 bring-up: CO5300 AMOLED (Arduino_GFX over QSPI) + LVGL.
// Owns the panel + LVGL display driver so main.cpp stays glue-only.
// Touch (CST9217 indev) and the radar UI come in later milestones.
#include <stdint.h>

namespace display {

// Init the panel and LVGL (draw buffers in PSRAM) and show the M0 hello screen.
// Returns false if the panel failed to initialize.
bool begin();

// Pump LVGL: render dirty areas + run LVGL timers. Call every loop() iteration.
void loop();

// 0..255 panel brightness (CO5300 command 0x51).
void setBrightness(uint8_t v);

// ms since the last touch (LVGL inactivity timer) — for idle auto-dim.
uint32_t inactiveMs();

// Rotate the whole UI in 90° steps (0/1/2/3 = 0°/90°/180°/270°), e.g. to mount with the
// USB-C port on any side. Applied in the flush (180° in place; 90/270° via a PSRAM
// scratch buffer) and mirrored on touch input.
void setRotation(uint8_t quarters);
uint8_t rotation();

} // namespace display

uint32_t display_frames();   // total rendered frames (for FPS measurement)
