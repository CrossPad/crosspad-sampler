// SPDX-License-Identifier: MIT

#include "crosspad-sampler/waveform/waveform_draw.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/waveform/waveform_loader.hpp"
#include "crosspad-sampler/waveform/waveform_cache_ui.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"

// crosspad-gui styles
#include "crosspad-gui/crosspad_gui.h"

#include <cmath>
#include <cstdio>

namespace crosspad_sampler {

// ── dB-like scaling (sqrt approximation) ────────────────────────────────

static inline int fast_db_scale(int16_t amplitude, int channelHeight) {
    int abs_amp = abs(amplitude);
    if (abs_amp < 328) return 0;  // ~< -48 dB
    float normalized = sqrtf((float)abs_amp / 32768.0f);
    return (int)(normalized * (channelHeight / 2));
}

// ── Async draw state ────────────────────────────────────────────────────

struct AsyncDrawState {
    lv_obj_t*  canvas       = nullptr;
    int16_t*   waveformData = nullptr;
    uint32_t   sampleCount  = 0;
    int        currentX     = 0;
    bool       active       = false;
    bool       yScaleLinear = false;
    lv_timer_t* timer       = nullptr;
};

static AsyncDrawState s_draw = {};

static void async_draw_timer_cb(lv_timer_t* timer) {
    if (!s_draw.active || !s_draw.canvas || !s_draw.waveformData) {
        if (s_draw.timer) { lv_timer_delete(s_draw.timer); s_draw.timer = nullptr; }
        s_draw.active = false;
        return;
    }
    if (!lv_obj_is_valid(s_draw.canvas)) {
        if (s_draw.timer) { lv_timer_delete(s_draw.timer); s_draw.timer = nullptr; }
        s_draw.active = false;
        return;
    }

    const int chunkSize = 20;
    int endX = s_draw.currentX + chunkSize;
    bool isFirst = (s_draw.currentX == 0);

    waveform_draw_chunk(s_draw.canvas, s_draw.waveformData,
                        s_draw.sampleCount, s_draw.currentX, endX,
                        WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT, isFirst);

    s_draw.currentX = endX;
    if (s_draw.currentX >= WAVEFORM_CANVAS_WIDTH) {
        if (s_draw.timer) { lv_timer_delete(s_draw.timer); s_draw.timer = nullptr; }
        s_draw.active = false;
    }
}

// ── Public API ──────────────────────────────────────────────────────────

void waveform_draw_chunk(lv_obj_t* canvas, int16_t* waveform, uint32_t sampleCount,
                         int startX, int endX, int width, int height, bool clearFirst) {
    if (!canvas || !waveform || sampleCount == 0) return;

    if (clearFirst) {
        lv_canvas_fill_bg(canvas, lv_color_hex3(0x000), LV_OPA_COVER);
    }

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);

    uint32_t samplesPerChannel = sampleCount / 2;
    int channelHeight = height / 2 - 2;
    int leftCenterY  = channelHeight / 2 + 1;
    int rightCenterY = height / 2 + channelHeight / 2 + 1;
    float samplesPerPixel = (float)samplesPerChannel / (float)width;

    // Grid + center lines on first chunk
    if (clearFirst || startX == 0) {
        dsc.color = lv_color_hex3(0x333);
        dsc.width = 1;
        dsc.p1.x = 0; dsc.p1.y = leftCenterY;
        dsc.p2.x = width; dsc.p2.y = leftCenterY;
        lv_draw_line(&layer, &dsc);
        dsc.p1.y = rightCenterY; dsc.p2.y = rightCenterY;
        lv_draw_line(&layer, &dsc);

        dsc.color = lv_color_hex3(0x222);
        for (int gx = 15; gx < width; gx += 15) {
            dsc.p1.x = gx; dsc.p1.y = 0;
            dsc.p2.x = gx; dsc.p2.y = height;
            lv_draw_line(&layer, &dsc);
        }
    }

    // Waveform
    dsc.color = lv_color_hex(0x00FFAA);
    dsc.width = 1;

    for (int x = startX; x < endX && x < width - 1; x++) {
        uint32_t ss = (uint32_t)(x * samplesPerPixel);
        uint32_t se = (uint32_t)((x + 1) * samplesPerPixel);

        int16_t lMin = 0, lMax = 0, rMin = 0, rMax = 0;
        for (uint32_t s = ss; s < se && s < samplesPerChannel; s++) {
            int16_t l = waveform[s * 2];
            int16_t r = waveform[s * 2 + 1];
            if (l < lMin) lMin = l;
            if (l > lMax) lMax = l;
            if (r < rMin) rMin = r;
            if (r > rMax) rMax = r;
        }

        int lMaxO, lMinO, rMaxO, rMinO;
        if (s_draw.yScaleLinear) {
            lMaxO = (abs(lMax) * channelHeight / 2) / 32768;
            lMinO = (abs(lMin) * channelHeight / 2) / 32768;
            rMaxO = (abs(rMax) * channelHeight / 2) / 32768;
            rMinO = (abs(rMin) * channelHeight / 2) / 32768;
        } else {
            lMaxO = fast_db_scale(lMax, channelHeight);
            lMinO = fast_db_scale(lMin, channelHeight);
            rMaxO = fast_db_scale(rMax, channelHeight);
            rMinO = fast_db_scale(rMin, channelHeight);
        }

        int lY1 = leftCenterY  - (lMax >= 0 ? lMaxO : -lMaxO);
        int lY2 = leftCenterY  - (lMin >= 0 ? lMinO : -lMinO);
        int rY1 = rightCenterY - (rMax >= 0 ? rMaxO : -rMaxO);
        int rY2 = rightCenterY - (rMin >= 0 ? rMinO : -rMinO);

        dsc.p1.x = x; dsc.p1.y = lY1; dsc.p2.x = x; dsc.p2.y = lY2;
        lv_draw_line(&layer, &dsc);
        dsc.p1.y = rY1; dsc.p2.y = rY2;
        lv_draw_line(&layer, &dsc);
    }

