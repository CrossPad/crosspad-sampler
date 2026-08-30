// SPDX-License-Identifier: MIT

#include "crosspad-sampler/waveform/waveform_cache_ui.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/waveform/waveform_loader.hpp"
#include "crosspad-sampler/waveform/waveform_draw.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/platform/PlatformServices.hpp>

// crosspad-gui shared styles & input devices
#include "crosspad-gui/crosspad_gui.h"

#include <cmath>
#include <cstdio>

namespace crosspad_sampler {

// ── State ───────────────────────────────────────────────────────────────

bool userAskedAboutCache = false;
bool userWantsCaching    = false;

static lv_obj_t*   s_promptOverlay  = nullptr;
static lv_obj_t*   s_prevFocused    = nullptr;
static lv_group_t* s_promptGroup    = nullptr;

static lv_obj_t*   s_progressOverlay = nullptr;
static lv_obj_t*   s_progressBar     = nullptr;
static lv_obj_t*   s_progressLabel   = nullptr;
static lv_timer_t* s_progressTimer   = nullptr;

// ── Focus helpers — uses crosspad-gui's encoder input device ────────────

static void restore_focus_after_prompt() {
    if (s_promptGroup) { lv_group_delete(s_promptGroup); s_promptGroup = nullptr; }
    if (s_prevFocused) {
        lv_group_t* g = lv_obj_get_group(s_prevFocused);
        if (g) { lv_indev_set_group(crosspad_gui::getGuiPlatform().getNavigationEncoder(), g); lv_group_focus_obj(s_prevFocused); }
        s_prevFocused = nullptr;
    }
}

// ── Public API ──────────────────────────────────────────────────────────

void waveform_ui_draw_empty(lv_obj_t* canvas) {
    waveform_draw_empty(canvas);
}

void waveform_ui_create_prompt(lv_obj_t* canvas) {
    if (s_promptOverlay) return;

    // Save current focus
    lv_group_t* cur = lv_indev_get_group(crosspad_gui::getGuiPlatform().getNavigationEncoder());
    if (cur) s_prevFocused = lv_group_get_focused(cur);

    lv_obj_t* parent = lv_obj_get_parent(canvas);
    if (!parent) return;

    s_promptOverlay = lv_obj_create(parent);
    lv_obj_set_size(s_promptOverlay, lv_obj_get_width(canvas), lv_obj_get_height(canvas));
    lv_obj_set_pos(s_promptOverlay, lv_obj_get_x(canvas), lv_obj_get_y(canvas));
    lv_obj_set_style_bg_color(s_promptOverlay, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_promptOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_promptOverlay, 0, 0);
    lv_obj_set_style_pad_all(s_promptOverlay, 1, 0);

    s_promptGroup = lv_group_create();
    lv_indev_set_group(crosspad_gui::getGuiPlatform().getNavigationEncoder(), s_promptGroup);

    // Question
    lv_obj_t* label = lv_label_create(s_promptOverlay);
    lv_label_set_text(label, "Cache all\n.wav files\nat once?");
    lv_obj_set_style_text_color(label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 5, 0);

    // Yes button
    lv_obj_t* yesBtn = lv_button_create(s_promptOverlay);
    lv_obj_set_size(yesBtn, 60, 25);
    lv_obj_align(yesBtn, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_add_style(yesBtn, &styleButton, 0);
    lv_obj_add_style(yesBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_t* yl = lv_label_create(yesBtn);
    lv_label_set_text(yl, "Yes");
    lv_obj_set_style_text_color(yl, lv_color_hex(0x000000), 0);
    lv_obj_center(yl);
    lv_group_add_obj(s_promptGroup, yesBtn);

    lv_obj_add_event_cb(yesBtn, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        userAskedAboutCache = true;
        userWantsCaching    = true;

        lv_obj_t* cv = (lv_obj_t*)lv_event_get_user_data(e);
        restore_focus_after_prompt();
        waveform_ui_destroy_prompt();

        // Count pads and start caching
        auto* mgr = crosspad::getPlatformServices().kitManager;
        auto* kit = mgr ? mgr->getCurrentKit() : nullptr;
        if (kit) {
            totalWaveformsToCache = 0;
            waveformsCached = 0;
            for (int i = 0; i < PAD_COUNT; i++) {
                if (!kit->pads[i].samples.empty()) totalWaveformsToCache++;
            }
            if (cv && totalWaveformsToCache > 0) {
                waveform_ui_create_progress(cv);
                waveform_ui_update_progress(0, totalWaveformsToCache);
            }
            waveform_loader_start();
            waveform_loader_preload_kit(PAD_COUNT);
        }
    }, LV_EVENT_CLICKED, canvas);

    // No button
    lv_obj_t* noBtn = lv_button_create(s_promptOverlay);
    lv_obj_set_size(noBtn, 60, 25);
    lv_obj_align(noBtn, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_add_style(noBtn, &styleButton, 0);
    lv_obj_add_style(noBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_t* nl = lv_label_create(noBtn);
    lv_label_set_text(nl, "No");
    lv_obj_set_style_text_color(nl, lv_color_hex(0x000000), 0);
    lv_obj_center(nl);
    lv_group_add_obj(s_promptGroup, noBtn);

    lv_obj_add_event_cb(noBtn, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        userAskedAboutCache = true;
        userWantsCaching    = false;

        lv_obj_t* cv = (lv_obj_t*)lv_event_get_user_data(e);
        restore_focus_after_prompt();
        waveform_ui_destroy_prompt();
        if (cv) waveform_draw_empty(cv);
    }, LV_EVENT_CLICKED, canvas);
}

void waveform_ui_destroy_prompt() {
    if (s_promptOverlay) {
        lv_obj_delete_async(s_promptOverlay);
        s_promptOverlay = nullptr;
    }
}

void waveform_ui_create_progress(lv_obj_t* canvas) {
    if (s_progressOverlay) return;

    lv_obj_t* parent = lv_obj_get_parent(canvas);
    if (!parent) return;

    s_progressOverlay = lv_obj_create(parent);
    lv_obj_set_size(s_progressOverlay, lv_obj_get_width(canvas), lv_obj_get_height(canvas));
    lv_obj_set_pos(s_progressOverlay, lv_obj_get_x(canvas), lv_obj_get_y(canvas));
    lv_obj_set_style_bg_color(s_progressOverlay, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_progressOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_progressOverlay, 0, 0);
    lv_obj_set_style_pad_all(s_progressOverlay, 8, 0);

    s_progressLabel = lv_label_create(s_progressOverlay);
    lv_label_set_text(s_progressLabel, "Caching...");
    lv_obj_set_style_text_color(s_progressLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(s_progressLabel, LV_ALIGN_TOP_MID, 0, 5);

    s_progressBar = lv_bar_create(s_progressOverlay);
    lv_obj_set_size(s_progressBar, lv_obj_get_width(canvas) - 20, 15);
    lv_obj_center(s_progressBar);
    lv_bar_set_range(s_progressBar, 0, 100);
    lv_bar_set_value(s_progressBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progressBar, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progressBar, lv_color_hex(0x00AA00), LV_PART_INDICATOR);

    s_progressTimer = lv_timer_create([](lv_timer_t*) {
        waveform_ui_update_progress(waveformsCached, totalWaveformsToCache);
        if (waveformsCached >= totalWaveformsToCache && totalWaveformsToCache > 0) {
            waveform_ui_destroy_progress();
        }
    }, 100, nullptr);
}

void waveform_ui_update_progress(int current, int total) {
    if (!s_progressBar || !s_progressLabel) return;
    int pct = (total > 0) ? (current * 100 / total) : 0;
    lv_bar_set_value(s_progressBar, pct, LV_ANIM_OFF);
    static char buf[32];
    snprintf(buf, sizeof(buf), "Caching %d/%d", current, total);
    lv_label_set_text(s_progressLabel, buf);
}

void waveform_ui_destroy_progress() {
    if (s_progressOverlay) {
        lv_obj_delete_async(s_progressOverlay);
        s_progressOverlay = nullptr;
        s_progressBar = nullptr;
        s_progressLabel = nullptr;
    }
    if (s_progressTimer) {
        lv_timer_delete(s_progressTimer);
        s_progressTimer = nullptr;
    }
    totalWaveformsToCache = 0;
    waveformsCached = 0;

    // Save cache to disk for instant loading next time
    auto* mgr = crosspad::getPlatformServices().kitManager;
    auto* kit = mgr ? mgr->getCurrentKit() : nullptr;
    if (kit && !kit->path.empty()) {
        waveform_cache_save_to_disk(kit->path.c_str());
    }
}

} // namespace crosspad_sampler
