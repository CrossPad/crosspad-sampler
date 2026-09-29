// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

#include "crosspad-sampler/ui/waveform_view.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-gui/crosspad_gui.h"

#include <cstdlib>
#include <cstring>
#include <lvgl.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace crosspad_sampler {

// ── Internal state ──────────────────────────────────────────────────────

static WaveformDataProvider s_provider  = nullptr;
static void*                s_userData  = nullptr;
static WaveformFileProvider s_fileProvider = nullptr;
static void*                s_fileUserData = nullptr;

/// PCM buffer used during waveform_view_load() — one sample per canvas pixel column.
static int16_t s_waveformBuf[WAVEFORM_CANVAS_WIDTH];

/// Canvas pixel buffer — allocated from PSRAM on first use to avoid wasting DRAM.
static uint8_t* cbuf = nullptr;

/* What is on screen while it says "loading". The view retries on its own
 * timer rather than waiting for whichever panel owns it to tick: a waveform
 * that lands has to appear without the pad being pressed again, and both the
 * sampler panel and the pad editor redraw on schedules of their own. */
static lv_obj_t*  s_pendingView  = nullptr;
static lv_timer_t* s_pendingTimer = nullptr;
static bool       s_pendingIsFile = false;
static uint8_t    s_pendingPad   = 0;
static uint8_t    s_pendingSlot  = 0;
static char       s_pendingPath[160] = {};

static constexpr uint32_t kPendingPollMs = 300;

static void pending_stop();
static void pending_poll_cb(lv_timer_t*);
static constexpr uint32_t CBUF_BYTES = LV_CANVAS_BUF_SIZE(WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT, 16, 1);

/// Marker width, and the span a 0..1 position is mapped across: a marker sits
/// left-aligned on its column, so the last one it can occupy is width - 2.
static constexpr int32_t kMarkerW = 2;
static constexpr int32_t kSpan    = WAVEFORM_CANVAS_WIDTH - kMarkerW;

/* Where a view's trimmed region sits. The playhead is handed a position
 * inside *that* region -- the callers divide elapsed time by the trimmed
 * length, not by the file -- so progress 0 belongs under the start marker and
 * progress 1 under the end one. Two views exist at most (the sampler panel's
 * and the pad editor's); a third would simply travel the whole canvas. */
struct ViewBand { lv_obj_t* view; float start; float end; bool reversed; };
static ViewBand s_bands[4] = {};

static ViewBand* band_slot(lv_obj_t* view, bool create)
{
    for (auto& b : s_bands) if (b.view == view) return &b;
    if (!create) return nullptr;
    for (auto& b : s_bands) {
        if (!b.view) { b.view = view; b.start = 0.0f; b.end = 1.0f; b.reversed = false; return &b; }
    }
    return nullptr;
}

static void band_forget(lv_obj_t* view)
{
    for (auto& b : s_bands) if (b.view == view) b.view = nullptr;
}

