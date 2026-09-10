#ifdef USE_LVGL


#include <lvgl.h>
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/ui/sampler_panel.hpp"
#include "crosspad-sampler/ui/waveform_view.hpp"
#include "crosspad-sampler/ui/pad_editor.hpp"
#include "crosspad-sampler/ui/pitched_editor.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/logic/kit_load_task.hpp"
#include "crosspad-sampler/waveform/waveform_loader.hpp"

// crosspad-gui
#include "crosspad-gui/crosspad_gui.h"   // styles, widgets, App class
#include "crosspad-gui/components/app_orchestrator.h"
#include "crosspad-gui/components/scope_view.h"

// crosspad-core
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/pad/PadLedController.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/platform/IClock.hpp>
#include <crosspad/settings/CrosspadSettings.hpp>
#include <crosspad/status/CrosspadStatus.hpp>
#include <crosspad/led/RgbColor.hpp>
#include <crosspad/led/HtmlColor.hpp>
#include <crosspad/instrument/PitchedInstrument.hpp>

#include <string>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>

using crosspad::IPadLogicHandler;
using crosspad::PadManager;
using crosspad::RgbColor;
using crosspad::HtmlColor;

namespace crosspad_sampler {

// ── Portable millis() replacement ───────────────────────────────────────
static uint32_t portable_millis() {
    auto* clk = crosspad::getPlatformServices().clock;
    if (clk) return static_cast<uint32_t>(clk->getTimeUs() / 1000);
    return lv_tick_get();
}

lv_subject_t s_pad_sampler; // subject for the last pressed pad (non-static: shared with pad_editor)
static lv_subject_t s_kit_name; // subject for the kit name label
static lv_subject_t s_audio_idx; // subject for the audio index
static lv_subject_t s_audio_name; // subject for the audio name label
static lv_subject_t s_audio_time; // subject for the audio time label
static lv_subject_t s_pWaveform; // subject for the waveform display pointer

static lv_obj_t * buttonFrame = NULL; // frame for the sampler buttons
static lv_obj_t * pickKitBtn = NULL; // pick kit button
static lv_obj_t * waveformCanvas = NULL; // waveform display canvas
/* The provider found nothing in the cache for the pad on screen and asked the
 * loader for it; the pad timer redraws once it lands instead of leaving the
 * box empty until the next pad change. */
static bool waveformPending = false;
static lv_obj_t * waveformPlayhead = NULL; // overlay line showing playback position
static lv_obj_t * waveformStartMarker = NULL; // green marker for start position
static lv_obj_t * waveformEndMarker = NULL; // red marker for end position
static lv_obj_t * waveformDimLeft = NULL; // dimming overlay before start
static lv_obj_t * waveformDimRight = NULL; // dimming overlay after end
static lv_obj_t * waveformFileLabel = NULL; // sample filename under the waveform
static lv_obj_t * waveformTimeLabel = NULL; // playback time under the waveform
static lv_obj_t * scopeView = NULL;
static lv_obj_t * waveformModeLabel = NULL;

enum class WaveformDisplayMode : uint8_t { File, Live, HistoryRms };
static WaveformDisplayMode waveformDisplayMode = WaveformDisplayMode::File;
/* -2 until first sync, -1 without a source, 0 for the 16-pad sampler and 1
 * for the pitched engine. A source change also resets the RMS history. */
static int scopeSourceKind = -2;

class PadEditorLogicHandler : public IPadLogicHandler {
public:

    void onActivate(PadManager& padManager) override {
        LV_LOG_INFO("Pad editor logic activated");
    }
    void onDeactivate(PadManager& padManager) override {
        LV_LOG_INFO("Pad editor logic deactivated");
    }
    void onPadPress(PadManager& padManager, uint8_t padIdx, uint8_t velocity) override {
        crosspad::KitInfo * kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (kit == NULL) return;
        crosspad::Pad * pad = &kit->pads[padIdx];
        if (pad->sampleCount == 0) return;

        // The same path SamplerPadLogic::onPadPress takes; the engine's own
        // per-pad loop config decides one-shot vs loop, not the caller.
        crosspad::getEventBus().postSamplePlayback(
            padIdx, /*wavIdx=*/0, /*isOn=*/true, crosspad::EventSource::App, velocity);
    }

    void onPadRelease(PadManager& padManager, uint8_t padIdx) override {
        crosspad::KitInfo * kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (!kit) return;

        // Hold mode: cut playback on release, same as the main sampler window.
        if (kit->parsed && kit->pads[padIdx].playbackMode == 1) {
            crosspad::getEventBus().postSamplePlayback(
                padIdx, /*wavIdx=*/0, /*isOn=*/false, crosspad::EventSource::App);
        }
    }
    void onPadPressure(PadManager& padManager, uint8_t padIdx, uint8_t pressure) override {}
};

static std::shared_ptr<PadEditorLogicHandler> g_samplerPadEditorLogic; // shared pointer to the pad editor logic handler

static lv_obj_t * g_parentContainer = NULL; // parent container for overlays
static bool g_prevNoteOffOnRelease = false; // store previous global setting to restore after destroy

// Button child index (row-major from top-left) -> displayed label number (1-16).
// Physical pad p lives on child notes[p]-1, whose label is p+1 — so the label
// is the pad number, and the button for pad p is child notes[p]-1.
static const int notes[] = {13, 14, 15, 16, 9, 10, 11, 12, 5, 6, 7, 8, 1, 2, 3, 4};
#define NOTES_MAX_QUEUE_LEN 100


static lv_timer_t * timerPad = NULL;
static lv_group_t * g_sampler = NULL; // group for the sampler buttons
static void (*s_on_back)() = nullptr; // back button callback (calls app->destroyApp())

// One callback for press, release and click, switched on the playback mode.
/* The gate the on-screen pads hold a note for. They sound on the click, not on
 * the touch-down, so there is no finger still on the pad to end the note — a
 * length has to be chosen, and this is long enough to hear a sample start and
 * short enough not to stack up under fast tapping. */
static constexpr uint32_t kScreenPadGateMs = 150;

static void screen_pad_gate_cb(lv_timer_t * t) {
    const int padIdx = (int)(intptr_t)lv_timer_get_user_data(t);
    crosspad::getPadManager().handlePadRelease(padIdx);
    lv_timer_delete(t);
}

/* Click, not press. A press-triggered pad made every screen-wide swipe that
 * crossed the pad row fire a note before it could become a swipe, which is
 * what made gestures on this panel awkward to land. LVGL sends CLICKED only
 * when the press was not lost, and the gesture layer cancels the press with
 * lv_indev_wait_release() — so a drag across the pads is silent, and a tap
 * still plays. The 16 physical pads are untouched by any of this. */
static void Sampler_pad_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = (lv_obj_t * )lv_event_get_target(e);
    if (!obj) return;
    lv_obj_t * label = lv_obj_get_child(obj, 0);
    if (!label) return;
    int displayIdx = atoi(lv_label_get_text(label)); // shown label number
    if (displayIdx < 1 || displayIdx > 16) return;
    int padIdx = displayIdx - 1;

