// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

#include <crosspad-sampler/ui/pad_editor.hpp>
#include <crosspad-sampler/ui/waveform_view.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <cstdio>

namespace crosspad_sampler {

// ── Internal state ────────────────────────────────────────────────────────────

struct PadEditorState {
    uint8_t   padIdx            = 0;
    lv_obj_t* volumeSpinbox     = nullptr;
    lv_obj_t* panSpinbox        = nullptr;
    lv_obj_t* pitchSpinbox      = nullptr;
    lv_obj_t* chokeSpinbox      = nullptr;
    lv_obj_t* playbackModeDropdown = nullptr;
    lv_obj_t* sampleList        = nullptr;
    lv_obj_t* waveformView      = nullptr;
};

static PadEditorState s_state;

// ── Helpers ───────────────────────────────────────────────────────────────────

static crosspad::Pad* getCurrentPad()
{
    crosspad::IKitManager* km = crosspad::getKitManager();
    if (!km) return nullptr;
    crosspad::KitInfo* kit = km->getCurrentKit();
    if (!kit) return nullptr;
    if (s_state.padIdx >= KIT_PADS) return nullptr;
    return &kit->pads[s_state.padIdx];
}

// ── Spinbox / dropdown change callbacks ──────────────────────────────────────

static void on_volume_changed(lv_event_t* e)
{
    (void)e;
    crosspad::Pad* pad = getCurrentPad();
    if (!pad || !s_state.volumeSpinbox) return;
    pad->volume = static_cast<int>(lv_spinbox_get_value(s_state.volumeSpinbox));
    pad->dirty         = true;
    pad->dirtyModified = true;
}

static void on_pan_changed(lv_event_t* e)
{
    (void)e;
    crosspad::Pad* pad = getCurrentPad();
    if (!pad || !s_state.panSpinbox) return;
    pad->pan = static_cast<int>(lv_spinbox_get_value(s_state.panSpinbox));
    pad->dirty         = true;
    pad->dirtyModified = true;
}

static void on_pitch_changed(lv_event_t* e)
{
    (void)e;
    crosspad::Pad* pad = getCurrentPad();
    if (!pad || !s_state.pitchSpinbox) return;
    pad->pitch = static_cast<int>(lv_spinbox_get_value(s_state.pitchSpinbox));
    pad->dirty         = true;
    pad->dirtyModified = true;
}

static void on_choke_changed(lv_event_t* e)
{
    (void)e;
    crosspad::Pad* pad = getCurrentPad();
    if (!pad || !s_state.chokeSpinbox) return;
    pad->choke_group = static_cast<int>(lv_spinbox_get_value(s_state.chokeSpinbox));
    pad->dirty         = true;
    pad->dirtyModified = true;
}

static void on_playback_mode_changed(lv_event_t* e)
{
    (void)e;
    crosspad::Pad* pad = getCurrentPad();
    if (!pad || !s_state.playbackModeDropdown) return;
    pad->playbackMode  = static_cast<uint8_t>(lv_dropdown_get_selected(s_state.playbackModeDropdown));
    pad->dirty         = true;
    pad->dirtyModified = true;
}

// ── Preview / back callbacks ──────────────────────────────────────────────────

static void on_preview_clicked(lv_event_t* e)
{
    (void)e;
    crosspad::getEventBus().postSamplePlayback(
        s_state.padIdx, 0, true, crosspad::EventSource::App);
}

static void on_back_clicked(lv_event_t* e)
{
    lv_obj_t* editor = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    pad_editor_destroy(editor);
}

// ── Sample list population ────────────────────────────────────────────────────

static void populate_sample_list(crosspad::Pad* pad)
{
    if (!s_state.sampleList) return;

    lv_obj_clean(s_state.sampleList);

    if (!pad || pad->samples.empty()) {
        lv_list_add_text(s_state.sampleList, "(no samples)");
        return;
    }

    for (size_t i = 0; i < pad->samples.size(); ++i) {
        lv_list_add_button(s_state.sampleList, nullptr,
                           pad->samples[i].sample.c_str());
    }
}

// ── Labeled spinbox helper ────────────────────────────────────────────────────

/// Creates a horizontal row containing a gray label (50 px wide) and a spinbox.
/// @return The spinbox lv_obj_t.
static lv_obj_t* create_labeled_spinbox(lv_obj_t* parent, const char* label,
                                        int32_t minVal, int32_t maxVal,
                                        int32_t value,
                                        lv_event_cb_t changeCb)
{
    // Row container
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 2, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Label
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_width(lbl, 50);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), LV_PART_MAIN);

    // Spinbox
    lv_obj_t* spinbox = lv_spinbox_create(row);
    lv_spinbox_set_range(spinbox, minVal, maxVal);
    lv_spinbox_set_value(spinbox, value);
    lv_spinbox_set_digit_format(spinbox, 4, 0);
    lv_obj_set_width(spinbox, LV_SIZE_CONTENT);

    if (changeCb) {
        lv_obj_add_event_cb(spinbox, changeCb, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    return spinbox;
}

// ── Public API ────────────────────────────────────────────────────────────────

lv_obj_t* pad_editor_create(lv_obj_t* parent, uint8_t padIdx)
{
    s_state = PadEditorState{};
    s_state.padIdx = padIdx;

    // ── Full-screen container ─────────────────────────────────────────────
    lv_obj_t* editor = lv_obj_create(parent);
    lv_obj_set_size(editor, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(editor, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(editor, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(editor, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(editor, 4, LV_PART_MAIN);
    lv_obj_clear_flag(editor, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header row ────────────────────────────────────────────────────────
    lv_obj_t* header = lv_obj_create(editor);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 2, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Back button
    lv_obj_t* backBtn = lv_button_create(header);
    lv_obj_t* backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
    lv_obj_add_event_cb(backBtn, on_back_clicked, LV_EVENT_CLICKED, editor);

    // Pad label
    char padLabelBuf[16];
    snprintf(padLabelBuf, sizeof(padLabelBuf), "Pad %u", static_cast<unsigned>(padIdx + 1));
    lv_obj_t* padLabel = lv_label_create(header);
    lv_label_set_text(padLabel, padLabelBuf);
    lv_obj_set_style_text_color(padLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    // Preview button
    lv_obj_t* previewBtn = lv_button_create(header);
    lv_obj_t* previewLbl = lv_label_create(previewBtn);
    lv_label_set_text(previewLbl, LV_SYMBOL_PLAY);
    lv_obj_add_event_cb(previewBtn, on_preview_clicked, LV_EVENT_CLICKED, nullptr);

    // ── Waveform view ─────────────────────────────────────────────────────
    s_state.waveformView = waveform_view_create(editor);

    // ── Sample list ───────────────────────────────────────────────────────
    s_state.sampleList = lv_list_create(editor);
    lv_obj_set_width(s_state.sampleList, LV_PCT(100));
    lv_obj_set_height(s_state.sampleList, 60);

    // ── Parameter spinboxes ───────────────────────────────────────────────
    crosspad::Pad* pad = getCurrentPad();
    int initVol   = pad ? pad->volume      : 100;
    int initPan   = pad ? pad->pan         : 0;
    int initPitch = pad ? pad->pitch       : 0;
    int initChoke = pad ? pad->choke_group : 0;
    uint8_t initMode = pad ? pad->playbackMode : 0;

    s_state.volumeSpinbox = create_labeled_spinbox(editor, "Vol",   0,   127, initVol,   on_volume_changed);
    s_state.panSpinbox    = create_labeled_spinbox(editor, "Pan", -100,  100, initPan,   on_pan_changed);
    s_state.pitchSpinbox  = create_labeled_spinbox(editor, "Pitch", -24,  24, initPitch, on_pitch_changed);
    s_state.chokeSpinbox  = create_labeled_spinbox(editor, "Choke",  0,   16, initChoke, on_choke_changed);

    // ── Playback mode dropdown ────────────────────────────────────────────
    lv_obj_t* modeRow = lv_obj_create(editor);
    lv_obj_set_width(modeRow, LV_PCT(100));
    lv_obj_set_height(modeRow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(modeRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(modeRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(modeRow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(modeRow, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(modeRow, 2, LV_PART_MAIN);
    lv_obj_clear_flag(modeRow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* modeLbl = lv_label_create(modeRow);
    lv_label_set_text(modeLbl, "Mode");
    lv_obj_set_width(modeLbl, 50);
    lv_obj_set_style_text_color(modeLbl, lv_color_hex(0x888888), LV_PART_MAIN);

    s_state.playbackModeDropdown = lv_dropdown_create(modeRow);
    lv_dropdown_set_options(s_state.playbackModeDropdown, "One-shot\nHold\nLoop");
    lv_dropdown_set_selected(s_state.playbackModeDropdown, initMode);
    lv_obj_add_event_cb(s_state.playbackModeDropdown, on_playback_mode_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    // ── Initial population ────────────────────────────────────────────────
    populate_sample_list(pad);
    if (pad && !pad->samples.empty()) {
        waveform_view_load(s_state.waveformView, padIdx, 0);
    }

    return editor;
}

void pad_editor_destroy(lv_obj_t* editor)
{
    s_state.volumeSpinbox        = nullptr;
    s_state.panSpinbox           = nullptr;
    s_state.pitchSpinbox         = nullptr;
    s_state.chokeSpinbox         = nullptr;
    s_state.playbackModeDropdown = nullptr;
    s_state.sampleList           = nullptr;
    s_state.waveformView         = nullptr;

    lv_obj_delete_async(editor);
}

void pad_editor_set_pad(lv_obj_t* editor, uint8_t padIdx)
{
    (void)editor;
    s_state.padIdx = padIdx;

    crosspad::Pad* pad = getCurrentPad();

    // Refresh spinbox values
    if (s_state.volumeSpinbox && pad) {
        lv_spinbox_set_value(s_state.volumeSpinbox, pad->volume);
    }
    if (s_state.panSpinbox && pad) {
        lv_spinbox_set_value(s_state.panSpinbox, pad->pan);
    }
    if (s_state.pitchSpinbox && pad) {
        lv_spinbox_set_value(s_state.pitchSpinbox, pad->pitch);
    }
    if (s_state.chokeSpinbox && pad) {
        lv_spinbox_set_value(s_state.chokeSpinbox, pad->choke_group);
    }
    if (s_state.playbackModeDropdown && pad) {
        lv_dropdown_set_selected(s_state.playbackModeDropdown, pad->playbackMode);
    }

    // Refresh sample list
    populate_sample_list(pad);

    // Reload waveform
    if (s_state.waveformView) {
        if (pad && !pad->samples.empty()) {
            waveform_view_load(s_state.waveformView, padIdx, 0);
        } else {
            waveform_view_clear(s_state.waveformView);
        }
    }
}

} // namespace crosspad_sampler

#endif // USE_LVGL
