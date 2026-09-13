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
#include "crosspad-sampler/PreviewPlayer.hpp"
#include <string>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/sequencer/SequencerController.hpp>
#include <crosspad/platform/IClock.hpp>
#include <crosspad/settings/CrosspadSettings.hpp>
#include <crosspad/status/CrosspadStatus.hpp>
#include <crosspad/led/RgbColor.hpp>
#include <crosspad/led/HtmlColor.hpp>
#include <crosspad/instrument/PitchedInstrument.hpp>

#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>
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
static lv_obj_t * previewBtn = NULL;   // plays the kit's preview.mid, looping
static lv_obj_t * previewLabel = NULL;
static lv_timer_t * previewTimer = NULL;

/* Loop recorder: record a take over the kit's own click, then let it run.
 * Reached through PlatformServices, so this panel stays as portable as the
 * rest of the sampler -- a platform with no sequencer simply has none of the
 * buttons do anything, which is what the null checks are for. */
static lv_obj_t * seqRecBtn = NULL;
static lv_obj_t * seqRecLabel = NULL;
static lv_obj_t * seqClickBtn = NULL;
static lv_obj_t * seqBarsBtn = NULL;
static lv_obj_t * seqBarsLabel = NULL;
static lv_obj_t * seqDubBtn = NULL;
static lv_obj_t * seqPlayBtn = NULL;
static lv_obj_t * seqStopBtn = NULL;
static lv_obj_t * seqSceneDrop = NULL;   // sits above the waveform
static lv_obj_t * seqRow = NULL;         // the transport strip, slides in and out
static lv_obj_t * seqEditBtn = NULL;     // the pad editor travels with the strip
static lv_obj_t * seqToggleBtn = NULL;   // "Advanced" -- brings the strip in
static bool seqControlsShown = false;    // the panel opens as a sampler
static lv_obj_t * seqBpmSpin = NULL;     // the kit's tempo, in the same row
static bool seqBpmSyncing = false;       // a sync must not read back as an edit
static bool seqTempoDirty = false;       // tempo changed, kit.json does not have it
static lv_timer_t * seqSyncTimer = NULL;

/* Encoder focus, made unmissable.
 *
 * The panel's buttons already carry colour for *state* -- REC goes red while a
 * take runs, CLK green while the click is on -- and those are written as local
 * styles. A focus style merely *added* to the object would lose to them, so the
 * focus skin is local too, on LV_STATE_FOCUSED, where it beats the default-state
 * write without erasing it: move away and the state colour is back.
 *
 * Blue rather than a lighter grey because the row is grey at rest; the point is
 * that the cursor is findable at a glance on a 320 px screen, from arm's length.
 */
/* Light fill, saturated edge. The dark labels on Back / Edit set their own
 * colour locally, so a child label never follows the parent into the focused
 * state -- a deep blue fill left black text on dark blue, which is what looked
 * broken. A pale fill keeps every existing label legible without touching it. */
#define CP_CLICK_OFF  lv_color_hex(0xFFFFFF)   /* metronome + bars at rest   */
#define CP_CLICK_ON   lv_color_hex(0xE8B33A)   /* metronome while it counts  */
#define CP_FOCUS_BG   lv_color_hex(0xBBD6FF)
#define CP_FOCUS_EDGE lv_color_hex(0x2F6BFF)