// ── Helper: convert uint32_t 0xRRGGBB to lv_color_t ─────────────────────
static lv_color_t color_from_hex(uint32_t hex)
{
    return lv_color_make((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

// ── Public API ──────────────────────────────────────────────────────────

void waveform_view_set_provider(WaveformDataProvider fn, void* userData)
{
    s_provider = fn;
    s_userData = userData;
}

void waveform_view_set_file_provider(WaveformFileProvider fn, void* userData)
{
    s_fileProvider = fn;
    s_fileUserData = userData;
}

lv_obj_t* waveform_view_create(lv_obj_t* parent)
{
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(cont, color_from_hex(WAVEFORM_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);

    // Canvas, child 0. Its buffer comes from PSRAM when there is any.
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
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_fill_bg(canvas, color_from_hex(WAVEFORM_BG_COLOR), LV_OPA_COVER);

    // Playhead, child 1.
    lv_obj_t* playhead = lv_obj_create(cont);
    lv_obj_set_size(playhead, 2, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(playhead, color_from_hex(WAVEFORM_HEAD_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(playhead, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(playhead, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(playhead, 0, LV_PART_MAIN);
    lv_obj_align(playhead, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(playhead, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(playhead, LV_OBJ_FLAG_CLICKABLE);

    // Start marker, child 2.
    lv_obj_t* startMarker = lv_obj_create(cont);
    lv_obj_set_size(startMarker, 2, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(startMarker, color_from_hex(WAVEFORM_START_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(startMarker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(startMarker, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(startMarker, 0, LV_PART_MAIN);
    lv_obj_align(startMarker, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(startMarker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(startMarker, LV_OBJ_FLAG_CLICKABLE);

    // End marker, child 3.
    lv_obj_t* endMarker = lv_obj_create(cont);
    lv_obj_set_size(endMarker, 2, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(endMarker, color_from_hex(WAVEFORM_END_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(endMarker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(endMarker, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(endMarker, 0, LV_PART_MAIN);
    lv_obj_align(endMarker, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(endMarker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(endMarker, LV_OBJ_FLAG_CLICKABLE);

    // "loading", child 4, over the canvas. The turning ring for this is the
    // status bar's, driven from the panel; the canvas only says what it is
    // waiting for. Hidden until a provider answers Pending.
    lv_obj_t* pending = lv_label_create(cont);
    lv_label_set_text(pending, "loading");
    lv_obj_set_style_text_color(pending, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_center(pending);
    lv_obj_add_flag(pending, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(pending, LV_OBJ_FLAG_CLICKABLE);

    // Everything outside start..end, children 5 and 6: the part of the file
    // that will not be heard, greyed rather than hidden so the trim can be
    // read against the whole sample. They never overlap the markers or the
    // playhead, which is why they may sit on top of them in the child order.
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* dim = lv_obj_create(cont);
        lv_obj_set_size(dim, 1, WAVEFORM_CANVAS_HEIGHT);
        lv_obj_set_style_bg_color(dim, color_from_hex(WAVEFORM_DIM_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dim, LV_OPA_80, LV_PART_MAIN);
        lv_obj_set_style_border_width(dim, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(dim, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(dim, 0, LV_PART_MAIN);
        lv_obj_align(dim, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_flag(dim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    }

    // The poll holds a raw pointer to this view; its owner is free to delete
    // it at any time, and an app teardown does. So does the band table.
    lv_obj_add_event_cb(cont, [](lv_event_t* e) {
        lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
        if (s_pendingView == target) pending_stop();
        band_forget(target);
    }, LV_EVENT_DELETE, nullptr);

    return cont;
}

// Redraw the canvas from the first @p actualSamples entries of s_waveformBuf.
static void pending_stop()
{
    if (s_pendingTimer) { lv_timer_delete(s_pendingTimer); s_pendingTimer = nullptr; }
    s_pendingView = nullptr;
    s_pendingPath[0] = '\0';
}

/// Show or hide the "loading" label, child 4. An empty box means "this pad
/// holds nothing"; a waveform on its way says so instead. While it is up, a
/// timer re-asks the provider so the drawing replaces it by itself.
static void set_pending_label(lv_obj_t* waveformView, bool pending)
{
    lv_obj_t* label = lv_obj_get_child(waveformView, 4);
    if (label) {
        if (pending) lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }

    if (!pending) {
        if (s_pendingView == waveformView) pending_stop();
        return;
    }

    s_pendingView = waveformView;
    if (!s_pendingTimer) {
        s_pendingTimer = lv_timer_create(pending_poll_cb, kPendingPollMs, nullptr);
    }
}

static void draw_from_buffer(lv_obj_t* waveformView, uint32_t actualSamples)
{
    const ViewBand* band = band_slot(waveformView, false);
    const bool reversed = band && band->reversed;
    lv_obj_t* canvas = lv_obj_get_child(waveformView, 0);
    if (!canvas) return;

    lv_canvas_fill_bg(canvas, color_from_hex(WAVEFORM_BG_COLOR), LV_OPA_COVER);

    if (actualSamples < 2) return;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color   = color_from_hex(WAVEFORM_WAVE_COLOR);
    dsc.width   = 1;
    dsc.opa     = LV_OPA_COVER;

    const int32_t midY  = WAVEFORM_CANVAS_HEIGHT / 2;
    const float   scale = static_cast<float>(midY) / 32768.0f;

    // One bar per column, -|peak| to +|peak|. Each cached sample is its
    // bin's abs-max, so mirroring it gives the usual symmetric envelope and
    // keeps transients visible in long bins, where a point-to-point line
    // only zigzagged near the midline.
    for (uint32_t i = 0; i < actualSamples; ++i) {
        int16_t v   = s_waveformBuf[i];
        int32_t mag = static_cast<int32_t>((v < 0 ? -v : v) * scale);
        if (mag < 1) mag = 1;

        /* A reversed sample is drawn as it is heard: its end first. */
        int32_t x   = static_cast<int32_t>(reversed ? actualSamples - 1 - i : i);
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

void waveform_view_load(lv_obj_t* waveformView, uint8_t padIdx, uint8_t sampleSlot)
{
    if (!s_provider) { LV_LOG_WARN("waveform_view_load: no provider!"); return; }
    if (!lv_obj_get_child(waveformView, 0)) return;

    // Fetch sample data downsampled to WAVEFORM_CANVAS_WIDTH columns.
    uint32_t actualSamples = 0;
    const WaveformStatus st = s_provider(padIdx, sampleSlot,
               s_waveformBuf, static_cast<uint32_t>(WAVEFORM_CANVAS_WIDTH),
               &actualSamples, s_userData);

    s_pendingIsFile = false;
    s_pendingPad    = padIdx;
    s_pendingSlot   = sampleSlot;

    draw_from_buffer(waveformView, actualSamples);
    set_pending_label(waveformView, st == WaveformStatus::Pending);
}

void waveform_view_load_file(lv_obj_t* waveformView, const char* path)
{
    if (!s_fileProvider || !path || !path[0]) return;
    if (!lv_obj_get_child(waveformView, 0)) return;

    uint32_t actualSamples = 0;
    const WaveformStatus st =
        s_fileProvider(path, s_waveformBuf, static_cast<uint32_t>(WAVEFORM_CANVAS_WIDTH),
                       &actualSamples, s_fileUserData);

    s_pendingIsFile = true;
    snprintf(s_pendingPath, sizeof(s_pendingPath), "%s", path);

    draw_from_buffer(waveformView, actualSamples);
    set_pending_label(waveformView, st == WaveformStatus::Pending);
}

static void pending_poll_cb(lv_timer_t*)
{
    lv_obj_t* view = s_pendingView;
    if (!view) { pending_stop(); return; }

    if (s_pendingIsFile) waveform_view_load_file(view, s_pendingPath);
    else                 waveform_view_load(view, s_pendingPad, s_pendingSlot);
}

void waveform_view_clear(lv_obj_t* waveformView)
{
    lv_obj_t* canvas = lv_obj_get_child(waveformView, 0);
    if (canvas) {
        lv_canvas_fill_bg(canvas, color_from_hex(WAVEFORM_BG_COLOR), LV_OPA_COVER);
    }

    if (s_pendingView == waveformView) pending_stop();

    if (ViewBand* b = band_slot(waveformView, false)) { b->start = 0.0f; b->end = 1.0f; }

    // Hide the markers, the playhead, the loading label and the dimming.
    for (int i = 1; i <= 6; ++i) {
        lv_obj_t* child = lv_obj_get_child(waveformView, i);
        if (child) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/// Place one dimming overlay, or hide it when it would be empty -- a trim that
/// starts at zero has nothing to grey on the left.
static void place_dim(lv_obj_t* dim, int32_t x, int32_t w)
{
    if (!dim) return;
    if (w <= 0) { lv_obj_add_flag(dim, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_set_size(dim, w, WAVEFORM_CANVAS_HEIGHT);
    lv_obj_align(dim, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_HIDDEN);
}

void waveform_view_set_reversed(lv_obj_t* waveformView, bool reversed)
{
    if (ViewBand* b = band_slot(waveformView, true)) b->reversed = reversed;
}

void waveform_view_set_markers(lv_obj_t* waveformView, float startNorm, float endNorm)
{
    if (startNorm < 0.0f) startNorm = 0.0f;
    if (endNorm   > 1.0f) endNorm   = 1.0f;
    if (endNorm   < startNorm) endNorm = startNorm;
    /* Reversed, the picture is mirrored: what is heard first -- the trim's
     * end -- is on the left, under the start marker. */
    if (const ViewBand* b = band_slot(waveformView, false); b && b->reversed) {
        const float s = 1.0f - endNorm;
        endNorm = 1.0f - startNorm;
        startNorm = s;
    }

    const int32_t startX = static_cast<int32_t>(startNorm * kSpan);
    const int32_t endX   = static_cast<int32_t>(endNorm   * kSpan);

    lv_obj_t* startMarker = lv_obj_get_child(waveformView, 2);
    lv_obj_t* endMarker   = lv_obj_get_child(waveformView, 3);

    if (startMarker) {
        lv_obj_align(startMarker, LV_ALIGN_LEFT_MID, startX, 0);
        lv_obj_clear_flag(startMarker, LV_OBJ_FLAG_HIDDEN);
    }

    if (endMarker) {
        lv_obj_align(endMarker, LV_ALIGN_LEFT_MID, endX, 0);
        lv_obj_clear_flag(endMarker, LV_OBJ_FLAG_HIDDEN);
    }

    // Up to the start marker, and from the far side of the end one.
    place_dim(lv_obj_get_child(waveformView, 5), 0, startX);
    place_dim(lv_obj_get_child(waveformView, 6), endX + kMarkerW,
              WAVEFORM_CANVAS_WIDTH - (endX + kMarkerW));

    if (ViewBand* b = band_slot(waveformView, true)) { b->start = startNorm; b->end = endNorm; }
}

void waveform_view_set_playhead(lv_obj_t* waveformView, float position)
{
    lv_obj_t* playhead = lv_obj_get_child(waveformView, 1);
    if (!playhead) return;

    // Negative is how the callers say "not playing".
    if (position < 0.0f) { lv_obj_add_flag(playhead, LV_OBJ_FLAG_HIDDEN); return; }
    if (position > 1.0f) position = 1.0f;

    /* Progress is measured over the trimmed part, so it has to land there:
     * a sample trimmed to its second half starts under its start marker and
     * ends under the end one, not at the edges of the canvas. */
    float start = 0.0f, end = 1.0f;
    if (const ViewBand* b = band_slot(waveformView, false)) { start = b->start; end = b->end; }

    const float mapped = start + position * (end - start);
    lv_obj_align(playhead, LV_ALIGN_LEFT_MID, static_cast<int32_t>(mapped * kSpan), 0);
    lv_obj_clear_flag(playhead, LV_OBJ_FLAG_HIDDEN);
}

} // namespace crosspad_sampler

#endif // USE_LVGL