    lv_subject_set_int(&s_pad_sampler, padIdx + 1);

    if (code != LV_EVENT_CLICKED) return;

    crosspad::getPadManager().handlePadPress(padIdx, 100);
    lv_timer_t * gate = lv_timer_create(screen_pad_gate_cb, kScreenPadGateMs,
                                        (void*)(intptr_t)padIdx);
    lv_timer_set_repeat_count(gate, 1);
}



static uint8_t lastPadPressed = 255; // Track last pad for timing
static long startPlayTime = 0; // start time of the sample playback
static uint8_t padIdxTime = 0; // pad index for the time display
static float lastPlayheadProgress = 0.0f; // Track playhead to detect restart

static volatile bool appIsDestroying = false; // Flag to indicate app destruction
static volatile bool samplerAppActive = false; // Track if sampler app is active

static bool lastPadStatesPlaying[16] = {false};

static uint8_t currentSampleIndex[16] = {0};
static lv_obj_t * padModeIndicators[16] = {nullptr};
static uint8_t lastPlaybackMode[16];
static bool padModeIndicatorsInitialized = false;

static SampleInfo currentSampleInfo = {};
static bool sampleInfoValid = false;
static char infoPath[128] = {0};

/* Header info per sample file, fetched once: reading it is an fopen and a
 * header parse on the LVGL thread under the file guard, and every such read
 * makes audio_bg stand aside from its refill. Per hit, that doubled the
 * engine's underruns. Cleared with the kit. */
struct SampleInfoEntry { std::string path; SampleInfo info; };
static std::vector<SampleInfoEntry> s_sampleInfoCache;

static bool cached_sample_info(const char* path, SampleInfo* out) {
    for (const auto& e : s_sampleInfoCache) {
        if (e.path == path) { *out = e.info; return true; }
    }
    auto& cb = sampler_get_platform_callbacks();
    if (!cb.player.getSampleInfo || !cb.player.getSampleInfo(path, out)) return false;
    s_sampleInfoCache.push_back(SampleInfoEntry{path, *out});
    return true;
}

// ── Engine callbacks, from the audio engine's NoteOn/NoteOff ───────────────
// GUI state only; the PadManager and EventBus side is platform-side, in
// SamplerPlatformCallbacks.cpp.

void sampler_on_engine_note_on(uint8_t note, uint8_t wavIdx)
{
    LV_LOG_INFO("SSP on cb: %u-%u", note, wavIdx);
    if (note < 16) {
        currentSampleIndex[note] = wavIdx;

        // Reset on every note-on, so the timing is per hit.
        padIdxTime = note;
        startPlayTime = portable_millis();
        lastPlayheadProgress = 0.0f;

        sampleInfoValid = false;
        infoPath[0] = '\0';

        LV_LOG_INFO("Timer reset: pad=%u, wavIdx=%u, time=%lu", note, wavIdx, (unsigned long)startPlayTime);
    }
}

void sampler_on_engine_note_off(uint8_t note, uint8_t wavIdx)
{
    LV_LOG_INFO("SSP off cb: %u-%u", note, wavIdx);
    (void)note;
    (void)wavIdx;
}

static lv_obj_t * g_saveConfirmMsgbox = NULL;

static bool HasDirtyPads(std::vector<uint8_t>& dirtyPadNumbers) {
    dirtyPadNumbers.clear();
    crosspad::KitInfo * kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (kit == NULL) return false;

    for (uint8_t i = 0; i < KIT_PADS; i++) {
        if (kit->pads[i].dirtyModified) {
            dirtyPadNumbers.push_back(i + 1); // Store 1-based pad numbers for display
        }
    }
    // A pitched kit's edits are the instrument's, not any pad's; without this
    // the leave-app prompt never appeared for them.
    return !dirtyPadNumbers.empty() || kit->instrument.dirty;
}

static std::string FormatDirtyPadsList(const std::vector<uint8_t>& dirtyPadNumbers) {
    std::string result = "Modified pads: ";
    for (size_t i = 0; i < dirtyPadNumbers.size(); i++) {
        result += std::to_string(dirtyPadNumbers[i]);
        if (i < dirtyPadNumbers.size() - 1) {
            result += ", ";
        }
    }
    return result;
}

// Deferred, so LVGL objects are not destroyed during event processing.
static void close_sampler_async(void*) {
    if (s_on_back) {
        s_on_back();
    } else {
        auto& orch = crosspad_gui::AppOrchestrator::getInstance();
        auto* running = orch.getRunningApp();
        if (running && running->isStarted()) {
            running->destroyApp();
            orch.setRunningApp(nullptr);
        }
    }
}