static void apply_focus_skin(lv_obj_t * o)
{
    if (!o) return;
    lv_obj_set_style_bg_color(o, CP_FOCUS_BG, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(o, CP_FOCUS_EDGE, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(o, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(o, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(o, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(o, CP_FOCUS_EDGE, LV_STATE_FOCUSED);
}

static crosspad::SequencerController * sequencer_or_null()
{
    return crosspad::getPlatformServices().sequencer;
}

/// The encoder moved the tempo. It goes to the clock at once -- the pattern is
/// rescaled with it, so a take keeps its place in the bar -- and into the kit,
/// which is where a tempo belongs: sequences.json carries it on the next sweep
/// by itself, kit.json waits for the save prompt like every other kit edit.
static void seq_bpm_changed_cb(lv_event_t * e)
{
    auto * sq = sequencer_or_null();
    if (!sq || seqBpmSyncing) return;
    /* No set_cursor_pos()/set_step() here, however tempting: both call
     * lv_spinbox_updatevalue(), which sets the textarea's text, which sends
     * VALUE_CHANGED straight back into this handler. That recursion overflowed
     * the LVGL task's stack the moment the panel gave the spinbox its starting
     * value -- the Sampler died on open. The cursor is pinned where it cannot
     * recurse: on focus and on click. */
    const int32_t v = lv_spinbox_get_value((lv_obj_t *)lv_event_get_target(e));
    if ((int32_t)lroundf(sq->getBpm()) == v) return;
    sq->setBpm((float)v);

    crosspad::KitInfo * kit = crosspad::getKitManager()
                                ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (kit && kit->bpm != (uint16_t)v) {
        kit->bpm = (uint16_t)v;
        seqTempoDirty = true;
    }
}

static void seq_sync_labels()
{
    auto * sq = sequencer_or_null();
    if (!sq || !seqRecBtn) return;
    /* Off screen there is nothing to say; the strip is synced as it comes back. */
    if (!seqControlsShown) return;

    const lv_color_t idle = lv_color_hex(0xB0B0B0);
    const lv_color_t live = lv_color_hex(0xE05050);
    const lv_color_t go   = lv_color_hex(0x50C070);

    const bool running = sq->getState() == crosspad::SequencerState::Running;
    const auto mode    = sq->getMode();
    const bool rec = running && mode == crosspad::SequencerMode::Record;
    const bool dub = running && mode == crosspad::SequencerMode::Overdub;

    /* Each button carries its own state now that they are four: which one is
     * lit says what the sequencer is doing, and the labels stay put so a
     * player is not reading text mid-take. */
    lv_obj_set_style_bg_color(seqRecBtn, rec ? live : idle, 0);
    if (seqDubBtn)  lv_obj_set_style_bg_color(seqDubBtn,  dub ? live : idle, 0);
    if (seqPlayBtn) lv_obj_set_style_bg_color(seqPlayBtn,
                        (running && !rec && !dub) ? go : idle, 0);
    if (seqStopBtn) lv_obj_set_style_bg_color(seqStopBtn, idle, 0);

    if (seqClickBtn) {
        /* White at rest, amber while the click is on. The colour is the whole
         * indicator -- there is no room at 34 x 22 for anything subtler. */
        lv_obj_set_style_bg_color(seqClickBtn,
            sq->getClickActive() ? CP_CLICK_ON : CP_CLICK_OFF, 0);
    }

    const uint8_t active  = sq->getActiveScene();
    const uint8_t pending = sq->getPendingScene();

    if (seqBarsBtn) lv_obj_set_style_bg_color(seqBarsBtn, CP_CLICK_OFF, 0);
    if (seqBarsLabel) {
        /* The length of the scene that is sounding, not a global setting --
         * each scene carries its own. */
        const uint8_t bars = sq->getSceneBars(active) ? sq->getSceneBars(active) : 1;
        lv_label_set_text_fmt(seqBarsLabel, "%ub", (unsigned)bars);
    }
    if (seqBpmSpin) {
        /* The clock is the authority: a kit landing with a tempo of its own
         * sets it without anyone touching this widget. */
        const int32_t shown = (int32_t)lroundf(sq->getBpm());
        if (lv_spinbox_get_value(seqBpmSpin) != shown) {
            seqBpmSyncing = true;
            lv_spinbox_set_value(seqBpmSpin, shown);
            seqBpmSyncing = false;
        }
    }

    if (seqSceneDrop && !lv_dropdown_is_open(seqSceneDrop)) {
        /* Shows the scene you asked for, amber until the loop end hands it
         * over -- a switch that has not happened yet must not look like one
         * that has. Left alone while the list is open, or the sync would fight
         * whoever is scrolling it. */
        if (lv_dropdown_get_selected(seqSceneDrop) != pending) {
            lv_dropdown_set_selected(seqSceneDrop, pending);
        }
        lv_obj_set_style_bg_color(seqSceneDrop,
            pending == active ? lv_color_hex(0x303030) : lv_color_hex(0xE0A050), 0);
        lv_obj_set_style_text_color(seqSceneDrop,
            pending == active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000), 0);
    }
}

/// The button is the transport, so its face is the only state the user sees.
static void preview_sync_label()
{
    if (!previewLabel) return;
    lv_label_set_text(previewLabel,
        crosspad_sampler::previewPlayer().running() ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
}
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
static lv_obj_t * vuMeter = NULL;
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

        /* The pad you hit is the pad you are editing. Until now this handler
         * only auditioned the sample: the editor kept showing whichever pad
         * was selected before it opened, so reaching for another sound meant
         * leaving the editor to pick it. The subject is what every panel reads
         * for "current pad", so setting it here moves the whole editor. */
        lv_subject_set_int(&s_pad_sampler, padIdx + 1);

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

/* Every sequencer button is the same shape; only the text and what it does
 * differ, so the shape is written once. */
static lv_obj_t * seq_make_button(lv_obj_t * parent, int w, int h, const char * text,
                                  lv_event_cb_t cb)
{
    lv_obj_t * b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_add_style(b, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    apply_focus_skin(b);
    if (g_sampler) lv_group_add_obj(g_sampler, b);
    return b;
}

/* The sampler opens as a sampler: pads, waveform, the kit. The sequencer is one
 * tab away -- pressing it slides the scene picker and the transport in from the
 * left edge, pressing it again sends them back. Hidden rather than deleted, so
 * the loop keeps running and the widgets keep their state; LVGL's group skips
 * hidden objects, which is what keeps the encoder from walking into a strip
 * that is not on screen. */
static constexpr int32_t kSeqSlideX = -330;   // past the left edge, both rows

static void seq_sync_labels();

static void seq_controls_slide_cb(void * var, int32_t v)
{
    (void)var;
    if (seqRow)       lv_obj_set_style_translate_x(seqRow, v, 0);
    if (seqSceneDrop) lv_obj_set_style_translate_x(seqSceneDrop, v, 0);
    /* The pad editor is an advanced control too: a sampler you just play does
     * not need it, and leaving it on the bottom row was the one thing still
     * saying "there is more here" on a screen meant to say nothing of the kind. */
    if (seqEditBtn)   lv_obj_set_style_translate_x(seqEditBtn, v, 0);
}

static void seq_controls_hidden_cb(lv_anim_t * a)
{
    (void)a;
    if (seqRow)       lv_obj_add_flag(seqRow, LV_OBJ_FLAG_HIDDEN);
    if (seqSceneDrop) lv_obj_add_flag(seqSceneDrop, LV_OBJ_FLAG_HIDDEN);
    if (seqEditBtn)   lv_obj_add_flag(seqEditBtn, LV_OBJ_FLAG_HIDDEN);
}

static void seq_controls_set(bool show, bool animate)
{
    if (!seqRow) return;
    seqControlsShown = show;

    /* An open list lives outside this panel's tree: sliding the dropdown out
     * from under it would leave it hanging over the pads. */
    if (seqSceneDrop && lv_dropdown_is_open(seqSceneDrop)) lv_dropdown_close(seqSceneDrop);

    /* Lit while the advanced controls are out, so the button says which of the
     * two screens you are looking at without reading the rest of it. */
    if (seqToggleBtn) {
        lv_obj_set_style_bg_color(seqToggleBtn, show ? CP_CLICK_ON : CP_CLICK_OFF, 0);
    }

    lv_anim_delete(seqRow, seq_controls_slide_cb);

    if (show) {
        lv_obj_remove_flag(seqRow, LV_OBJ_FLAG_HIDDEN);
        if (seqSceneDrop) lv_obj_remove_flag(seqSceneDrop, LV_OBJ_FLAG_HIDDEN);
        if (seqEditBtn)   lv_obj_remove_flag(seqEditBtn, LV_OBJ_FLAG_HIDDEN);
        seq_sync_labels();
    } else if (g_sampler && seqToggleBtn) {
        /* Focus cannot be left on something about to leave the screen -- but
         * it is only ours to move when it is standing on the strip. */
        lv_obj_t * focused = lv_group_get_focused(g_sampler);
        if (focused && (focused == seqSceneDrop || focused == seqEditBtn ||
                        lv_obj_get_parent(focused) == seqRow)) {
            lv_group_focus_obj(seqToggleBtn);
        }
    }

    if (!animate) {
        seq_controls_slide_cb(seqRow, show ? 0 : kSeqSlideX);
        if (!show) seq_controls_hidden_cb(nullptr);
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    /* The row is the anim's var, so the panel's teardown deletes the anim with
     * it rather than leaving it writing into freed memory. */
    lv_anim_set_var(&a, seqRow);
    lv_anim_set_exec_cb(&a, seq_controls_slide_cb);
    lv_anim_set_values(&a, show ? kSeqSlideX : 0, show ? 0 : kSeqSlideX);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (!show) lv_anim_set_completed_cb(&a, seq_controls_hidden_cb);
    lv_anim_start(&a);
}

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
    // the leave-app prompt never appeared for them. A tempo dialled in the
    // transport row is the same shape of edit: the loop already carries it in
    // sequences.json, kit.json only gets it if the player says save.
    return !dirtyPadNumbers.empty() || kit->instrument.dirty || seqTempoDirty;
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

    std::string message = "Do you want to save changes to the kit?\n\n";
    if (!dirtyPadNumbers.empty()) message += FormatDirtyPadsList(dirtyPadNumbers);
    if (seqTempoDirty) {
        crosspad::KitInfo * tempoKit = crosspad::getKitManager()
                                        ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (!dirtyPadNumbers.empty()) message += "\n";
        message += "Tempo: " + std::to_string(tempoKit ? tempoKit->bpm : 0) + " BPM";
    }
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

        seqTempoDirty = false;
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

        seqTempoDirty = false;
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
        /* Whatever tempo was dialled in belonged to the kit that just left. */
        seqTempoDirty = false;
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
    lv_obj_align(backBtn, LV_ALIGN_BOTTOM_MID, -72, -8);
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
    apply_focus_skin(backBtn);
    lv_group_add_obj(g_sampler, backBtn); // add the back button to the group

    lv_obj_t * editBtn = lv_button_create(cont);
    seqEditBtn = editBtn;
    lv_obj_align(editBtn, LV_ALIGN_BOTTOM_MID, 72, -8);
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
    apply_focus_skin(editBtn);
    lv_group_add_obj(g_sampler, editBtn); // add the edit button to the group

    /* Preview: play the kit's own preview.mid through the pads, looping, so a
     * kit can be auditioned without a host. Driven by an lv_timer rather than a
     * task -- see PreviewPlayer for why that is good enough. */
    previewBtn = lv_button_create(cont);
    lv_obj_align(previewBtn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_event_cb(previewBtn, [](lv_event_t * e) {
        (void)e;
        auto& pp = crosspad_sampler::previewPlayer();
        if (pp.running()) { pp.stop(); preview_sync_label(); return; }
        crosspad::KitInfo * kit = crosspad::getKitManager()
                                    ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (!kit) return;
        /* Same resolver the artwork uses, so the mount prefix and the two kit
         * layouts are already handled. */
        const std::string path = crosspad::kitAssetPath(kit->path, "preview.mid");
        if (!pp.load(path.c_str(), kit->bpm)) {
            LV_LOG_WARN("no usable preview.mid for this kit");
            return;
        }
        pp.start(lv_tick_get());
        if (!previewTimer) {
            previewTimer = lv_timer_create([](lv_timer_t *) {
                crosspad_sampler::previewPlayer().poll(lv_tick_get());
            }, 10, nullptr);
        }
        preview_sync_label();
    }, LV_EVENT_CLICKED, NULL);
    /* The timer outlives the click handler, so it has to die with the panel. */
    lv_obj_add_event_cb(previewBtn, [](lv_event_t * e) {
        (void)e;
        crosspad_sampler::previewPlayer().stop();
        if (previewTimer) { lv_timer_delete(previewTimer); previewTimer = nullptr; }
    }, LV_EVENT_DELETE, NULL);
    lv_obj_add_style(previewBtn, &styleButton, 0);
    lv_obj_add_style(previewBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_style(previewBtn, &styleButtonInactive, LV_STATE_DISABLED);
    previewLabel = lv_label_create(previewBtn);
    lv_obj_set_style_text_color(previewLabel, lv_color_hex(0x000000), 0);
    preview_sync_label();
    /* Parked: the preview beat is off the bottom row for now, so Back and Edit
     * get the width instead. The player and its timer still exist, so bringing
     * the button back is a matter of unhiding it. */
    lv_obj_add_flag(previewBtn, LV_OBJ_FLAG_HIDDEN);

    g_samplerPadEditorLogic = std::make_shared<PadEditorLogicHandler>();
    crosspad::getPadManager().registerPadLogic("Pad editor", g_samplerPadEditorLogic);

    static auto s_samplerLogic = std::shared_ptr<SamplerPadLogic>(&g_samplerPadLogic, [](SamplerPadLogic*){});
    crosspad::getPadManager().registerPadLogic("Sampler", s_samplerLogic);
    crosspad::getPadManager().setActivePadLogic("Sampler");

    // NULL callbacks: our own events are attached below.
    buttonFrame = createButtons(cont, NULL);
    /* Positioned later, flush with the scene dropdown -- the anchor has to
     * exist first. */

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

    /* "KITS", not a list glyph. The icon was recoloured to the theme's primary,
     * which on this background came out near-black: you could not see it, and
     * even spotted it did not say what it opened. A light plate with a word on
     * it does both. */
    pickKitBtn = lv_button_create(cont);
    lv_obj_set_size(pickKitBtn, 72, 20);
    /* Off the status bar: flush against it the plate read as part of the bar
     * rather than as something on this screen. */
    lv_obj_align(pickKitBtn, LV_ALIGN_TOP_RIGHT, -2, 6);
    lv_obj_add_event_cb(pickKitBtn, PickKit_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_style(pickKitBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(pickKitBtn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(pickKitBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pickKitBtn, 5, 0);
    lv_obj_set_style_pad_all(pickKitBtn, 0, 0);
    lv_obj_t * pickKitLabel = lv_label_create(pickKitBtn);
    lv_label_set_text(pickKitLabel, "Browse kit");
    lv_obj_set_style_text_font(pickKitLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(pickKitLabel, lv_color_hex(0x101010), 0);
    lv_obj_center(pickKitLabel);

    apply_focus_skin(pickKitBtn);
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

    /* Scene picker, directly above the waveform. A dropdown rather than a
     * cycling button because four scenes are a list you pick from, not a
     * sequence you step through -- and a list says which ones exist without
     * pressing anything. The pick only queues the scene; the swap still waits
     * for the loop end, which is what the amber is for. */
    seqSceneDrop = lv_dropdown_create(cont);
    lv_dropdown_set_options_static(seqSceneDrop, "Seq 1\nSeq 2\nSeq 3\nSeq 4");
    lv_obj_set_size(seqSceneDrop, WAVEFORM_CANVAS_WIDTH, 24);
    lv_obj_set_style_text_font(seqSceneDrop, &lv_font_montserrat_10, 0);
    lv_obj_set_style_pad_all(seqSceneDrop, 4, 0);
    /* -10, not -3: the pads hang from this and have to finish above the
     * transport row. Measured -- 120 px of pads from y 52 end at 172, and the
     * row starts at 174. */
    lv_obj_align_to(seqSceneDrop, waveformCanvas, LV_ALIGN_OUT_TOP_MID, 0, -10);
    lv_obj_add_event_cb(seqSceneDrop, [](lv_event_t * e) {
        auto * sq = sequencer_or_null();
        if (!sq) return;
        lv_obj_t * dd = (lv_obj_t *)lv_event_get_target(e);
        sq->selectScene((uint8_t)lv_dropdown_get_selected(dd));
        seq_sync_labels();
    }, LV_EVENT_VALUE_CHANGED, NULL);
    apply_focus_skin(seqSceneDrop);
    if (g_sampler) lv_group_add_obj(g_sampler, seqSceneDrop);
    seq_sync_labels();

    /* The pads sit flush with the dropdown -- raised together, so the grid
     * lines up with the scene picker beside it. */
    lv_obj_align_to(buttonFrame, seqSceneDrop, LV_ALIGN_OUT_LEFT_TOP, -10, 0);
    lv_obj_update_layout(cont);

    /* The six controls live in a flex row so LVGL owns the spacing and the
     * centring: the strip is 224 px against a 320 px screen, and a hand-chained
     * set of offsets would have to be recomputed every time a button changes
     * size. Order here is the order on screen. */
    seqRow = lv_obj_create(cont);
    lv_obj_remove_style_all(seqRow);
    lv_obj_set_size(seqRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seqRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seqRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(seqRow, 4, 0);
    lv_obj_remove_flag(seqRow, LV_OBJ_FLAG_SCROLLABLE);

    /* One row under the waveform, every button the same 34x22: click, loop
     * length, then the transport. Six of them are 224 px and the waveform
     * column is only 150, so the row runs left under the pads -- which is why
     * the pads had to be lifted to finish at 172. REC replaces the scene, DUB
     * adds to it, and play and stop are not a toggle: a player reaching for
     * stop mid-take should not have to know what a second press would mean. */
    /* Click and loop length take the same shape as the transport and sit to
     * REC's left, so the whole row reads as one strip of controls rather than
     * settings parked somewhere else. First in the row, so the order in this
     * function is the order on the screen. */
    /* Tempo, and it is the kit's rather than the session's: focus it, press to
     * edit, turn for a BPM at a time, press again to leave. A spinbox because
     * a tempo is a number you dial, and it is the widget the pad editor
     * already teaches -- a button would have to invent an edit mode of its
     * own. First in the row: the tempo is what the click and the bars are
     * counted against. */
    seqBpmSpin = lv_CreateStandardSpinbox(seqRow,
                                          (int32_t)crosspad::SequencerLimits::MIN_BPM,
                                          (int32_t)crosspad::SequencerLimits::MAX_BPM,
                                          120, seq_bpm_changed_cb, g_sampler, nullptr,
                                          &lv_font_montserrat_16);
    /* Local styles, so they beat the ones the helper added. */
    lv_obj_set_size(seqBpmSpin, 48, 24);
    lv_obj_set_style_pad_all(seqBpmSpin, 0, 0);
    lv_obj_set_style_radius(seqBpmSpin, 5, 0);
    lv_obj_set_style_bg_color(seqBpmSpin, CP_CLICK_OFF, 0);
    lv_obj_set_style_bg_opa(seqBpmSpin, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(seqBpmSpin, lv_color_hex(0x000000), 0);
    /* 16 px, not the row's 10: three digits at 10 px were a smudge, and this
     * build has no bold Montserrat -- size is the only weight available. The
     * field is a one-line textarea whose label spans the whole width, so
     * text_align is what centres the number in it. */
    lv_obj_set_style_text_font(seqBpmSpin, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(seqBpmSpin, LV_TEXT_ALIGN_CENTER, 0);
    /* No cursor block. The shared spinbox style paints LV_PART_CURSOR in the
     * same white as the field, so the digit under it disappeared -- which is
     * what made the tempo unreadable. */
    lv_obj_set_style_bg_opa(seqBpmSpin, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_text_color(seqBpmSpin, lv_color_hex(0x000000), LV_PART_CURSOR);
    lv_obj_set_style_text_opa(seqBpmSpin, LV_OPA_COVER, LV_PART_CURSOR);
    /* One BPM per detent, always. A spinbox steps by whichever digit its cursor
     * stands on -- a tap on the hundreds column and the encoder starts moving
     * the tempo in hundreds, which is not what a tempo control should do.
     * Pinned to the last digit here and re-pinned wherever it could move. */
    lv_spinbox_set_cursor_pos(seqBpmSpin, 0);
    lv_spinbox_set_step(seqBpmSpin, 1);
    /* Pinning the cursor sends VALUE_CHANGED (it rewrites the textarea), so it
     * is done under the sync flag: what comes back is our own doing, not a
     * tempo the player dialled. */
    auto pinCursor = [](lv_event_t * e) {
        lv_obj_t * spin = (lv_obj_t *)lv_event_get_target(e);
        const bool wasSyncing = seqBpmSyncing;
        seqBpmSyncing = true;
        lv_spinbox_set_cursor_pos(spin, 0);
        lv_spinbox_set_step(spin, 1);
        seqBpmSyncing = wasSyncing;
    };
    lv_obj_add_event_cb(seqBpmSpin, pinCursor, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(seqBpmSpin, pinCursor, LV_EVENT_CLICKED, NULL);
    apply_focus_skin(seqBpmSpin);

    /* Just "M". Four beat dots at 22 px high read as clutter rather than as a
     * beat -- the click is something you hear, and the colour already says
     * whether it is on. */
    seqClickBtn = seq_make_button(seqRow, 34, 22, "M", [](lv_event_t * e) {
        (void)e;
        auto * sq = sequencer_or_null();
        if (!sq) return;
        sq->setClickActive(!sq->getClickActive());
        seq_sync_labels();
    });

    seqBarsBtn = seq_make_button(seqRow, 34, 22, "4b", [](lv_event_t * e) {
        (void)e;
        auto * sq = sequencer_or_null();
        if (!sq) return;
        const uint8_t active = sq->getActiveScene();
        const uint8_t bars = sq->getSceneBars(active);
        const uint8_t next = (bars >= 8) ? 1 : (uint8_t)(bars ? bars * 2 : 1);
        sq->setSceneBars(active, next);
        seq_sync_labels();
    });
    seqBarsLabel = lv_obj_get_child(seqBarsBtn, 0);

    seqRecBtn = seq_make_button(seqRow, 34, 22, "REC", [](lv_event_t * e) {
        (void)e;
        auto * sq = sequencer_or_null();
        if (!sq) return;
        /* A second press closes the take without stopping the loop, which is
         * what you want after playing the part in. */
        if (sq->isRecording()) sq->playback();
        else                   sq->record();
        seq_sync_labels();
    });
    seqRecLabel = lv_obj_get_child(seqRecBtn, 0);

    seqDubBtn = seq_make_button(seqRow, 34, 22, "DUB", [](lv_event_t * e) {
        (void)e;
        auto * sq = sequencer_or_null();
        if (!sq) return;
        /* Overdub keeps what is there and layers on top -- the difference from
         * REC is the whole point of having both. */
        sq->overdub();
        seq_sync_labels();
    });

    seqPlayBtn = seq_make_button(seqRow, 34, 22, LV_SYMBOL_PLAY, [](lv_event_t * e) {
        (void)e;
        auto * sq = sequencer_or_null();
        if (!sq) return;
        sq->playback();
        seq_sync_labels();
    });

    seqStopBtn = seq_make_button(seqRow, 34, 22, LV_SYMBOL_STOP, [](lv_event_t * e) {
        (void)e;
        auto * sq = sequencer_or_null();
        if (!sq) return;
        sq->stop();
        seq_sync_labels();
    });

    /* y from the waveform so the row follows it, x centred on the panel. */
    lv_obj_update_layout(cont);
    const int32_t rowY = lv_obj_get_y(waveformCanvas) + lv_obj_get_height(waveformCanvas) + 18;
    lv_obj_align(seqRow, LV_ALIGN_TOP_MID, 0, rowY);

    /* "Advanced", in the gap Back and Edit leave in the middle of the bottom
     * row -- the slot Edit used to reach past. A word rather than an arrow:
     * what it opens is not obvious from a chevron, and with Edit now behind it
     * the button is the only way in. */
    seqToggleBtn = lv_button_create(cont);
    lv_obj_set_size(seqToggleBtn, 76, 32);
    lv_obj_align(seqToggleBtn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_style(seqToggleBtn, &styleButton, 0);
    lv_obj_add_style(seqToggleBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_event_cb(seqToggleBtn, [](lv_event_t * e) {
        (void)e;
        seq_controls_set(!seqControlsShown, true);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t * seqToggleLabel = lv_label_create(seqToggleBtn);
    lv_obj_set_style_text_color(seqToggleLabel, lv_color_hex(0x000000), 0);
    lv_label_set_text(seqToggleLabel, "Advanced");
    lv_obj_center(seqToggleLabel);
    apply_focus_skin(seqToggleBtn);
    if (g_sampler) lv_group_add_obj(g_sampler, seqToggleBtn);

    /* Closed on entry: opening the Sampler is opening a sampler. */
    seq_controls_set(false, false);

    /* A queued scene becomes active at a loop end nobody clicked, so the
     * labels have to be refreshed by time rather than by event. */
    if (!seqSyncTimer) {
        seqSyncTimer = lv_timer_create([](lv_timer_t *) { seq_sync_labels(); }, 200, nullptr);
    }

    /* The loop deliberately keeps running when the panel closes -- it lives on
     * its own task. Only the pointers to the widgets die with it. */
    lv_obj_add_event_cb(seqRecBtn, [](lv_event_t * e) {
        (void)e;
        seqRecBtn = seqRecLabel = seqClickBtn = seqBarsBtn = seqBarsLabel = NULL;
        seqDubBtn = seqPlayBtn = seqStopBtn = seqSceneDrop = seqBpmSpin = NULL;
        seqRow = seqEditBtn = seqToggleBtn = NULL;
        if (seqSyncTimer) { lv_timer_delete(seqSyncTimer); seqSyncTimer = NULL; }
    }, LV_EVENT_DELETE, NULL);
    seq_sync_labels();

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

    vuMeter = create_vu_meter(cont, 6, 85);
    lv_obj_align(vuMeter, LV_ALIGN_RIGHT_MID, -4, -3);

    return cont;
}

void sampler_panel_set_covered(bool covered)
{
    if (timerPad)     { if (covered) lv_timer_pause(timerPad);     else lv_timer_resume(timerPad); }
    if (seqSyncTimer) { if (covered) lv_timer_pause(seqSyncTimer); else lv_timer_resume(seqSyncTimer); }
    /* Hidden objects do not invalidate, which is the only handle on the VU:
     * its timer lives in crosspad-gui and is not ours to pause. */
    if (vuMeter) {
        if (covered) lv_obj_add_flag(vuMeter, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_remove_flag(vuMeter, LV_OBJ_FLAG_HIDDEN);
    }
    if (covered) {
        if (scopeView) scope_view_set_active(scopeView, false);
        set_live_scope_capture(false);   // and the audio-side tap with it
    } else {
        apply_waveform_display_mode();   // puts the scope and its capture back
    }
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

    /* An open dropdown list is a live object outside this panel's tree, and it
     * is bound to the group about to be freed. Tearing the panel down around
     * it leaves the encoder reaching into freed memory -- the crash class
     * CLAUDE.md warns about, and the sampler died on an ENC_ROTATE with the
     * list open. Close it first; closed, there is nothing to race. */
    if (seqSceneDrop && lv_dropdown_is_open(seqSceneDrop)) {
        lv_dropdown_close(seqSceneDrop);
    }

    lv_group_delete(g_sampler); // delete the group
    g_sampler = NULL;           // freed: never hand it out again
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
    vuMeter = nullptr;
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
