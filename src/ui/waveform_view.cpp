// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

#include "crosspad-sampler/ui/waveform_view.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"

#include <cstring>
#include <lvgl.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace crosspad_sampler {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static WaveformDataProvider s_provider  = nullptr;
static void*                s_userData  = nullptr;

/// PCM buffer used during waveform_view_load() — one sample per canvas pixel column.
static int16_t s_waveformBuf[WAVEFORM_CANVAS_WIDTH];

/// Canvas pixel buffer — allocated from PSRAM on first use to avoid wasting DRAM.
static uint8_t* cbuf = nullptr;
static constexpr uint32_t CBUF_BYTES = LV_CANVAS_BUF_SIZE(WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT, 16, 1);

// ---------------------------------------------------------------------------
// Helper: convert uint32_t 0xRRGGBB to lv_color_t
// ---------------------------------------------------------------------------
static lv_color_t color_from_hex(uint32_t hex)
{
    return lv_color_make((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void waveform_view_set_provider(WaveformDataProvider fn, void* userData)
{
    s_provider = fn;
    s_userData = userData;
}

lv_obj_t* waveform_view_create(lv_obj_t* parent)
{
    // --- Container ---
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // --- Canvas (child index 0) ---
    // Allocate canvas buffer from PSRAM if available, else heap
    if (!cbuf) {
#ifdef ESP_PLATFORM
        cbuf = static_cast<uint8_t*>(heap_caps_malloc(CBUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
        if (!cbuf) {
            cbuf = static_cast<uint8_t*>(malloc(CBUF_BYTES));
        }
    }
    lv_obj_t* canvas = lv_canvas_create(cont);
    lv_canvas_set_buffer(canvas, cbuf, WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(canvas, WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_fill_bg(canvas, lv_color_hex(0x111111), LV_OPA_COVER);

    // --- Playhead (child index 1) — 2 px wide red vertical line ---
    lv_obj_t* playhead = lv_obj_create(cont);
    lv_obj_set_size(playhead, 2, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(playhead, color_from_hex(WAVEFORM_HEAD_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(playhead, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(playhead, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(playhead, 0, LV_PART_MAIN);
    lv_obj_align(playhead, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(playhead, LV_OBJ_FLAG_HIDDEN);

    // --- Start marker (child index 2) — 2 px wide green vertical line ---
    lv_obj_t* startMarker = lv_obj_create(cont);
    lv_obj_set_size(startMarker, 2, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(startMarker, color_from_hex(WAVEFORM_START_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(startMarker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(startMarker, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(startMarker, 0, LV_PART_MAIN);
    lv_obj_align(startMarker, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(startMarker, LV_OBJ_FLAG_HIDDEN);

    // --- End marker (child index 3) — 2 px wide red vertical line ---
    lv_obj_t* endMarker = lv_obj_create(cont);
    lv_obj_set_size(endMarker, 2, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(endMarker, color_from_hex(WAVEFORM_END_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(endMarker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(endMarker, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(endMarker, 0, LV_PART_MAIN);
    lv_obj_align(endMarker, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(endMarker, LV_OBJ_FLAG_HIDDEN);

    return cont;
}

void waveform_view_load(lv_obj_t* waveformView, uint8_t padIdx, uint8_t sampleSlot)
{
    if (!s_provider) { LV_LOG_WARN("waveform_view_load: no provider!"); return; }

    lv_obj_t* canvas = lv_obj_get_child(waveformView, 0);
    if (!canvas) return;

    // Fetch sample data downsampled to WAVEFORM_CANVAS_WIDTH columns.
    uint32_t actualSamples = 0;
    s_provider(padIdx, sampleSlot,
               s_waveformBuf, static_cast<uint32_t>(WAVEFORM_CANVAS_WIDTH),
               &actualSamples, s_userData);

    // Clear background.
    lv_canvas_fill_bg(canvas, lv_color_hex(0x111111), LV_OPA_COVER);

    if (actualSamples < 2) return;

    // Begin LVGL v9 draw layer.
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color   = lv_color_hex(0x00AAFF);
    dsc.width   = 1;
    dsc.opa     = LV_OPA_COVER;

    const int32_t midY  = WAVEFORM_CANVAS_HEIGHT / 2;
    const float   scale = static_cast<float>(midY) / 32768.0f;

    // One vertical bar per column from -|peak| to +|peak|. Each cached
    // sample is the abs-max of its source bin, so |peak| is the bin's
    // loudness; mirroring it gives the standard symmetric envelope. This
    // keeps loud transients visible inside long high-compression bins
    // where the prior point-to-point line would zigzag near midY.
    for (uint32_t i = 0; i < actualSamples; ++i) {
        int16_t v   = s_waveformBuf[i];
        int32_t mag = static_cast<int32_t>((v < 0 ? -v : v) * scale);
        if (mag < 1) mag = 1;

        int32_t x   = static_cast<int32_t>(i);
        int32_t yTop = midY - mag;
        int32_t yBot = midY + mag;

        dsc.p1 = lv_point_precise_t{ static_cast<lv_value_precise_t>(x),
                                     static_cast<lv_value_precise_t>(yTop) };
        dsc.p2 = lv_point_precise_t{ static_cast<lv_value_precise_t>(x),
                                     static_cast<lv_value_precise_t>(yBot) };

        lv_draw_line(&layer, &dsc);
    }

    lv_canvas_finish_layer(canvas, &layer);
}

void waveform_view_clear(lv_obj_t* waveformView)
{
    lv_obj_t* canvas = lv_obj_get_child(waveformView, 0);
    if (canvas) {
        lv_canvas_fill_bg(canvas, lv_color_hex(0x111111), LV_OPA_COVER);
    }

    // Hide all markers and the playhead.
    for (int i = 1; i <= 3; ++i) {
        lv_obj_t* child = lv_obj_get_child(waveformView, i);
        if (child) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void waveform_view_set_markers(lv_obj_t* waveformView, float startNorm, float endNorm)
{
    lv_obj_t* startMarker = lv_obj_get_child(waveformView, 2);
    lv_obj_t* endMarker   = lv_obj_get_child(waveformView, 3);

    if (startMarker) {
        int32_t x = static_cast<int32_t>(startNorm * (WAVEFORM_CANVAS_WIDTH - 2));
        lv_obj_align(startMarker, LV_ALIGN_LEFT_MID, x, 0);
        lv_obj_clear_flag(startMarker, LV_OBJ_FLAG_HIDDEN);
    }

    if (endMarker) {
        int32_t x = static_cast<int32_t>(endNorm * (WAVEFORM_CANVAS_WIDTH - 2));
        lv_obj_align(endMarker, LV_ALIGN_LEFT_MID, x, 0);
        lv_obj_clear_flag(endMarker, LV_OBJ_FLAG_HIDDEN);
    }
}

void waveform_view_set_playhead(lv_obj_t* waveformView, float position)
{
    lv_obj_t* playhead = lv_obj_get_child(waveformView, 1);
    if (!playhead) return;

    int32_t x = static_cast<int32_t>(position * (WAVEFORM_CANVAS_WIDTH - 2));
    lv_obj_align(playhead, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_clear_flag(playhead, LV_OBJ_FLAG_HIDDEN);
}

} // namespace crosspad_sampler

#endif // USE_LVGL
