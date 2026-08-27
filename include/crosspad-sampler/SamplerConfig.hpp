#pragma once

#include <cstdint>

namespace crosspad_sampler {

// Waveform display dimensions
constexpr int WAVEFORM_CANVAS_WIDTH  = 150;
constexpr int WAVEFORM_CANVAS_HEIGHT = 70;

// Timer intervals (milliseconds)
constexpr int PAD_UPDATE_INTERVAL_MS = 50;

// Pad grid
constexpr int PAD_COUNT     = 16;
constexpr int PAD_GRID_COLS = 4;
constexpr int PAD_GRID_ROWS = 4;
// KIT_PADS is defined as a preprocessor macro by crosspad-core's KitInfo.hpp.
// Do NOT redefine it here — just include KitInfo.hpp where needed.

// Default colors (uint32_t RRGGBB)
constexpr uint32_t DEFAULT_PAD_COLOR     = 0x20FF10;
constexpr uint32_t DEFAULT_PRESSED_COLOR = 0xFF1020;
constexpr uint32_t DEFAULT_PLAYING_COLOR = 0x320005;

// Waveform marker colors
constexpr uint32_t WAVEFORM_START_COLOR = 0x00FF00;  // green
constexpr uint32_t WAVEFORM_END_COLOR   = 0xFF0000;  // red
constexpr uint32_t WAVEFORM_HEAD_COLOR  = 0xFF0000;  // red playhead

// Pad note mapping: physical pad index (0-15) -> displayed label number (1-16)
// Bottom-left is pad 0 displayed as "1", top-right is pad 15 displayed as "16"
constexpr int PAD_NOTES[] = {13, 14, 15, 16, 9, 10, 11, 12, 5, 6, 7, 8, 1, 2, 3, 4};

// Max sample slots per pad
constexpr int MAX_SAMPLES_PER_PAD = 1;

// Default kit path
constexpr const char* DEFAULT_KIT_PATH = "/crosspad/kits";

// Loop end sentinel value (max uint32_t)
constexpr uint32_t SAMPLE_END_MAX = 0xFFFFFFFF;

// Waveform cache
// Every layer of every pad is cached, not only the first: a velocity-split or
// round-robin pad shows the layer that fired. 600 samples × 2 B each, in PSRAM.
constexpr int WAVEFORM_CACHE_SIZE = 64;
constexpr uint32_t TARGET_CACHE_SAMPLES = 600; // Fixed target cache sample count

// Waveform loader (FreeRTOS task)
constexpr int LOADER_QUEUE_SIZE  = 32;
constexpr int LOADER_STACK_SIZE  = 6192;
constexpr int LOADER_PRIORITY    = 2;
constexpr int LOADER_CORE        = 0;    // Core 0 (LVGL on Core 1)

} // namespace crosspad_sampler