static void ShowSaveConfirmationDialog(lv_obj_t * panel) {
    std::vector<uint8_t> dirtyPadNumbers;
    if (!HasDirtyPads(dirtyPadNumbers)) {
        lv_async_call(close_sampler_async, nullptr);
        return;
    }

    if (g_saveConfirmMsgbox != NULL) {
        lv_msgbox_close(g_saveConfirmMsgbox);
        g_saveConfirmMsgbox = NULL;
    }

    g_saveConfirmMsgbox = lv_msgbox_create(lv_layer_top());
    lv_obj_t * closeBtn = lv_msgbox_add_close_button(g_saveConfirmMsgbox);

    lv_msgbox_add_title(g_saveConfirmMsgbox, "Unsaved Changes");

    std::string message = "Do you want to save changes to the kit?\n\n" + FormatDirtyPadsList(dirtyPadNumbers);
    lv_msgbox_add_text(g_saveConfirmMsgbox, message.c_str());

    lv_obj_t * saveBtn = lv_msgbox_add_footer_button(g_saveConfirmMsgbox, "Save");
    lv_obj_t * discardBtn = lv_msgbox_add_footer_button(g_saveConfirmMsgbox, "Discard");
    lv_obj_t * cancelBtn = lv_msgbox_add_footer_button(g_saveConfirmMsgbox, "Cancel");

    lv_obj_add_style(saveBtn, &styleButton, 0);
    lv_obj_add_style(discardBtn, &styleButton, 0);
    lv_obj_add_style(cancelBtn, &styleButton, 0);

    lv_obj_add_event_cb(saveBtn, [](lv_event_t * e) {
        LV_LOG_INFO("Saving kit before closing...");
        auto* kitMgr = crosspad::getKitManager();
        if (kitMgr && kitMgr->getCurrentKit()) {
            kitMgr->saveKit(*kitMgr->getCurrentKit());
        }

        crosspad::KitInfo * kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (kit != NULL) {
            kit->instrument.dirty = false;
            for (uint8_t i = 0; i < KIT_PADS; i++) {
                kit->pads[i].dirty = false;
                kit->pads[i].dirtyModified = false;
            }
        }

        if (g_saveConfirmMsgbox) {
            lv_msgbox_close(g_saveConfirmMsgbox);
            g_saveConfirmMsgbox = NULL;
        }
        lv_async_call(close_sampler_async, nullptr);
    }, LV_EVENT_CLICKED, NULL);

    // Discard: reload the kit from storage.
    lv_obj_add_event_cb(discardBtn, [](lv_event_t * e) {
        LV_LOG_INFO("Discarding changes - reloading kit...");

        crosspad::KitInfo * kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (kit != NULL) {
                kit->instrument.dirty = false;
                for (uint8_t i = 0; i < KIT_PADS; i++) {
                kit->pads[i].dirty = false;
                kit->pads[i].dirtyModified = false;
            }

            auto* kitMgr2 = crosspad::getKitManager();
            if (kitMgr2 && kit) kitMgr2->loadKit(*kit);
        }

        if (g_saveConfirmMsgbox) {
            lv_msgbox_close(g_saveConfirmMsgbox);
            g_saveConfirmMsgbox = NULL;
        }
        lv_async_call(close_sampler_async, nullptr);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(cancelBtn, [](lv_event_t * e) {
        LV_LOG_INFO("Cancel close operation");
        if (g_saveConfirmMsgbox) {
            lv_msgbox_close(g_saveConfirmMsgbox);
            g_saveConfirmMsgbox = NULL;
        }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(closeBtn, [](lv_event_t * e) {
        LV_LOG_INFO("Cancel close operation (close button)");
        if (g_saveConfirmMsgbox) {
            lv_msgbox_close(g_saveConfirmMsgbox);
            g_saveConfirmMsgbox = NULL;
        }
    }, LV_EVENT_CLICKED, NULL);
}

static void PickKit_button_cb(lv_event_t * e) {
    (void)e;
    const auto& cb = sampler_get_platform_callbacks().kitSelector;
    if (cb.openSelector) cb.openSelector();
}

static uint32_t scope_pull_engine(void* ctx, float* out, uint32_t n,
                                  float* hz, uint32_t* sampleRate) {
    auto* inst = static_cast<crosspad::PitchedInstrument*>(ctx);
    *hz         = inst->lastNoteHz();
    *sampleRate = inst->scopeTap().sampleRate();
    return inst->scopeTap().snapshot(out, n);
}

static uint32_t scope_pull_sampler(void*, float* out, uint32_t n,
                                   float* hz, uint32_t* sampleRate) {
    const auto& cb = sampler_get_platform_callbacks().player;
    if (!cb.getLiveScopeData) return 0;
    return cb.getLiveScopeData(out, n, hz, sampleRate);
}

static void set_live_scope_capture(bool enabled) {
    if (scopeSourceKind == 1) {
        auto* inst = crosspad::getPlatformServices().pitchedInstrument;
        if (!inst) return;
        auto& tap = inst->scopeTap();
        if (enabled && !tap.enabled()) {
            tap.clear();
            tap.setEnabled(true);
        } else if (!enabled) {
            tap.setEnabled(false);
        }
    } else if (scopeSourceKind == 0) {
        const auto fn = sampler_get_platform_callbacks().player.setLiveScopeEnabled;
        if (fn) fn(enabled);
    }
}

static void apply_waveform_display_mode() {
    if (!scopeView || !waveformCanvas) return;

    const bool showScope = waveformDisplayMode != WaveformDisplayMode::File
                        && scopeSourceKind >= 0;
    scope_view_set_active(scopeView, false);
    if (showScope) {
        scope_view_set_mode(scopeView,
            waveformDisplayMode == WaveformDisplayMode::HistoryRms
                ? crosspad_gui::ScopeViewMode::HistoryRms
                : crosspad_gui::ScopeViewMode::Live);
        set_live_scope_capture(true);
        lv_obj_clear_flag(scopeView, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(waveformCanvas, LV_OBJ_FLAG_HIDDEN);
        if (waveformFileLabel) lv_obj_add_flag(waveformFileLabel, LV_OBJ_FLAG_HIDDEN);
        if (waveformTimeLabel) lv_obj_add_flag(waveformTimeLabel, LV_OBJ_FLAG_HIDDEN);
        scope_view_set_active(scopeView, true);
    } else {
        set_live_scope_capture(false);
        lv_obj_add_flag(scopeView, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(waveformCanvas, LV_OBJ_FLAG_HIDDEN);
        if (waveformFileLabel) lv_obj_clear_flag(waveformFileLabel, LV_OBJ_FLAG_HIDDEN);
        if (waveformTimeLabel) lv_obj_clear_flag(waveformTimeLabel, LV_OBJ_FLAG_HIDDEN);
    }

    if (waveformModeLabel) {
        if (showScope) {
            lv_obj_add_flag(waveformModeLabel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text_static(waveformModeLabel, "FILE");
            lv_obj_clear_flag(waveformModeLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(waveformModeLabel);
        }
    }
}

static void waveform_mode_click_cb(lv_event_t*) {
    if (scopeSourceKind < 0) {
        waveformDisplayMode = WaveformDisplayMode::File;
    } else if (waveformDisplayMode == WaveformDisplayMode::File) {
        waveformDisplayMode = WaveformDisplayMode::Live;
    } else if (waveformDisplayMode == WaveformDisplayMode::Live) {
        waveformDisplayMode = WaveformDisplayMode::HistoryRms;
    } else {
        waveformDisplayMode = WaveformDisplayMode::File;
    }
    apply_waveform_display_mode();
}

/// Select the appropriate live source when the kit's engine changes. The
/// file/live/RMS choice itself is controlled by clicking the waveform.
static void sync_engine_view() {
    if (!scopeView || !waveformCanvas) return;

    auto* mgr  = crosspad::getKitManager();
    auto* kit  = mgr ? mgr->getCurrentKit() : nullptr;
    auto* inst = crosspad::getPlatformServices().pitchedInstrument;
    const bool pitched = kit && kit->engine == crosspad::KitEngine::Pitched && inst;
    const bool sampler = sampler_get_platform_callbacks().player.getLiveScopeData != nullptr;
    const int sourceKind = pitched ? 1 : (sampler ? 0 : -1);
    if (sourceKind == scopeSourceKind) return;
    scope_view_set_active(scopeView, false);
    set_live_scope_capture(false);
    scopeSourceKind = sourceKind;

    if (sourceKind == 1) {
        scope_view_set_source(scopeView, scope_pull_engine, inst);
        waveformDisplayMode = WaveformDisplayMode::Live;
    } else if (sourceKind == 0) {
        scope_view_set_source(scopeView, scope_pull_sampler, nullptr);
        waveformDisplayMode = WaveformDisplayMode::File;
    } else {
        scope_view_clear_source(scopeView);
        waveformDisplayMode = WaveformDisplayMode::File;
    }
    apply_waveform_display_mode();
}

// Timer-based pad state monitoring.

static void check_pad_timer_cb(lv_timer_t * timer) {
    // Skip everything while the app is being destroyed: the LVGL objects and
    // styles are already freed.
    if (appIsDestroying) return;

    sync_engine_view();

    static long lastTime = 0;
    static char time_str[24]; // Increased buffer for "M:SS:T / M:SS:T" format
    static crosspad::KitInfo * lastKit = NULL; // last kit

    for (uint8_t pad = 0; pad < 16; pad++) {
        bool currentlyPlaying = crosspad::getPadManager().isPadPlaying(pad);
        bool currentlyPressed = crosspad::getPadManager().isPadPressed(pad);
        bool currentlyActive = currentlyPlaying || currentlyPressed;

        // On every press, even a repeat, so the pad editor keeps up.
        if (currentlyPressed) {
            lv_subject_set_int(&s_pad_sampler, (pad + 1));
            lastPadPressed = pad;
            padIdxTime = pad;
        }

        if (currentlyActive != lastPadStatesPlaying[pad]) {
            lastPadStatesPlaying[pad] = currentlyActive;

            if (currentlyActive) {
                if (buttonFrame != nullptr && pad < sizeof(notes)/sizeof(notes[0])) {
                    int btnIndex = notes[pad] - 1; // map physical pad -> button child index
                    lv_obj_t * btn = lv_obj_get_child(buttonFrame, btnIndex);
                    if (btn) {
                        RgbColor padColor = crosspad::getPadManager().getPressedColor(pad);
                        lv_obj_set_style_bg_color(btn, lv_color_make(padColor.R, padColor.G, padColor.B), 0);
                    }
                }
            } else {
                if (buttonFrame != nullptr && pad < sizeof(notes)/sizeof(notes[0])) {
                    int btnIndex = notes[pad] - 1;
                    lv_obj_t * btn = lv_obj_get_child(buttonFrame, btnIndex);
                    if (btn) {
                        RgbColor padColor = crosspad::getPadManager().getPadColor(pad);
                        lv_obj_set_style_bg_color(btn, lv_color_make(padColor.R, padColor.G, padColor.B), 0);
                    }
                }
            }
        }
    }

    crosspad::KitInfo * kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (kit == NULL) {
        if (lastKit != NULL) {
            LV_LOG_INFO("Kit became NULL");
        }
        return;
    }
    if (lastKit != kit) { // kit changed
        s_sampleInfoCache.clear();
        LV_LOG_INFO("Kit changed: %s (previous: %s)", kit->name.c_str(),
                    lastKit ? lastKit->name.c_str() : "NULL");
        lv_subject_copy_string(&s_kit_name, kit->name.c_str());

        for (uint8_t n = 0; n < KIT_PADS; n++)
        {
            auto color = RgbColor(HtmlColor(kit->pads[n].color));
            auto color_pressed = RgbColor(HtmlColor(kit->pads[n].color_pressed));
            crosspad::getPadLedController().setPadColor(n, color);
            crosspad::getPadLedController().setPadPressedColor(n, color_pressed);
            int btnIndex = notes[n] - 1;
            lv_obj_t * btn = lv_obj_get_child(buttonFrame, btnIndex);
            if(btn) {
                lv_obj_set_style_bg_color(btn,
                                          lv_color_make(color.R, color.G, color.B), 0);
            }
        }
        auto* statusPtr = crosspad::getPlatformServices().status;
        if (statusPtr) statusPtr->keypadDirty = true; // force keypad refresh
        // PadManager needs the kit pads for the playback modes to work.
        crosspad::getPadManager().loadPadConfig(kit->pads, KIT_PADS);
    }
    lastKit = kit;
    if (kit) {
        for (uint8_t n = 0; n < 16; ++n) {
            uint8_t mode = kit->pads[n].playbackMode; // 0=one shot, 1=hold, 2=loop
            if (mode != lastPlaybackMode[n] && padModeIndicators[n]) {
                switch (mode) {
                    case 0: // One Shot (default, no indicator)
                        lv_label_set_text(padModeIndicators[n], "");
                        break;
                    case 1: // Hold
                        lv_label_set_text(padModeIndicators[n], "H");
                        lv_obj_set_style_text_color(padModeIndicators[n], lv_color_hex(0x2020FF), 0);
                        break;
                    case 2: // Loop
                        lv_label_set_text(padModeIndicators[n], LV_SYMBOL_REFRESH);
                        lv_obj_set_style_text_color(padModeIndicators[n], lv_color_hex(0x20FF20), 0);
                        break;
                    default:
                        lv_label_set_text(padModeIndicators[n], "");
                        break;
                }
                lastPlaybackMode[n] = mode;
            }
        }
    }
    long now = portable_millis();

    if (now - lastTime > 100 && padIdxTime < 16 && crosspad::getPadManager().isPadPlaying(padIdxTime)) {
        lastTime = now;

        int elapsedMs = now - startPlayTime;
        int tenthsSeconds = (elapsedMs / 100) % 10;
        int seconds = (elapsedMs / 1000) % 60;
        int minutes = (elapsedMs / 1000) / 60;

        char totalTimeStr[24] = "";
        if (sampleInfoValid && currentSampleInfo.sampleRate > 0) {
            crosspad::KitInfo* kitP = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
            uint32_t startS = 0, endS = 0;
            if (kitP && (padIdxTime < KIT_PADS) && !kitP->pads[padIdxTime].samples.empty()) {
                uint8_t sIdx = currentSampleIndex[padIdxTime];
                if (sIdx >= kitP->pads[padIdxTime].samples.size()) sIdx = 0;
                startS = kitP->pads[padIdxTime].samples[sIdx].start;
                endS = kitP->pads[padIdxTime].samples[sIdx].end;
            }
            uint32_t totalSamples = 0;
            if (endS > startS) totalSamples = endS - startS;
            else totalSamples = currentSampleInfo.sampleCount; // fallback: whole file

            if (totalSamples > 0) {
                float totalMs = (float)totalSamples * 1000.0f / (float)currentSampleInfo.sampleRate;
                int totalTenths = ((int)totalMs / 100) % 10;
                int totalSec = ((int)totalMs / 1000) % 60;
                int totalMin = ((int)totalMs / 1000) / 60;
                snprintf(totalTimeStr, sizeof(totalTimeStr), " / %d:%02d:%01d", totalMin, totalSec, totalTenths);
            }
        }

        snprintf(time_str, sizeof(time_str), "%d:%02d:%01d%s", minutes, seconds, tenthsSeconds, totalTimeStr);
        LV_LOG_INFO("Time: %s", time_str);
        lv_subject_copy_string(&s_audio_time, time_str);
    }
    else if (now - lastTime > 2000) {
        // Reset the label after 2 s with no update.
        lv_subject_copy_string(&s_audio_time, "0:00:0");
    }

    /* The status bar's loading ring, the one the kit loader drives, says how
     * far the waveforms are too — otherwise the only way to know a pad had
     * arrived was to press it. The kit load owns the ring while it runs. */
    if (!kit_load_task_is_loading()) {
        static int lastRing = -2;
        const int ring = waveform_loader_progress();
        if (ring != lastRing) {
            lastRing = ring;
            crosspad_gui::statusbar_set_loading(ring);
        }
    }

    uint8_t pad = lv_subject_get_int(&s_pad_sampler);
    static uint8_t lastDisplayedPad = 0; // Track which pad was last displayed
    static uint8_t lastDisplayedSampleIdx = 255; // Track last sample index displayed

    // A waveform the loader was asked for: redraw as soon as it is cached.
    // The cache generation, not a shared flag — the pad editor draws from the
    // same provider, and whichever view ticked first used to eat the flag and
    // leave the other one showing an empty box for good.
    static uint32_t lastDrawnWaveformGen = 0;
    const uint32_t waveformGen = waveform_cache_generation();
    if (waveformPending && waveformGen != lastDrawnWaveformGen
        && waveformCanvas && lastDisplayedPad > 0 && lastDisplayedSampleIdx != 255) {
        lastDrawnWaveformGen = waveformGen;
        waveformPending = false;   // the provider sets it again on a miss
        waveform_view_load(waveformCanvas, lastDisplayedPad - 1, lastDisplayedSampleIdx);
    }

    if (pad > 0) {
        crosspad::KitInfo* kit2 = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (kit2 != nullptr && (pad - 1) < KIT_PADS && !kit2->pads[pad - 1].samples.empty()) {
            uint8_t sampleIdx = currentSampleIndex[pad - 1];
            if (sampleIdx >= kit2->pads[pad - 1].samples.size()) {
                sampleIdx = 0; // Fall back to first sample if index is out of range
            }

            if (pad != lastDisplayedPad || sampleIdx != lastDisplayedSampleIdx) {
                lastDisplayedPad = pad;
                lastDisplayedSampleIdx = sampleIdx;

                lv_subject_set_int(&s_audio_idx, sampleIdx + 1); // Display is 1-based

                std::string samplePath = kit2->pads[pad - 1].samples[sampleIdx].sample;
                if (samplePath.size() > 9 && samplePath.substr(0, 9) == "/crosspad") {
                    size_t lastSlash = samplePath.rfind('/');
                    if (lastSlash != std::string::npos) {
                        samplePath = "../" + samplePath.substr(lastSlash + 1);
                    }
                }
                lv_subject_copy_string(&s_audio_name, samplePath.c_str());

                if (waveformCanvas) {
                    waveform_view_load(waveformCanvas, pad - 1, sampleIdx);

                    if (sampleInfoValid && currentSampleInfo.sampleCount > 0) {
                        uint32_t startS = kit2->pads[pad - 1].samples[sampleIdx].start;
                        uint32_t endS = kit2->pads[pad - 1].samples[sampleIdx].end;
                        uint32_t totalSamples = currentSampleInfo.sampleCount;

                        if (endS == 0 || endS > totalSamples) endS = totalSamples;

                        float startNorm = totalSamples > 0 ? (float)startS / (float)totalSamples : 0.0f;
                        float endNorm = totalSamples > 0 ? (float)endS / (float)totalSamples : 1.0f;

                        waveform_view_set_markers(waveformCanvas, startNorm, endNorm);
                    }
                }
            }
        } else if (pad != lastDisplayedPad) {
            lastDisplayedPad = pad;
            lastDisplayedSampleIdx = 255; // Reset sample index
            lv_subject_set_int(&s_audio_idx, 0);
            lv_subject_copy_string(&s_audio_name, "No sample");

            if (waveformCanvas) {
                waveform_view_clear(waveformCanvas);
            }
        }
    } else if (pad == 0 && lastDisplayedPad != 0) {
        lastDisplayedPad = 0;
        lastDisplayedSampleIdx = 255; // Reset sample index tracking
        lv_subject_set_int(&s_audio_idx, 0);
        lv_subject_copy_string(&s_audio_name, "Press any pad");

        if (waveformCanvas) {
            waveform_view_clear(waveformCanvas);
        }
    }

    if (waveformCanvas) {
        if (padIdxTime < 16 && crosspad::getPadManager().isPadPlaying(padIdxTime)) {
            if (infoPath[0] == '\0' || (lastDisplayedPad > 0)) {
                crosspad::KitInfo* kitP2 = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
                if (kitP2 && padIdxTime < KIT_PADS && !kitP2->pads[padIdxTime].samples.empty()) {
                    uint8_t sIdx = currentSampleIndex[padIdxTime];
                    if (sIdx >= kitP2->pads[padIdxTime].samples.size()) sIdx = 0;
                    std::string resolvedP = crosspad::resolveKitSamplePath(kitP2->path, kitP2->pads[padIdxTime].samples[sIdx].sample);
                    const char* fullPath = resolvedP.empty() ? nullptr : resolvedP.c_str();
                    if (fullPath && (infoPath[0] == '\0' || strcmp(infoPath, fullPath) != 0)) {
                        sampleInfoValid = cached_sample_info(fullPath, &currentSampleInfo);
                        if (sampleInfoValid) {
                            strncpy(infoPath, fullPath, sizeof(infoPath) - 1);
                            infoPath[sizeof(infoPath) - 1] = '\0';
                        } else {
                            infoPath[0] = '\0';
                        }
                    }
                }
            }

            float progress = 0.0f;
            if (sampleInfoValid && currentSampleInfo.sampleRate > 0) {
                crosspad::KitInfo* kitP = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
                uint32_t startS = 0, endS = 0;
                if (kitP && (padIdxTime < KIT_PADS) && !kitP->pads[padIdxTime].samples.empty()) {
                    uint8_t sIdx = currentSampleIndex[padIdxTime];
                    if (sIdx >= kitP->pads[padIdxTime].samples.size()) sIdx = 0;
                    startS = kitP->pads[padIdxTime].samples[sIdx].start;
                    endS = kitP->pads[padIdxTime].samples[sIdx].end;
                }
                uint32_t totalSamples = 0;
                if (endS > startS) totalSamples = endS - startS;
                else totalSamples = currentSampleInfo.sampleCount; // fallback: whole file

                if (totalSamples > 0) {
                    float totalMs = (float)totalSamples * 1000.0f / (float)currentSampleInfo.sampleRate;
                    int elapsedMs = now - startPlayTime;
                    if (elapsedMs < 0) elapsedMs = 0;
                    progress = totalMs > 1.0f ? (float)elapsedMs / totalMs : 0.0f;
                    if (progress < 0.0f) progress = 0.0f;
                    if (progress > 1.0f) progress = 1.0f;

                    lastPlayheadProgress = progress;
                }
            }

            waveform_view_set_playhead(waveformCanvas, progress);
        } else {
            waveform_view_set_playhead(waveformCanvas, -1.0f); // hide playhead
        }
    }
}

// ── Shared logic objects (must precede sampler_panel_create) ───────────
static SamplerPadLogic g_samplerPadLogic;
static PadColorManager g_padColorManager;
static KitOrchestrator g_kitOrchestrator;

lv_obj_t * sampler_panel_create(lv_obj_t *parent, void (*onBack)())
{
    s_on_back = onBack;
    g_parentContainer = NULL;
    appIsDestroying = false;

    // Waveform data provider, reading the cache the loader task fills.
    waveform_view_set_provider([](uint8_t padIdx, uint8_t sampleSlot,
                                  int16_t* outData, uint32_t maxSamples,
                                  uint32_t* actualSamples, void* /*userData*/) {
        *actualSamples = 0;

        auto* mgr = crosspad::getKitManager();
        auto* kit = mgr ? mgr->getCurrentKit() : nullptr;
        if (!kit || padIdx >= KIT_PADS) return WaveformStatus::NoSample;
        if (sampleSlot >= kit->pads[padIdx].samples.size()) return WaveformStatus::NoSample;

        std::string resolvedSample = crosspad::resolveKitSamplePath(kit->path, kit->pads[padIdx].samples[sampleSlot].sample);
        if (resolvedSample.empty()) return WaveformStatus::NoSample;
        const char* fullPath = resolvedSample.c_str();

        // A cache miss is a request, not a blank.
        WaveformCacheEntry* entry = waveform_cache_find(fullPath);
        if (!entry || !entry->valid || !entry->waveformData || entry->sampleCount == 0) {
            waveform_loader_start();
            waveform_loader_request(fullPath, true, padIdx, 0, 0);
            waveformPending = true;
            return WaveformStatus::Pending;
        }

        uint32_t count = entry->sampleCount < maxSamples ? entry->sampleCount : maxSamples;
        if (entry->sampleCount <= maxSamples) {
            memcpy(outData, entry->waveformData, count * sizeof(int16_t));
        } else {
            for (uint32_t i = 0; i < maxSamples; i++) {
                uint32_t srcIdx = (uint64_t)i * entry->sampleCount / maxSamples;
                outData[i] = entry->waveformData[srcIdx];
            }
            count = maxSamples;
        }
        *actualSamples = count;
        return WaveformStatus::Ok;
    }, nullptr);
    g_sampler = lv_group_create(); // create the group for the sampler buttons
    lv_indev_t * navEncoder = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    if (navEncoder) lv_indev_set_group(navEncoder, g_sampler); // set the group for the encoder input device
    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_obj_get_content_width(parent), lv_obj_get_content_height(parent));
    lv_obj_center(cont);
    lv_obj_add_style(cont, &styleAppContainer, 0);
    g_parentContainer = cont;

    lv_obj_t * title = lv_label_create(cont);
    lv_obj_add_style(title, &styleTitle, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    const size_t kit_name_len = 32;
    static char kit_name[kit_name_len];
    static char kit_name_prev[kit_name_len];
    lv_subject_init_string(&s_kit_name, kit_name, kit_name_prev, kit_name_len, "Kit name");
    lv_label_bind_text(title, &s_kit_name, NULL);


    lv_obj_t * backBtn = lv_button_create(cont);
    lv_obj_align(backBtn, LV_ALIGN_BOTTOM_MID, -50, -8);
    lv_obj_add_event_cb(backBtn, [](lv_event_t * e) {
        LV_LOG_INFO("Exit button clicked");
        ShowSaveConfirmationDialog(g_parentContainer);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_style(backBtn, &styleButton, 0);
    lv_obj_add_style(backBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_style(backBtn, &styleButtonInactive, LV_STATE_DISABLED);
    lv_obj_t * backLabel = lv_label_create(backBtn);
    lv_label_set_text(backLabel, "Back");
    lv_obj_set_style_text_color(backLabel, lv_color_hex(0x000000), 0);
    lv_group_add_obj(g_sampler, backBtn); // add the back button to the group

    lv_obj_t * editBtn = lv_button_create(cont);
    lv_obj_align(editBtn, LV_ALIGN_BOTTOM_MID, 50, -8);
    lv_obj_add_event_cb(editBtn, [](lv_event_t * e) {
        LV_LOG_INFO("Opening pad editor");
        lv_obj_t * cont = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        if (!cont) return;
        crosspad::KitInfo * kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (kit && kit->engine == crosspad::KitEngine::Pitched) {
            pitched_editor_create(cont);
        } else {
            pad_editor_create(cont, nullptr);
        }
    }, LV_EVENT_CLICKED, cont);
    lv_obj_add_style(editBtn, &styleButton, 0);
    lv_obj_add_style(editBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_style(editBtn, &styleButtonInactive, LV_STATE_DISABLED);
    lv_obj_t * editLabel = lv_label_create(editBtn);
    lv_label_set_text(editLabel, "Edit");
    lv_obj_set_style_text_color(editLabel, lv_color_hex(0x000000), 0);
    lv_group_add_obj(g_sampler, editBtn); // add the edit button to the group
    g_samplerPadEditorLogic = std::make_shared<PadEditorLogicHandler>();
    crosspad::getPadManager().registerPadLogic("Pad editor", g_samplerPadEditorLogic);

    static auto s_samplerLogic = std::shared_ptr<SamplerPadLogic>(&g_samplerPadLogic, [](SamplerPadLogic*){});
    crosspad::getPadManager().registerPadLogic("Sampler", s_samplerLogic);
    crosspad::getPadManager().setActivePadLogic("Sampler");

    // NULL callbacks: our own events are attached below.
    buttonFrame = createButtons(cont, NULL);
    lv_obj_align(buttonFrame, LV_ALIGN_LEFT_MID, 15, -5);

    if (buttonFrame) {
        uint32_t cnt = lv_obj_get_child_cnt(buttonFrame);
        for (uint32_t i = 0; i < cnt; ++i) {
            lv_obj_t * btn = lv_obj_get_child(buttonFrame, i);
            if (!btn) continue;
            lv_obj_add_event_cb(btn, Sampler_pad_event_cb, LV_EVENT_CLICKED, NULL);
        }
        for (int pad = 0; pad < 16; ++pad) {
            int btnIndex = notes[pad] - 1; // button child index for this pad
            if (btnIndex >= 0 && btnIndex < (int)cnt) {
                lv_obj_t * btn = lv_obj_get_child(buttonFrame, btnIndex);
                if (btn && padModeIndicators[pad] == nullptr) {
                    padModeIndicators[pad] = lv_label_create(btn);
                    lv_label_set_text(padModeIndicators[pad], "");
                    lv_obj_set_style_text_font(padModeIndicators[pad], &lv_font_montserrat_10, 0);
                    lv_obj_set_style_text_color(padModeIndicators[pad], lv_color_hex(0x000000), 0);
                    lv_obj_align(padModeIndicators[pad], LV_ALIGN_TOP_LEFT, 1, 0);
                }
            }
        }
        if (!padModeIndicatorsInitialized) {
            for (int k = 0; k < 16; ++k) lastPlaybackMode[k] = 255; // force update
            padModeIndicatorsInitialized = true;
        }
    }


    lv_obj_t * padLabel = lv_label_create(cont);
    lv_obj_align(padLabel, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_obj_set_style_text_opa(padLabel, LV_OPA_70, 0);
    lv_subject_init_int(&s_pad_sampler, 0);
    lv_label_bind_text(padLabel, &s_pad_sampler, "PAD: %d");

    pickKitBtn = lv_button_create(cont);
    lv_obj_align(pickKitBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t * pickKitBtnIcon = lv_image_create(pickKitBtn);
    lv_image_set_src(pickKitBtnIcon, LV_SYMBOL_LIST);
    lv_obj_set_size(pickKitBtn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(pickKitBtnIcon);
    lv_obj_add_event_cb(pickKitBtn, PickKit_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_style(pickKitBtn, &styleButton, 0);
    lv_obj_add_style(pickKitBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_style(pickKitBtn, &stylePrimaryImgRecolor, 0);
    lv_obj_add_style(pickKitBtnIcon, &stylePrimaryImgRecolor, 0);
    lv_obj_set_style_pad_all(pickKitBtn, 1, 0);
    lv_obj_set_style_pad_all(pickKitBtnIcon, 1, 0);

    if (g_sampler) {
        lv_group_add_obj(g_sampler, pickKitBtn);
    }

    waveformCanvas = waveform_view_create(cont);
    lv_obj_align(waveformCanvas, LV_ALIGN_RIGHT_MID, -25, -10);
    lv_obj_add_event_cb(waveformCanvas, waveform_mode_click_cb, LV_EVENT_CLICKED, nullptr);

    scopeView = create_scope_view(cont, WAVEFORM_CANVAS_WIDTH, WAVEFORM_CANVAS_HEIGHT);
    if (scopeView) {
        lv_obj_align(scopeView, LV_ALIGN_RIGHT_MID, -25, -10);
        lv_obj_add_flag(scopeView, LV_OBJ_FLAG_HIDDEN);
        scope_view_set_active(scopeView, false);
        lv_obj_add_event_cb(scopeView, waveform_mode_click_cb, LV_EVENT_CLICKED, nullptr);
    }
    scopeSourceKind = -2;
    waveformDisplayMode = WaveformDisplayMode::File;

    waveformModeLabel = lv_label_create(cont);
    lv_label_set_text_static(waveformModeLabel, "FILE");
    lv_obj_set_style_text_font(waveformModeLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(waveformModeLabel, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_opa(waveformModeLabel, LV_OPA_70, 0);
    lv_obj_align_to(waveformModeLabel, waveformCanvas, LV_ALIGN_TOP_RIGHT, -2, 1);
    lv_obj_add_flag(waveformModeLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(waveformModeLabel, waveform_mode_click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * fileLabel = lv_label_create(cont);
    static char audio_name[32];
    static char audio_name_prev[32];
    lv_subject_init_int(&s_audio_idx, 0);
    lv_subject_init_string(&s_audio_name, audio_name, audio_name_prev, 32, "Press any pad");
    lv_label_bind_text(fileLabel, &s_audio_name, NULL);
    lv_obj_align_to(fileLabel, waveformCanvas, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);
    waveformFileLabel = fileLabel;

    lv_obj_t * timeLabel = lv_label_create(cont);
    static char audio_time[10];
    static char audio_time_prev[10];
    lv_subject_init_string(&s_audio_time, audio_time, audio_time_prev, 10, "0:00:0");
    lv_label_bind_text(timeLabel, &s_audio_time, NULL);
    lv_obj_align_to(timeLabel, waveformCanvas, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 0);
    waveformTimeLabel = timeLabel;

    timerPad = lv_timer_create(check_pad_timer_cb, 50, NULL); // 50ms = 20fps
    lv_timer_ready(timerPad);

    auto* settingsPtr = crosspad::getPlatformServices().settings;
    if (settingsPtr) {
        settingsPtr->keypad.lightsOnNoteOff = true; // turn on feature
        settingsPtr->keypad.lightsOnNoteOn = true; // turn on feature
        g_prevNoteOffOnRelease = settingsPtr->keypad.noteOffOnRelease;
        settingsPtr->keypad.noteOffOnRelease = false;
    }

    crosspad::KitInfo * kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (kit != NULL) {
        lv_subject_copy_string(&s_kit_name, kit->name.c_str());
    } else {
        lv_subject_copy_string(&s_kit_name, "No Kit");
    }

    lv_obj_t * vu = create_vu_meter(cont, 6, 85);
    lv_obj_align(vu, LV_ALIGN_RIGHT_MID, -4, -3);

    return cont;
}

void sampler_panel_destroy(lv_obj_t * panel)
{
    appIsDestroying = true;

    // "Sampler" stays registered on purpose — it is the system instrument
    // (sampler_service_init), so the pads keep playing after the app closes.
    // setActivePadLogic("") drops back to it instead of going silent.
    crosspad::getPadManager().setActivePadLogic("");
    crosspad::getPadManager().unregisterPadLogic("Pad editor");
    g_samplerPadEditorLogic.reset();

    lv_group_delete(g_sampler); // delete the group
    lv_subject_deinit(&s_pad_sampler);
    lv_subject_deinit(&s_kit_name);
    lv_subject_deinit(&s_audio_idx);
    lv_subject_deinit(&s_audio_name);
    lv_subject_deinit(&s_audio_time);
    lv_subject_deinit(&s_pWaveform);

    lv_timer_delete(timerPad);

    // These labels are children of the button frame about to be deleted;
    // left dangling, the timer callbacks write through freed memory.
    for (int i = 0; i < 16; ++i) {
        padModeIndicators[i] = nullptr;
        lastPlaybackMode[i] = 255;
    }
    padModeIndicatorsInitialized = false;

    // Other globals into UI objects, to survive the async deletion.
    buttonFrame = nullptr;
    pickKitBtn = nullptr;
    waveformCanvas = nullptr;
    waveformPlayhead = nullptr;
    waveformStartMarker = nullptr;
    waveformEndMarker = nullptr;
    waveformDimLeft = nullptr;
    waveformDimRight = nullptr;
    waveformFileLabel = nullptr;
    waveformTimeLabel = nullptr;
    waveformModeLabel = nullptr;
    if (scopeView) {
        scope_view_set_active(scopeView, false);
        set_live_scope_capture(false);
        scope_view_clear_source(scopeView);
    }
    scopeView = nullptr;
    scopeSourceKind = -2;
    timerPad = nullptr;
    g_sampler = nullptr;
    g_saveConfirmMsgbox = nullptr; // Clear msgbox reference

    g_parentContainer = nullptr;

    LV_LOG_INFO("Sampler GUI destroyed");

    lv_obj_delete_async(panel);

    auto* settingsPtr = crosspad::getPlatformServices().settings;
    if (settingsPtr) {
        settingsPtr->keypad.lightsOnNoteOff = false; // reset the lights on note off setting
        settingsPtr->keypad.lightsOnNoteOn = false;  // reset
        settingsPtr->keypad.noteOffOnRelease = g_prevNoteOffOnRelease;
    }

    auto* statusPtr = crosspad::getPlatformServices().status;
    if (statusPtr) statusPtr->keypadDirty = true; // refresh the keypad RGB

    lv_indev_t * navEncoder = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    // The caller restores the encoder group.
    (void)navEncoder;
}

// ── Accessors for shared logic objects ──────────────────────────────────

SamplerPadLogic& getSamplerPadLogic() { return g_samplerPadLogic; }
PadColorManager& getPadColorManager() { return g_padColorManager; }
KitOrchestrator& getKitOrchestrator() { return g_kitOrchestrator; }

} // namespace crosspad_sampler

#endif // USE_LVGL