    lv_canvas_finish_layer(canvas, &layer);
}

void waveform_draw_on_canvas(lv_obj_t* canvas, int16_t* waveform, uint32_t sampleCount,
                             int width, int height) {
    if (!canvas || !waveform || sampleCount == 0) return;
    waveform_draw_chunk(canvas, waveform, sampleCount, 0, width, width, height, true);
}

void waveform_start_async_draw(lv_obj_t* canvas, int16_t* waveformData,
                               uint32_t sampleCount, bool useLinearScale) {
    if (!canvas || !waveformData || sampleCount == 0) return;

    if (s_draw.timer) { lv_timer_delete(s_draw.timer); s_draw.timer = nullptr; }

    s_draw.canvas       = canvas;
    s_draw.waveformData = waveformData;
    s_draw.sampleCount  = sampleCount;
    s_draw.currentX     = 0;
    s_draw.active       = true;
    s_draw.yScaleLinear = useLinearScale;
    s_draw.timer        = lv_timer_create(async_draw_timer_cb, 5, nullptr);
    lv_timer_set_repeat_count(s_draw.timer, -1);
}

void waveform_clear_drawing_state() {
    if (s_draw.timer) { lv_timer_delete(s_draw.timer); s_draw.timer = nullptr; }
    s_draw.active = false;
    s_draw.canvas = nullptr;
    s_draw.waveformData = nullptr;
    waveform_loader_clear_queue();
}

bool waveform_load_and_draw(lv_obj_t* canvas, const char* filename, bool forceSync) {
    if (!canvas || !filename) return false;

    WaveformCacheEntry* entry = waveform_cache_find(filename);
    if (entry && entry->valid && entry->waveformData && entry->sampleCount > 0) {
        waveform_start_async_draw(canvas, entry->waveformData, entry->sampleCount);
        return true;
    }

    // Not in cache — request async load
    if (waveform_loader_is_running()) {
        waveform_loader_request(filename, true);  // high priority
        lv_canvas_fill_bg(canvas, lv_color_hex3(0x333), LV_OPA_COVER);
    } else {
        lv_canvas_fill_bg(canvas, lv_color_hex3(0x333), LV_OPA_COVER);
    }
    return false;
}

void waveform_update_display(lv_obj_t* canvas, const char* filename) {
    if (!canvas || !filename) return;

    WaveformCacheEntry* entry = waveform_cache_find(filename);
    if (entry && entry->valid && entry->waveformData && entry->sampleCount > 0) {
        waveform_start_async_draw(canvas, entry->waveformData, entry->sampleCount);
        return;
    }

    lv_canvas_fill_bg(canvas, lv_color_hex3(0x333), LV_OPA_COVER);
    if (waveform_loader_is_running()) {
        waveform_loader_request(filename, true);
    }
}

void waveform_draw_empty(lv_obj_t* canvas) {
    if (!canvas) return;

    int32_t width  = lv_obj_get_width(canvas);
    int32_t height = lv_obj_get_height(canvas);

    lv_canvas_fill_bg(canvas, lv_color_hex3(0x222), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(0x666666);

    int ch = height / 2 - 2;
    int lCy = ch / 2 + 1;
    int rCy = height / 2 + ch / 2 + 1;

    float freq = 3.0f;
    int prevL = lCy, prevR = rCy;

    for (int x = 1; x < width; x++) {
        float a = (x / (float)width) * freq * 2.0f * (float)M_PI;
        int curL = lCy + (int)(sinf(a) * (ch / 2.5f));
        int curR = rCy + (int)(sinf(a + (float)M_PI / 4.0f) * (ch / 2.5f));

        dsc.p1.x = x - 1; dsc.p1.y = prevL; dsc.p2.x = x; dsc.p2.y = curL;
        lv_draw_line(&layer, &dsc);
        dsc.p1.y = prevR; dsc.p2.y = curR;
        lv_draw_line(&layer, &dsc);

        prevL = curL; prevR = curR;
    }

    lv_canvas_finish_layer(canvas, &layer);
}

lv_obj_t* waveform_create_display(lv_obj_t* parent, const char* filename) {
    auto* drawBuf = lv_draw_buf_create(WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT,
                                        LV_COLOR_FORMAT_RGB565, 0);
    if (!drawBuf) {
        lv_obj_t* box = lv_obj_create(parent);
        lv_obj_set_size(box, WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT);
        lv_obj_t* lbl = lv_label_create(box);
        lv_label_set_text(lbl, "No RAM for waveform");
        return nullptr;
    }

    lv_obj_t* canvas = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(canvas, drawBuf);
    lv_canvas_fill_bg(canvas, lv_color_hex3(0x222), LV_OPA_COVER);
    lv_obj_set_size(canvas, WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT);

    // Apply waveform box style from crosspad-gui (declared in crosspad-gui/styles/styles.h)
    lv_obj_add_style(canvas, &styleWaveformBox, 0);

    if (filename && filename[0]) {
        waveform_load_and_draw(canvas, filename, false);
    }

    // Free draw buffer on canvas delete
    lv_obj_add_event_cb(canvas, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_DELETE) {
            auto* db = (lv_draw_buf_t*)lv_event_get_user_data(e);
            if (db) lv_draw_buf_destroy(db);
        }
    }, LV_EVENT_DELETE, drawBuf);

    return canvas;
}

} // namespace crosspad_sampler
