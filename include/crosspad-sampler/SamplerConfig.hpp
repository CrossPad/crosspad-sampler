#pragma once

#include <cstdint>

namespace crosspad_sampler {

// Waveform display dimensions
constexpr int WAVEFORM_CANVAS_WIDTH  = 150;
constexpr int WAVEFORM_CANVAS_HEIGHT = 70;

// Timer intervals (milliseconds)
constexpr int PAD_UPDATE_INTERVAL_MS = 50;

// Pad grid
constexpr int PAD_COUNT = 16;
constexpr int PAD_GRID_COLS = 4;
constexpr int PAD_GRID_ROWS = 4;

// Default colors (uint32_t RRGGBB)
constexpr uint32_t DEFAULT_PAD_COLOR     = 0x20FF10;
constexpr uint32_t DEFAULT_PRESSED_COLOR = 0xFF1020;
constexpr uint32_t DEFAULT_PLAYING_COLOR = 0x320005;

// Waveform marker colors
constexpr uint32_t WAVEFORM_START_COLOR = 0x00FF00;  // green
constexpr uint32_t WAVEFORM_END_COLOR   = 0xFF0000;  // red
constexpr uint32_t WAVEFORM_HEAD_COLOR  = 0xFF0000;  // red playhead

} // namespace crosspad_sampler
