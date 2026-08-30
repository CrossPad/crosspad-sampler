#ifdef USE_LVGL

// overlay for the pad, samples settings

#include <lvgl.h>
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/ui/waveform_view.hpp"

#include "crosspad-gui/crosspad_gui.h"

#include <crosspad/led/ColorList.hpp>
#include <crosspad/led/HtmlColor.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/platform/IClock.hpp>
#include <crosspad/status/CrosspadStatus.hpp>

#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using crosspad::ColorList;
using crosspad::Pad;
using crosspad::SampleSlot;
using crosspad::KitInfo;

// Global scope — referenced by kit_selector_overlay.cpp
lv_style_t stylePadEditorContSel;

namespace crosspad_sampler {

// Helper to get the navigation encoder from the GUI platform
static inline lv_indev_t * getEncoder() {
    return crosspad_gui::getGuiPlatform().getNavigationEncoder();
}

// Helper to get current time in milliseconds (portable)
static inline uint32_t portable_millis() {
    auto * clk = crosspad::getPlatformServices().clock;
    if (clk) return static_cast<uint32_t>(clk->getTimeUs() / 1000);
    return lv_tick_get();
}

// Helper to get the current kit
static inline KitInfo * getCurrentKit() {
    auto * mgr = crosspad::getKitManager();
    return mgr ? mgr->getCurrentKit() : nullptr;
}

// Helper to get the platform status
static inline crosspad::CrosspadStatus * getStatus() {
    return crosspad::getPlatformServices().status;
}

// last pad was different than current one, so dont push pad reloading
static bool dontUpdateSettings = false;


// Styles are now available via crosspad-gui/styles/styles.h (included through crosspad_gui.h)

lv_group_t * pad_editor_group; // single flat group: settings widgets, list items, then bottom buttons
static lv_group_t * s_priorEncoderGroup = nullptr; // restored on destroy so encoder doesn't dangle

static crosspad_gui::ILvglApp * thisApp = NULL; // current app instance
static lv_obj_t * s_padEditorCont = NULL; // editor's own container — correctly bounded below the status bar

// settings subjects
lv_subject_t s_Color;
lv_subject_t s_ColorPressed;
lv_subject_t s_ColorPlaying;
lv_subject_t s_ChokeGroup;
lv_subject_t s_Pitch;
lv_subject_t s_Volume;
lv_subject_t s_Pan;

extern lv_subject_t s_pad_sampler; // subject for the last pressed pad (defined in sampler_panel.cpp)

lv_obj_t * list = NULL; // samples list
lv_timer_t * t_padUpdate;

static Pad * lastPad = NULL;
const int maxSamples = 1;

// Duplicate pad state
static bool g_duplicateMode = false;
static int g_duplicateSourcePadIdx = -1;  // 0-based
static lv_obj_t * g_duplicateBtn = NULL;

// Spinbox widget references for cleanup
static lv_obj_t * panSpinbox = NULL;
static lv_obj_t * volumeSpinbox = NULL;
static lv_obj_t * chokeSpinbox = NULL;
static lv_obj_t * startSpinbox = NULL;
static lv_obj_t * endSpinbox = NULL;

// Waveform view widget (replaces manual canvas + markers)
static lv_obj_t * waveformView = NULL;
static char currentWaveformPath[128] = {0};
static SampleInfo waveformSampleInfo = {};
static bool waveformInfoValid = false;
static uint32_t waveformStartTime = 0;
static bool waveformPlaying = false;

// Step multiplier for start/end spinboxes
static int stepMultiplierIndex = 3; // Index into multipliers array (default x1000)
static const int stepMultipliers[] = {1, 10, 100, 1000, 10000}; // Available multipliers
static const int stepMultipliersCount = 5;
static lv_obj_t * stepMultiplierBtn = NULL; // Button showing current multiplier

// declarations
static lv_obj_t * lv_CreatePadEditorContext(lv_obj_t *parent);
static void Browse_button_cb(lv_event_t * e);
static void UpdatePadEditor(lv_timer_t * e);
static void DestroyPadEditor(lv_event_t * e);
static void UpdateWaveformMarkers();
static void ShowDuplicateConfirmation(int sourcePadIdx, int targetPadIdx);
static void ExitDuplicateMode();

// Exit duplicate mode and reset button style
static void ExitDuplicateMode()
{
    g_duplicateMode = false;
    g_duplicateSourcePadIdx = -1;
    if (g_duplicateBtn) {
        lv_obj_set_style_bg_color(g_duplicateBtn, lv_color_hex(0x000000), 0);  // reset to default
        lv_obj_set_style_bg_opa(g_duplicateBtn, LV_OPA_TRANSP, 0);            // let style handle it
        lv_obj_remove_local_style_prop(g_duplicateBtn, LV_STYLE_BG_COLOR, 0);
        lv_obj_remove_local_style_prop(g_duplicateBtn, LV_STYLE_BG_OPA, 0);
    }
}

// Statics for duplicate confirmation dialog (need stable addresses for lambda captures)
static int s_dupSourceIdx = 0;
static int s_dupTargetIdx = 0;

// Show confirmation dialog for duplicating a pad
static void ShowDuplicateConfirmation(int sourcePadIdx, int targetPadIdx)
{
    // Same pad - just exit mode
    if (sourcePadIdx == targetPadIdx) {
        ExitDuplicateMode();
        return;
    }

    s_dupSourceIdx = sourcePadIdx;
    s_dupTargetIdx = targetPadIdx;

    lv_obj_t * mbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(mbox, "Copy Pad");

    char msg[80];
    snprintf(msg, sizeof(msg), "Copy Pad %d to Pad %d?\nTarget will be overwritten.", sourcePadIdx + 1, targetPadIdx + 1);
    lv_msgbox_add_text(mbox, msg);

    lv_obj_t * okBtn = lv_msgbox_add_footer_button(mbox, "OK");
    lv_obj_t * cancelBtn = lv_msgbox_add_footer_button(mbox, "Cancel");

    lv_obj_add_event_cb(okBtn, [](lv_event_t * e) {
        KitInfo * kit = getCurrentKit();
        if (kit == NULL) return;

        Pad * source = &kit->pads[s_dupSourceIdx];
        Pad * target = &kit->pads[s_dupTargetIdx];

        // Wipe target sampler state and copy pad data
        auto & cbs = sampler_get_platform_callbacks();
        if (cbs.player.wipeOut) cbs.player.wipeOut(s_dupTargetIdx);
        target->CopyFrom(*source);

        // Reload target pad in sampler engine
        if (cbs.audio.reloadPad) cbs.audio.reloadPad(s_dupTargetIdx);

        // Update LED color
        crosspad::getPadManager().setPadColor(s_dupTargetIdx, crosspad::HtmlColor(target->color));

        LV_LOG_INFO("Pad %d copied to Pad %d", s_dupSourceIdx + 1, s_dupTargetIdx + 1);

        // Switch view to newly copied pad
        lv_subject_set_int(&s_pad_sampler, s_dupTargetIdx + 1);
        lastPad = NULL; // force UI refresh

        // Close dialog
        lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_t * mbox = lv_obj_get_parent(lv_obj_get_parent(btn));
        if (mbox) lv_msgbox_close(mbox);

        ExitDuplicateMode();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(cancelBtn, [](lv_event_t * e) {
        lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_t * mbox = lv_obj_get_parent(lv_obj_get_parent(btn));
        if (mbox) lv_msgbox_close(mbox);
        ExitDuplicateMode();
    }, LV_EVENT_CLICKED, NULL);
}

// create function
lv_obj_t * pad_editor_create(lv_obj_t *parent, crosspad_gui::ILvglApp * a) {
    thisApp = a;


    // main container
    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_obj_get_content_width(parent), lv_obj_get_content_height(parent));
    lv_obj_center(cont);
    lv_obj_add_style(cont, &styleAppContainer, 0);
    s_padEditorCont = cont;
    lv_obj_add_event_cb(cont, DestroyPadEditor, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(cont, [](lv_event_t * e) {
        // if container is clicked, that means blank space is clicked, so defocus all childres
        LV_LOG_TRACE("Container clicked");
        lv_indev_set_group(getEncoder(), pad_editor_group);
    }, LV_EVENT_CLICKED, NULL); // consume the click event

    // style for the selected container
    lv_style_init(&stylePadEditorContSel);
    lv_style_set_border_color(&stylePadEditorContSel, lv_color_white());
    lv_style_set_border_width(&stylePadEditorContSel, 3);
    lv_style_set_border_opa(&stylePadEditorContSel, LV_OPA_COVER);

    pad_editor_group = lv_group_create();
    lv_group_set_wrap(pad_editor_group, true);

    // remember caller's encoder group (e.g. sampler) so we can restore it on destroy
    s_priorEncoderGroup = lv_indev_get_group(getEncoder());
    lv_indev_set_group(getEncoder(), pad_editor_group);

    lv_obj_update_layout(parent); // refresh layout
    // constants for the buttons (3 buttons: Close, Record, Browse)
    const int buttonYoffset = -8;

    // add close button (left)
    lv_obj_t * closeBtn = lv_button_create(cont);
    lv_obj_align(closeBtn, LV_ALIGN_BOTTOM_LEFT, 10, buttonYoffset);
    lv_obj_add_event_cb(closeBtn, [](lv_event_t * e) {
        LV_LOG_WARN("Close button clicked");
        // DestroyPadEditor is triggered via LV_EVENT_DELETE on the container
        lv_obj_t * editor = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        if (editor) lv_obj_delete_async(editor);
    }, LV_EVENT_CLICKED, cont);
    lv_obj_add_style(closeBtn, &styleButton, 0);
    lv_obj_add_style(closeBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_style(closeBtn, &styleButtonInactive, LV_STATE_DISABLED);
    lv_obj_t * closeLabel = lv_label_create(closeBtn);
    lv_label_set_text(closeLabel, "Close");
    lv_obj_set_style_text_color(closeLabel, lv_color_hex(0x000000), 0);

    // add copy/duplicate button (center)
    g_duplicateBtn = lv_button_create(cont);
    lv_obj_align(g_duplicateBtn, LV_ALIGN_BOTTOM_MID, 0, buttonYoffset);
    lv_obj_add_event_cb(g_duplicateBtn, [](lv_event_t * e) {
        if (g_duplicateMode) {
            // Second click cancels duplicate mode
            ExitDuplicateMode();
            return;
        }
        KitInfo * kit = getCurrentKit();
        if (kit == NULL) return;
        int padId = lv_subject_get_int(&s_pad_sampler);
        if (padId <= 0 || padId > 16) return;

        Pad * pad = &kit->pads[padId - 1];
        if (pad->GetSampleCount() == 0) return; // nothing to copy

        g_duplicateSourcePadIdx = padId - 1;
        g_duplicateMode = true;

        // Visual feedback: highlight button orange
        if (g_duplicateBtn) {
            lv_obj_set_style_bg_color(g_duplicateBtn, lv_color_hex(0xFF8800), 0);
            lv_obj_set_style_bg_opa(g_duplicateBtn, LV_OPA_COVER, 0);
        }

        LV_LOG_INFO("Duplicate mode: source Pad %d, tap target pad", padId);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_style(g_duplicateBtn, &styleButton, 0);
    lv_obj_add_style(g_duplicateBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_style(g_duplicateBtn, &styleButtonInactive, LV_STATE_DISABLED);
    lv_obj_t * dupLabel = lv_label_create(g_duplicateBtn);
    lv_label_set_text(dupLabel, "Copy");
    lv_obj_set_style_text_color(dupLabel, lv_color_hex(0x000000), 0);

    // add browse button (right)
    lv_obj_t * browseBtn = lv_button_create(cont);
    lv_obj_align(browseBtn, LV_ALIGN_BOTTOM_RIGHT, -10, buttonYoffset);
    lv_obj_add_event_cb(browseBtn, Browse_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_style(browseBtn, &styleButton, 0);
    lv_obj_add_style(browseBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_style(browseBtn, &styleButtonInactive, LV_STATE_DISABLED);
    lv_obj_t * browseLabel = lv_label_create(browseBtn);
    lv_label_set_text(browseLabel, "Browse");
    lv_obj_set_style_text_color(browseLabel, lv_color_hex(0x000000), 0);

    // constants for both panels
    const int panelTopMargin = 4; // no title above the panel anymore, keep this tight
    const int panelBottomMargin = 27; // clears the Close/Copy/Browse footer band
    const int panelHeight = lv_obj_get_content_height(cont) - panelTopMargin - panelBottomMargin;
    const int panelWidth = (lv_obj_get_content_width(cont) / 2) - 15;
    const int panelXoffset = 5; // from the wall

    // Left panel with settings (settings widgets are added to pad_editor_group inside)
    // Anchored to the top and stretched down — reclaims the space a removed
    // title used to leave above it, so the params fit without scrolling.
    lv_obj_t * settingsContext = lv_CreatePadEditorContext(cont);
    lv_obj_align(settingsContext, LV_ALIGN_TOP_LEFT, panelXoffset, panelTopMargin);
    lv_obj_set_size(settingsContext, panelWidth, panelHeight);

    // Right side: waveform display using waveform_view widget
    waveformView = waveform_view_create(cont);
    lv_obj_align(waveformView, LV_ALIGN_RIGHT_MID, -5, 0);

    // Sample list as thin strip above waveform (items are added to pad_editor_group in UpdatePadEditor)
    list = lv_list_create(cont);
    lv_obj_add_style(list, &styleFileExplorerList, 0);
    lv_obj_add_style(list, &styleFileExplorerScrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_width(list, lv_obj_get_width(waveformView)); // Match waveform width
    lv_obj_set_style_max_height(list, 30, 0); // Thin strip - 30px height
    lv_obj_align_to(list, waveformView, LV_ALIGN_OUT_TOP_MID, 0, -5); // Position above waveform
    lv_list_add_button(list, LV_SYMBOL_WARNING, "Select pad"); // default item

    // Bottom buttons appended last so encoder cycle ends with Close → Copy → Browse → wrap
    lv_group_add_obj(pad_editor_group, closeBtn);
    lv_group_add_obj(pad_editor_group, g_duplicateBtn);
    lv_group_add_obj(pad_editor_group, browseBtn);

    // Default focus: first useful settings widget
    if (chokeSpinbox) lv_group_focus_obj(chokeSpinbox);

    // update timer setup - faster polling for better responsiveness
    t_padUpdate = lv_timer_create(UpdatePadEditor, 200, list);
    return cont;
}

// cleanup function
// Helper function to update waveform markers using waveform_view API
static void UpdateWaveformMarkers() {
    if (!waveformView || !waveformInfoValid || waveformSampleInfo.sampleCount == 0) return;

    KitInfo * kit = getCurrentKit();
    if (!kit) return;
    int padId = lv_subject_get_int(&s_pad_sampler) - 1;
    if (padId < 0 || padId >= 16) return;
    Pad * pad = &kit->pads[padId];
    if (!pad->GetSampleCount()) return;

    uint32_t startS = pad->samples[0].start;
    uint32_t endS = pad->samples[0].end;
    uint32_t totalSamples = waveformSampleInfo.sampleCount;

    // If end is 0 or beyond total, use total samples
    if (endS == 0 || endS > totalSamples) endS = totalSamples;

    // Calculate marker positions (0.0 to 1.0 range)
    float startPos = totalSamples > 0 ? (float)startS / (float)totalSamples : 0.0f;
    float endPos = totalSamples > 0 ? (float)endS / (float)totalSamples : 1.0f;

    waveform_view_set_markers(waveformView, startPos, endPos);
}

static void DestroyPadEditor(lv_event_t * e) {
    lv_timer_delete(t_padUpdate);

    lv_subject_deinit(&s_Color);
    lv_subject_deinit(&s_ColorPressed);
    lv_subject_deinit(&s_ColorPlaying);
    lv_subject_deinit(&s_ChokeGroup);
    lv_subject_deinit(&s_Pitch);
    lv_subject_deinit(&s_Volume);
    lv_subject_deinit(&s_Pan);

    // restore encoder to caller's group BEFORE deleting our groups, otherwise
    // the indev keeps a dangling pointer and the next lv_indev_read will deref freed mem
    lv_indev_set_group(getEncoder(), s_priorEncoderGroup);
    s_priorEncoderGroup = nullptr;

    lv_group_remove_all_objs(pad_editor_group);
    lv_group_delete(pad_editor_group);
    pad_editor_group = nullptr;

    // Spinbox widgets will be automatically cleaned up via LV_EVENT_DELETE when container is destroyed

    s_padEditorCont = NULL;

    // Clear duplicate mode state
    g_duplicateMode = false;
    g_duplicateSourcePadIdx = -1;
    g_duplicateBtn = NULL;

    // Clear waveform state
    waveformView = NULL;
    stepMultiplierBtn = NULL;
    stepMultiplierIndex = 3; // Reset to x1000
    currentWaveformPath[0] = '\0';

    waveformInfoValid = false;
    waveformPlaying = false;

    lastPad = NULL;

    // Restore full LED colors for all pads (undo dim-others effect).
    {
        KitInfo * kit = getCurrentKit();
        if (kit) {
            auto & pm = crosspad::getPadManager();
            for (int i = 0; i < 16; i++) {
                pm.setPadColor((uint8_t)i, crosspad::HtmlColor(kit->pads[i].color));
            }
        }
    }

    // reload pad if it was modified in a way that requires sampler setup
    // (we avoid reload when only volume/pan/color were changed because those are applied immediately)
    auto & cbs = sampler_get_platform_callbacks();
    if (cbs.audio.reloadPads) cbs.audio.reloadPads();

    // switch back to default logic, assuming that sampler is the only entry point to this app
    crosspad::getPadManager().setActivePadLogic("Sampler");
    auto * st = getStatus();
    if (st) st->isSamplerPlaying = true; // re-enable sampler direct playback
    LV_LOG_INFO("Pad editor destroyed");
}

// spinbox with subject changing on value change and correct digit format
//@param label: label to attach the spinbox to
//@param txt: text to display on the label
//@param xOffset: offset from the label to the spinbox
//@param subject: subject to update on value change
//@param min: minimum value
//@param max: maximum value
//@returns spinbox object
lv_obj_t * lv_CreateSpinnboxParam(lv_obj_t * label, char * txt, int xOffset, lv_subject_t *subject, int min, int max) {

    lv_label_set_text(label, txt);
    lv_subject_init_int(subject, min);

    // Value change callback to update subject
    auto value_change_cb = [](lv_event_t * e) {
        lv_obj_t * spinbox = (lv_obj_t*)lv_event_get_target(e);
        int32_t value = lv_spinbox_get_value(spinbox);
        lv_subject_t * subject = (lv_subject_t*)lv_event_get_user_data(e);
        lv_subject_set_int(subject, value);
        LV_LOG_INFO("Spinbox value changed: %d", value);
    };

    // Create spinbox using the standard widget
    lv_obj_t * spinbox = lv_CreateStandardSpinbox(lv_obj_get_parent(label), min, max,
                         lv_subject_get_int(subject), value_change_cb,
                         pad_editor_group, subject, &lv_font_montserrat_14);

    // Handle negative values width adjustment (needed for Pan which can be negative)
    if (min < 0) {
        uint8_t digits = CalcDigits(LV_MAX(LV_ABS(min), LV_ABS(max)));
        lv_obj_set_width(spinbox, 5 + (8 * digits) + 8); // +8 for minus sign
    }

    lv_obj_align_to(spinbox, label, LV_ALIGN_OUT_RIGHT_MID, xOffset, 0);
    lv_group_add_obj(pad_editor_group, spinbox); // settings group

    return spinbox;
}

void ColorRollerCb(lv_event_t * e) {
    ColorList colorList;
    static char buf[16]; // option name from roller
    lv_obj_t * roller = (lv_obj_t *)lv_event_get_target(e);
    int selected = lv_roller_get_selected(roller); // idx
    // check if "The same" is selected or present
    bool withTheSame = lv_roller_get_option_count(roller) != colorList.GetCount(); // with "The same" option available
    LV_LOG_TRACE("withTheSame: %d, roller opt: %d, colorList: %d", withTheSame, lv_roller_get_option_count(roller), colorList.GetCount());
    lv_roller_get_selected_str(roller, buf, sizeof(buf));
    bool theSame = strcmp(buf, "The same") == 0; // if "The same" is selected
    if (!theSame && withTheSame) selected--; // fix offset
    // set color
    lv_style_t * style = (lv_style_t *)lv_event_get_user_data(e);
    auto color = lv_color_hex(colorList.Get(selected));
    if (lv_color_eq(color, lv_color_black())) color = lv_color_hex(0x202020); // avoid pure black
    lv_style_set_text_color(style, color);
    lv_obj_report_style_change(style);
    lv_obj_invalidate(roller);
    LV_LOG_USER("Selected color: %s id: %d", buf, selected);
    crosspad::getPadManager().setPadColor(lv_subject_get_int(&s_pad_sampler) - 1, crosspad::HtmlColor(colorList.Get(selected)));

    // Mark pad as modified (color change is instant but should be saved)
    if (!dontUpdateSettings) {
        KitInfo * kit = getCurrentKit();
        if (kit != NULL) {
            int padId = lv_subject_get_int(&s_pad_sampler) - 1;
            if (padId >= 0 && padId < 16) {
                kit->pads[padId].dirtyModified = true;
            }
        }
    }
}

lv_obj_t * lv_CreateColorRoler(lv_obj_t *parent, lv_style_t * styleSelectedColor, bool withTheSame) {
    lv_obj_t * collorRoller = lv_roller_create(parent);
    lv_obj_set_style_text_line_space(collorRoller, 4, 0); // between entries
    lv_obj_add_style(collorRoller, &styleAppContainer, 0);
    lv_obj_add_style(collorRoller, &styleAppContainer, LV_PART_SELECTED);
    lv_obj_add_style(collorRoller, styleSelectedColor, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(collorRoller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(collorRoller, LV_OPA_TRANSP, LV_PART_SELECTED);
    ColorList colorList; // list of colors from kit library
    if (withTheSame) lv_roller_set_options(collorRoller, colorList.GetPressed(), LV_ROLLER_MODE_NORMAL);
    else lv_roller_set_options(collorRoller, colorList.Get(), LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(collorRoller, 1);
    lv_obj_add_event_cb(collorRoller, ColorRollerCb, LV_EVENT_VALUE_CHANGED, styleSelectedColor);
    lv_obj_add_event_cb(collorRoller, ColorRollerCb, LV_EVENT_KEY, styleSelectedColor);
    return collorRoller;
}

static lv_obj_t * lv_CreatePadEditorContext(lv_obj_t *parent)
{
    dontUpdateSettings = true; // prevent updating settings while creating the context
    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_add_style(cont, &styleAppContainer, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);

    // setup pad logic
    crosspad::getPadManager().setActivePadLogic("Pad editor");
    auto * st = getStatus();
    if (st) st->isSamplerPlaying = false; // ensure that sampler is inactive for preview mode
    const int entryYoffset = 4; // offset between entries
    const int entryXoffset = 5; // offset from the wall
    // style for selected color text label
    static lv_style_t styleSelectedColor;
    static lv_style_t styleSelectedColorPressed;
    lv_style_init(&styleSelectedColor);
    lv_style_init(&styleSelectedColorPressed);
    lv_style_set_text_color(&styleSelectedColor, lv_color_white());
    lv_style_set_text_color(&styleSelectedColorPressed, lv_color_white());

    // color
    lv_obj_t * color = lv_label_create(cont);
    lv_obj_align(color, LV_ALIGN_TOP_LEFT, entryXoffset, entryYoffset/2);
    lv_label_set_text(color, "Color: ");
    // color roller
    lv_obj_t * colorRoller = lv_CreateColorRoler(cont, &styleSelectedColor, false);
    lv_group_add_obj(pad_editor_group, colorRoller); // settings group
    lv_obj_align_to(colorRoller, color, LV_ALIGN_OUT_RIGHT_MID, 2, 0);

    // Add event to scroll to top when color roller gets focus (for wrap-around navigation)
    lv_obj_add_event_cb(colorRoller, [](lv_event_t * e) {
        if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
            lv_obj_t * roller = (lv_obj_t*)lv_event_get_target(e);
            lv_obj_t * cont = lv_obj_get_parent(roller);
            lv_obj_scroll_to_y(cont, 0, LV_ANIM_ON);
            LV_LOG_INFO("Color roller focused - scrolling to top");
        }
    }, LV_EVENT_FOCUSED, NULL);

    // choke group
    lv_obj_t * labelChoke = lv_label_create(cont);
    lv_obj_align_to(labelChoke, color, LV_ALIGN_OUT_BOTTOM_LEFT, 0, entryYoffset);
    chokeSpinbox = lv_CreateSpinnboxParam(labelChoke, (char *)"Choke: ", 2, &s_ChokeGroup, 0, 7);
    // Additional logic for choke group (applied on value change via subject)
    lv_obj_add_event_cb(chokeSpinbox, [](lv_event_t * e) {
        int value = lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        LV_LOG_INFO("Choke group changed to: %d", value);
        if (dontUpdateSettings) return;
        KitInfo * kit = getCurrentKit();
        if (kit == NULL) return;
        Pad * pad = &kit->pads[lv_subject_get_int(&s_pad_sampler) - 1];
        pad->choke_group = value;
        pad->dirty = true; // needs reload
        pad->dirtyModified = true; // modified
        auto & cbs = sampler_get_platform_callbacks();
        if (cbs.audio.reloadPad) cbs.audio.reloadPad(lv_subject_get_int(&s_pad_sampler) - 1);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // volume
    lv_obj_t * labelVolume = lv_label_create(cont);
    lv_obj_align_to(labelVolume, labelChoke, LV_ALIGN_OUT_BOTTOM_LEFT, 0, entryYoffset);
    volumeSpinbox = lv_CreateSpinnboxParam(labelVolume, (char *)"Volume: ", 2, &s_Volume, 0, 127);
    // Additional logic for volume (applied on value change via subject)
    lv_obj_add_event_cb(volumeSpinbox, [](lv_event_t * e) {
        int value = lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        LV_LOG_INFO("Volume changed to: %d", value);
        if (dontUpdateSettings) return;
        KitInfo * kit = getCurrentKit();
        if (kit == NULL) return;
        int padId = lv_subject_get_int(&s_pad_sampler) - 1;
        Pad * pad = &kit->pads[padId];
        pad->volume = value;
        pad->dirtyModified = true; // modified (instant-apply, no reload needed)
        auto & cbs = sampler_get_platform_callbacks();
        if (cbs.player.setVolume) cbs.player.setVolume(padId, value);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // pan - visible is -64 to +63, internal is 0-127
    lv_obj_t * labelPan = lv_label_create(cont);
    lv_obj_align_to(labelPan, labelVolume, LV_ALIGN_OUT_BOTTOM_LEFT, 0, entryYoffset);
    panSpinbox = lv_CreateSpinnboxParam(labelPan, (char *)"Pan: ", 2, &s_Pan, -64, 63);
    // Additional logic for pan (applied on value change via subject)
    lv_obj_add_event_cb(panSpinbox, [](lv_event_t * e) {
        int value = lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        LV_LOG_INFO("Pan changed to: %d", value);
        if (dontUpdateSettings) return;
        KitInfo * kit = getCurrentKit();
        if (kit == NULL) return;
        int padId = lv_subject_get_int(&s_pad_sampler) - 1;
        Pad * pad = &kit->pads[padId];
        pad->pan = value;
        pad->dirtyModified = true; // modified (instant-apply, no reload needed)
        auto & cbs = sampler_get_platform_callbacks();
        if (cbs.player.setPan) cbs.player.setPan(padId, pad->pan);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Playback mode label + dropdown
    lv_obj_t * modeLabel = lv_label_create(cont);
    lv_obj_align_to(modeLabel, labelPan, LV_ALIGN_OUT_BOTTOM_LEFT, 0, entryYoffset);
    lv_label_set_text(modeLabel, "Mode: ");
    lv_obj_t * modeDropdown = lv_dropdown_create(cont);
    lv_dropdown_set_options(modeDropdown, "One Shot\nHold\nLoop");
    lv_obj_set_width(modeDropdown, 85);
    lv_obj_add_style(modeDropdown, &styleAppContainer, 0);
    lv_obj_align_to(modeDropdown, modeLabel, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_group_add_obj(pad_editor_group, modeDropdown);
    lv_obj_add_event_cb(modeDropdown, [](lv_event_t * e){
        if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
        if (dontUpdateSettings) return;
        KitInfo * kit = getCurrentKit();
        if (!kit) return;
        int padId = lv_subject_get_int(&s_pad_sampler) - 1;
        if (padId < 0 || padId >= 16) return;
        uint16_t sel = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
        kit->pads[padId].playbackMode = sel; // 0=one shot,1=hold,2=loop
        kit->pads[padId].dirty = true; // needs reload
        kit->pads[padId].dirtyModified = true; // modified
        // Apply loop setting immediately for better UX
        auto & cbs = sampler_get_platform_callbacks();
        if (sel == 2) { // Loop mode
            if (cbs.player.setLoopEnd) cbs.player.setLoopEnd(padId, SAMPLE_END_MAX);
        } else {
            if (cbs.player.clearLoop) cbs.player.clearLoop(padId);
        }
        LV_LOG_INFO("Pad %d playbackMode set to %u", padId, sel);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Pad start offset, exact values are set when pad is selected/checked later
    lv_obj_t * startLabel = lv_label_create(cont);
    lv_obj_align_to(startLabel, modeLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, entryYoffset);
    lv_label_set_text(startLabel, "Start: ");
    startSpinbox = lv_CreateStandardSpinbox(lv_obj_get_parent(startLabel), 0, INT32_MAX/16,
                                 0, NULL, pad_editor_group, NULL, &lv_font_montserrat_14);
    lv_obj_align_to(startSpinbox, startLabel, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_add_event_cb(startSpinbox, [](lv_event_t * e) {
        int value = lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        LV_LOG_INFO("Start offset changed to: %d", value);
        if (dontUpdateSettings) return;
        KitInfo * kit = getCurrentKit();
        if (kit == NULL) return;
        int padId = lv_subject_get_int(&s_pad_sampler) - 1;
        Pad * pad = &kit->pads[padId];
        if (!pad->GetSampleCount()) return;
        SampleSlot * sample = &pad->samples[0]; // only one sample supported
        sample->start = value;
        pad->dirty = true; // needs reload
        pad->dirtyModified = true; // modified
        // Reload now so pad-press playback (NoteOn) reflects the trim immediately,
        // instead of only picking it up on the next kit save/exit.
        auto & cbs = sampler_get_platform_callbacks();
        if (cbs.audio.reloadPad) cbs.audio.reloadPad(padId);
        // Update waveform markers immediately
        UpdateWaveformMarkers();
    }, LV_EVENT_VALUE_CHANGED, NULL);
    lv_spinbox_set_step(startSpinbox, stepMultipliers[stepMultiplierIndex]); // Initial step

    // Pad end offset
    lv_obj_t * endLabel = lv_label_create(cont);
    lv_obj_align_to(endLabel, startLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, entryYoffset);
    lv_label_set_text(endLabel, "End: ");
    endSpinbox = lv_CreateStandardSpinbox(lv_obj_get_parent(endLabel), 0, INT32_MAX/16,
                                 0, NULL, pad_editor_group, NULL, &lv_font_montserrat_14);
    lv_obj_align_to(endSpinbox, endLabel, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_add_event_cb(endSpinbox, [](lv_event_t * e) {
        int value = lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        LV_LOG_USER("End offset changed to: %d", value);
        if (dontUpdateSettings) return;
        KitInfo * kit = getCurrentKit();
        if (kit == NULL) return;
        int padId = lv_subject_get_int(&s_pad_sampler) - 1;
        Pad * pad = &kit->pads[padId];
        if (!pad->GetSampleCount()) return;
        SampleSlot * sample = &pad->samples[0]; // only one sample supported
        if (value <= (int)sample->start) return; // ensure end > start
        sample->end = value;
        pad->dirty = true; // needs reload
        pad->dirtyModified = true; // modified
        // Reload now so pad-press playback (NoteOn) reflects the trim immediately,
        // instead of only picking it up on the next kit save/exit.
        auto & cbs = sampler_get_platform_callbacks();
        if (cbs.audio.reloadPad) cbs.audio.reloadPad(padId);
        // Update waveform markers immediately
        UpdateWaveformMarkers();
    }, LV_EVENT_VALUE_CHANGED, NULL);
    lv_spinbox_set_step(endSpinbox, stepMultipliers[stepMultiplierIndex]); // Initial step

    // Step multiplier selector (below End spinbox) - styled like spinbox
    lv_obj_t * stepLabel = lv_label_create(cont);
    lv_obj_align_to(stepLabel, endLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, entryYoffset);
    lv_label_set_text(stepLabel, "Step:");

    // Create button styled like spinbox to cycle through multipliers
    stepMultiplierBtn = lv_button_create(cont);
    lv_obj_align_to(stepMultiplierBtn, stepLabel, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_add_style(stepMultiplierBtn, &styleSpinbox, 0);
    lv_obj_add_style(stepMultiplierBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_group_add_obj(pad_editor_group, stepMultiplierBtn);

    // Label inside button showing multiplier value
    lv_obj_t * stepValueLabel = lv_label_create(stepMultiplierBtn);
    lv_label_set_text_fmt(stepValueLabel, "x%d", stepMultipliers[stepMultiplierIndex]);
    lv_obj_center(stepValueLabel);
    lv_obj_set_style_text_font(stepValueLabel, &lv_font_montserrat_14, 0);

    // Calculate width based on digits (similar to spinbox logic)
    uint8_t digits = 4; // "x1000" = 5 chars but we use 4 for padding
    lv_obj_set_width(stepMultiplierBtn, 5 + (8 * digits));

    // Callback to cycle multiplier and update spinbox steps
    lv_obj_add_event_cb(stepMultiplierBtn, [](lv_event_t * e) {
        // Cycle to next multiplier
        stepMultiplierIndex = (stepMultiplierIndex + 1) % stepMultipliersCount;
        int newStep = stepMultipliers[stepMultiplierIndex];

        // Update label inside button
        lv_obj_t * btn = (lv_obj_t*)lv_event_get_target(e);
        lv_obj_t * label = lv_obj_get_child(btn, 0);
        if (label) {
            lv_label_set_text_fmt(label, "x%d", newStep);
        }

        // Update both spinboxes
        if (startSpinbox) lv_spinbox_set_step(startSpinbox, newStep);
        if (endSpinbox) lv_spinbox_set_step(endSpinbox, newStep);

        LV_LOG_INFO("Step multiplier changed to x%d", newStep);
    }, LV_EVENT_CLICKED, NULL);

    return cont;
}


// load button callback from file browser, it closes it and loads the sample to the selected pad
void Load_button_cb(lv_event_t * e) {

    LV_LOG_WARN("Load button pressed, closing file browser");
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_delete_async(lv_obj_get_parent(obj));
    lv_indev_set_group(getEncoder(), pad_editor_group); // switch encoder nav back

    crosspad_gui::FileItem currentFile = GetCurrentFile();
    if (currentFile.path.empty() || currentFile.name.empty() || currentFile.isFolder) {
        LV_LOG_ERROR("No file selected, closing file browser!");
        lv_timer_resume(t_padUpdate);
        return;
    }

    LV_LOG_USER("Loading sample...");
    KitInfo * kit = getCurrentKit();
    if (kit == NULL) return;
    uint8_t padId = s_pad_sampler.value.num;
    if (0 == padId) return; // no pad selected, leave default settings
    Pad * pad = &kit->pads[padId - 1];

    pad->RemoveAllSamples(); // remove all samples
    auto & cbs = sampler_get_platform_callbacks();
    if (cbs.player.wipeOut) cbs.player.wipeOut(pad->number);
    if (pad->GetSampleCount() >= maxSamples) return; // max samples reached
    if (cbs.player.getFreeSlotCount && cbs.player.getFreeSlotCount() <= 0) {
        LV_LOG_ERROR("No free sample slots available!");
        return;
    }

    pad->AddSample(currentFile.path);
    if (cbs.audio.reloadPad) cbs.audio.reloadPad(lv_subject_get_int(&s_pad_sampler)-1); // only this pad
    lastPad = NULL; // force update on next timer tick
    lv_timer_resume(t_padUpdate);
}

// open file browser to load samples, stop pad update timer to prevent conflicts
static void Browse_button_cb(lv_event_t * e) {
    LV_LOG_ERROR("Browse samples button pressed");
    lv_obj_t * parent = thisApp ? thisApp->getAppContainer() : s_padEditorCont;
    lv_CreateFileExplorer(parent, FILTER_AUDIO, DEFAULT_KIT_PATH, Load_button_cb, lv_event_get_user_data(e));
    lv_timer_pause(t_padUpdate);
}

static bool isMoving = false;
static bool areYouSure = false;
static lv_obj_t * areYouSureItem = NULL;
static lv_obj_t * adderItem = NULL;
static lv_obj_t * deleterItem = NULL;

void startMoving(lv_obj_t * list, lv_obj_t * item) {
    isMoving = true;
    // hide adder
    if (adderItem) lv_obj_set_hidden(adderItem, true);
    // show deleter
    if (deleterItem) lv_obj_set_hidden(deleterItem, false);
    // hide areYouSure
    if (areYouSureItem) lv_obj_set_hidden(areYouSureItem, true);
}

void endMoving(lv_obj_t * list) {
    isMoving = false;
    // hide deleter
    if (deleterItem) lv_obj_set_hidden(deleterItem, true);
    // show adder
    if (adderItem) lv_obj_set_hidden(adderItem, false);
    // hide areYouSure
    if (areYouSureItem) lv_obj_set_hidden(areYouSureItem, true);
    areYouSure = false; // reset the flag
}

void isMovingCb(lv_obj_t * list, lv_obj_t * item) {
    if (isMoving) {
        int * diff = (int *)lv_indev_get_user_data(getEncoder());
        int index = lv_obj_get_index(item);
        if (diff == NULL || index < 0) return;
        if (*diff < 0) {
            LV_LOG_WARN("Item moving up");
            if (index < 1) return; // can't move up the first item , 0 is background
            lv_obj_move_to_index(item, lv_obj_get_index(item) - 1);
            lastPad->SwapSamples(index, index - 1);
            int padId = lv_subject_get_int(&s_pad_sampler) - 1;
            auto & cbs = sampler_get_platform_callbacks();
            if (cbs.audio.reloadPad) cbs.audio.reloadPad(padId); // only this pads
        }
        else if (*diff > 0) {
            LV_LOG_WARN("Item moving down");
            // when moving down, user can delete the item when it reaches the last position
            if (index == lv_obj_get_child_count(list) - 3) {
                if (areYouSure) {
                    LV_LOG_WARN("Item %s deleted", lv_list_get_button_text(list, item));
                    lv_obj_delete(item);
                    if (areYouSureItem) lv_obj_set_hidden(areYouSureItem, true);
                    if(lastPad != NULL) {
                        lastPad->RemoveSample(index);
                        auto & cbs = sampler_get_platform_callbacks();
                        if (cbs.audio.reloadPad) cbs.audio.reloadPad(lv_subject_get_int(&s_pad_sampler) - 1); // only this pads
                    }
                    endMoving(list);
                    return;
                } // else
                areYouSure = true;
                lv_obj_set_hidden(areYouSureItem, false);
                lv_obj_scroll_to_view(areYouSureItem, LV_ANIM_ON);
            }
            else {
                lv_obj_move_to_index(item, lv_obj_get_index(item) + 1);
                lastPad->SwapSamples(index, index + 1);
            }
            int padId = lv_subject_get_int(&s_pad_sampler);
            auto & cbs = sampler_get_platform_callbacks();
            if (cbs.audio.reloadPad) cbs.audio.reloadPad(padId - 1); // only this pads
        }
        else LV_LOG_INFO("No movement");
        lv_obj_scroll_to_view(item, LV_ANIM_ON);
        *diff = 0;
    }
}

// deleter logic handling, showing, hiding etc.
void change_item_cb(lv_event_t * e) {
    lv_obj_t * list = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t * item = (lv_obj_t *)lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    /*
    case LV_EVENT_LONG_PRESSED:
    if (isMoving) return; // already moving
        LV_LOG_WARN("Item long pressed, moving around enabled");
        startMoving(list, item);
        break;
    case LV_EVENT_VALUE_CHANGED:
        LV_LOG_WARN("Item %s selected", lv_list_get_button_text(list, item));
        break;
    case LV_EVENT_LONG_PRESSED_REPEAT:
        isMovingCb(list, item);
        break;
    case LV_EVENT_RELEASED:
        LV_LOG_INFO("Item released");
        endMoving(list);
        break;
    */
    case LV_EVENT_CLICKED: {
        LV_LOG_INFO("Item clicked");
        if (isMoving) break; // already moving
        // get sample path play it
        KitInfo * kit = getCurrentKit();
        if (kit != NULL) {
            LV_LOG_TRACE("Button: %s", lv_list_get_button_text(list, item));
            auto & cbs = sampler_get_platform_callbacks();
            std::string resolved = crosspad::resolveKitSamplePath(kit->path, lv_list_get_button_text(list, item));
            const char * fullPath = resolved.empty() ? nullptr : resolved.c_str();
            if (fullPath) {
                LV_LOG_INFO("Playing sample: %s", fullPath);
                Pad * pad = &kit->pads[lv_subject_get_int(&s_pad_sampler) - 1];

                if (cbs.player.previewSample) {
                    cbs.player.previewSample(fullPath, pad->samples[0].start, pad->samples[0].end,
                                             pad->volume, pad->pan);
                }

                // Track playback for waveform playhead
                waveformStartTime = portable_millis();
                waveformPlaying = true;
            }
        }
        break;
    }
    default:
        break;
    }
}

// Show selection on the physical pad LEDs: selected pad full color, others dim.
static void applyPadEditorLeds(int selectedIdx) {
    KitInfo * kit = getCurrentKit();
    if (!kit) return;
    auto & pm = crosspad::getPadManager();
    for (int i = 0; i < 16; i++) {
        uint32_t c = kit->pads[i].color;
        if (i == selectedIdx) {
            pm.setPadColor((uint8_t)i, crosspad::HtmlColor(c));
        } else {
            uint8_t r = ((c >> 16) & 0xFF) / 16;
            uint8_t g = ((c >> 8)  & 0xFF) / 16;
            uint8_t b = ( c        & 0xFF) / 16;
            pm.setPadColor((uint8_t)i, r, g, b);
        }
    }
}

// update the pad editor variables
static void UpdatePadEditor(lv_timer_t * e) {
    LV_LOG_INFO("Update pad editor...");

    // get the pad from current kit and update the settings only if the pad has changed
    KitInfo * kit = getCurrentKit();
    if (kit == NULL) return;
    uint8_t padId = s_pad_sampler.value.num;
    if (0 == padId) return; // no pad selected, leave default settings
    Pad * pad = &kit->pads[(padId - 1)%16];

    // Duplicate mode: intercept pad change as target selection
    if (g_duplicateMode && g_duplicateSourcePadIdx >= 0 && pad != lastPad) {
        int targetPadIdx = (padId - 1) % 16;
        if (targetPadIdx != g_duplicateSourcePadIdx) {
            ShowDuplicateConfirmation(g_duplicateSourcePadIdx, targetPadIdx);
            lastPad = pad; // prevent re-triggering
            return;
        }
    }

    if (pad == lastPad) return;
    lastPad = pad;

    // Reflect selected pad on physical LEDs (full color), dim the rest.
    applyPadEditorLeds((padId - 1) % 16);

    dontUpdateSettings = true;

    // update the pad number <- this is already updated in sampler panel
    // and is always accessible if this app is running

    // update the settings window

    // color ?????????
    // lv_subject_set_int(&s_Color, 0x00FF00);

    // set playback mode dropdown selection
    // Find mode dropdown: it's first child elements sequence; simplest: iterate children to find label "Mode: " then next sibling dropdown
    lv_obj_t * cont = lv_obj_get_parent(chokeSpinbox); // settings container
    if (cont) {
        uint32_t childCnt = lv_obj_get_child_cnt(cont);
        for (uint32_t i = 0; i < childCnt; ++i) {
            lv_obj_t * child = lv_obj_get_child(cont, i);
            if (child && lv_obj_check_type(child, &lv_label_class)) {
                const char * txt = lv_label_get_text(child);
                if (txt && strcmp(txt, "Mode: ") == 0 && (i + 1) < childCnt) {
                    lv_obj_t * dd = lv_obj_get_child(cont, i + 1);
                    if (dd && lv_obj_check_type(dd, &lv_dropdown_class)) {
                        lv_dropdown_set_selected(dd, pad->playbackMode);
                    }
                    break;
                }
            }
        }
    }

    // choke group
    lv_spinbox_set_value(chokeSpinbox, pad->choke_group);

    // volume
    lv_spinbox_set_value(volumeSpinbox, pad->volume);

    // pan
    lv_spinbox_set_value(panSpinbox, pad->pan);
    // start offset
    if (startSpinbox) lv_spinbox_set_value(startSpinbox, pad->GetSampleCount() ? pad->samples[0].start : 0);
    // end offset
    if (endSpinbox) lv_spinbox_set_value(endSpinbox, pad->GetSampleCount() ? pad->samples[0].end : 0);

    LV_LOG_INFO("Pad editor updated, updating file list");

    // update the file list
    lv_obj_t * list = (lv_obj_t *)lv_timer_get_user_data(e);
    lv_list_cleanup(list); // remove all items from the list
    // add the samples
    for (int i = 0; i < pad->GetSampleCount(); i++) {
        lv_obj_t * item = lv_list_add_button(list, LV_SYMBOL_AUDIO, pad->samples[i].sample.c_str());
        lv_obj_add_style(item, &styleFileExplorerEntry, 0);
        lv_obj_add_event_cb(item, change_item_cb, LV_EVENT_ALL, list);
        lv_group_add_obj(pad_editor_group, item); // samples group
        lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
    }


    // we are adding dummy item to tell user to move futher to delete
    areYouSureItem = lv_list_add_button(list, LV_SYMBOL_WARNING, "Move down to delete");
    lv_obj_add_style(areYouSureItem, &styleFileExplorerEntry, 0);
    lv_obj_set_style_bg_color(areYouSureItem, lv_color_hex(0x8F8F20), 0);
    lv_obj_add_event_cb(areYouSureItem, [](lv_event_t * e) {
        LV_LOG_WARN("Move futher down to delete...");
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(areYouSureItem, true); // hidden by default

    // add the dummy "remove sample" button
    deleterItem = lv_list_add_button(list, LV_SYMBOL_TRASH, "Remover");
    lv_obj_add_style(deleterItem, &styleFileExplorerEntry, 0);
    lv_obj_add_event_cb(deleterItem, [](lv_event_t * e) {
        LV_LOG_WARN("Remove sample should't be used like that");
        lastPad = NULL; // force update on next timer tick
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(deleterItem, true); // hidden by default

    /* add the "add sample" button
    adderItem = lv_list_add_button(list, LV_SYMBOL_PLUS, "Sample");
    lv_obj_add_style(adderItem, &styleFileExplorerEntry, 0);
    lv_obj_add_event_cb(adderItem, Browse_button_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(pad_editor_group, adderItem); // samples group
    lv_obj_set_hidden(adderItem, pad->GetSampleCount() >= maxSamples);
    lv_obj_add_flag(adderItem, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_bg_color(adderItem, lv_color_hex(0x008F00), LV_STATE_FOCUSED);
    */
    LV_LOG_INFO("File list updated");

    // Update waveform display for the first sample
    if (waveformView && pad->GetSampleCount() > 0) {
        KitInfo * kitForWf = getCurrentKit();
        std::string resolvedWf = kitForWf ? crosspad::resolveKitSamplePath(kitForWf->path, pad->samples[0].sample) : "";
        const char * fullPath = resolvedWf.empty() ? nullptr : resolvedWf.c_str();
        if (fullPath) {
            bool pathChanged = strcmp(currentWaveformPath, fullPath) != 0;
            strncpy(currentWaveformPath, fullPath, sizeof(currentWaveformPath) - 1);
            currentWaveformPath[sizeof(currentWaveformPath) - 1] = '\0';
            waveform_view_load(waveformView, (padId - 1) % 16, 0);

            // Use the waveform-side getFileInfo callback (separate fopen,
            // no contention with SampleStreamPlayer's streaming handle).
            if (pathChanged || !waveformInfoValid) {
                auto& wf = sampler_get_platform_callbacks().waveform;
                AudioFileInfo info = {};
                if (wf.getFileInfo && wf.getFileInfo(fullPath, &info) && info.sampleCount > 0) {
                    waveformSampleInfo.sampleCount      = info.sampleCount;
                    waveformSampleInfo.sampleRate       = info.sampleRate;
                    waveformSampleInfo.numberOfChannels = info.channels;
                    waveformSampleInfo.bitsPerSample    = info.bitDepth;
                    waveformInfoValid = true;
                } else {
                    waveformInfoValid = false;
                }
            }

            if (waveformInfoValid && waveformSampleInfo.sampleCount > 0) {
                uint32_t startS = pad->samples[0].start;
                uint32_t endS = pad->samples[0].end;
                uint32_t totalSamples = waveformSampleInfo.sampleCount;

                if (endS == 0 || endS > totalSamples) endS = totalSamples;

                float startPos = totalSamples > 0 ? (float)startS / (float)totalSamples : 0.0f;
                float endPos = totalSamples > 0 ? (float)endS / (float)totalSamples : 1.0f;

                waveform_view_set_markers(waveformView, startPos, endPos);
            }
        }
    }

    dontUpdateSettings = false;

    // Update playhead position if playing
    if (waveformView && waveformPlaying) {
        uint32_t now = portable_millis();
        float progress = 0.0f;

        if (waveformInfoValid && waveformSampleInfo.sampleRate > 0 && pad->GetSampleCount() > 0) {
            uint32_t startS = pad->samples[0].start;
            uint32_t endS = pad->samples[0].end;
            uint32_t totalSamples = 0;

            if (endS > startS) totalSamples = endS - startS;
            else totalSamples = waveformSampleInfo.sampleCount;

            if (totalSamples > 0) {
                float totalMs = (float)totalSamples * 1000.0f / (float)waveformSampleInfo.sampleRate;
                uint32_t elapsedMs = now - waveformStartTime;
                progress = totalMs > 1.0f ? (float)elapsedMs / totalMs : 0.0f;
                if (progress < 0.0f) progress = 0.0f;
                if (progress > 1.0f) {
                    progress = 1.0f;
                    waveformPlaying = false; // Stop updating when done
                }
            }
        }

        if (waveformPlaying) {
            waveform_view_set_playhead(waveformView, progress);
        } else {
            waveform_view_set_playhead(waveformView, -1.0f); // hide playhead
        }
    }
}

// Public API wrappers matching the header
void pad_editor_destroy(lv_obj_t * editor) {
    if (editor) lv_obj_delete(editor);
}

void pad_editor_set_pad(lv_obj_t * editor, uint8_t padIdx) {
    lv_subject_set_int(&s_pad_sampler, padIdx + 1);
    lastPad = NULL; // force UI refresh
}

} // namespace crosspad_sampler

#endif // USE_LVGL
