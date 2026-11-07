// SPDX-License-Identifier: GPL-3.0-or-later

#include "crosspad-sampler/ui/kit_convert_dialog.hpp"
#include "crosspad-sampler/logic/kit_convert_task.hpp"
#include "crosspad-sampler/logic/kit_load_task.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"

#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/components/ui_modal.h"
#include "crosspad-gui/components/ui_toast.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad-gui/widgets/gui_helpers.h"

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <cstdio>
#include <cstring>

namespace crosspad_sampler {

namespace {

using Phase = crosspad::KitRateConverter::Phase;

constexpr uint8_t  kTicksPerLook = 10;    // the panel ticks every 50 ms: a look every 500
constexpr uint32_t kProgressMs = 200;

lv_group_t* s_panelGroup = nullptr;
uint8_t     s_tick = 0;
char        s_decided[192] = {};   // the kit the offer was last decided for
lv_obj_t*   s_offer = nullptr;
int         s_offerKit = -1;
uint32_t    s_offerRate = 0;

struct ProgressDialog {
    lv_obj_t*   overlay = nullptr;
    lv_obj_t*   bar = nullptr;
    lv_obj_t*   msg = nullptr;
    lv_group_t* group = nullptr;
    lv_timer_t* timer = nullptr;
    uint32_t    backToken = 0;
};
ProgressDialog s_prog;

/// 48000 -> "48", 44100 -> "44.1", 22050 -> "22.05".
const char* khz(uint32_t rate, char* buf, size_t cap) {
    std::snprintf(buf, cap, "%u.%03u", static_cast<unsigned>(rate / 1000), static_cast<unsigned>(rate % 1000));
    char* end = buf + std::strlen(buf);
    while (end > buf && end[-1] == '0') *--end = '\0';
    if (end > buf && end[-1] == '.') end[-1] = '\0';
    return buf;
}

void progress_close() {
    if (!s_prog.overlay) return;
    crosspad_gui::ui_back_remove(s_prog.backToken);
    if (s_prog.timer) lv_timer_delete(s_prog.timer);
    lv_indev_t* nav = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    if (nav && s_panelGroup) lv_indev_restore_group(nav, s_panelGroup);
    if (s_prog.group) lv_group_delete(s_prog.group);
    lv_obj_delete_async(s_prog.overlay);
    s_prog = ProgressDialog();
}

void progress_finish(const KitConvertStatus& st) {
    progress_close();
    const auto& p = st.progress;
    char to[12], line[160];
    switch (p.phase) {
    case Phase::Done:
        if (p.converted == 0) {
            ui_toast_show("Nothing in this kit to convert", 2500);
        } else {
            std::snprintf(line, sizeof(line), "Kit converted to %s kHz", khz(p.toRate, to, sizeof(to)));
            ui_toast_show(line, 2500, UI_TOAST_SUCCESS);
        }
        break;
    case Phase::Cancelled:
        ui_toast_show("Stopped - the kit is unchanged", 2500, UI_TOAST_WARNING);
        break;
    default: {
        std::snprintf(line, sizeof(line), "%s.\nThe kit is unchanged; converting it again resumes here.",
                      p.error[0] ? p.error : "Something went wrong");
        static const ui_modal_button_t kOk[] = {{"OK", UI_MODAL_BUTTON_PRIMARY}};
        ui_modal_choice("Conversion failed", line, kOk, 1, 0, nullptr, nullptr, s_panelGroup);
        break;
    }
    }
}

void progress_update(lv_timer_t*) {
    const KitConvertStatus st = kit_convert_status();
    const auto& p = st.progress;
    const bool running = st.job == KitConvertJob::Convert && st.busy &&
                         (p.phase == Phase::Planning || p.phase == Phase::Converting || p.phase == Phase::Committing);
    if (!running) { progress_finish(st); return; }
    char line[96];
    if (p.phase == Phase::Planning)        std::snprintf(line, sizeof(line), "Reading the samples...");
    else if (p.phase == Phase::Committing) std::snprintf(line, sizeof(line), "Saving the kit...");
    else std::snprintf(line, sizeof(line), "%u%%  %s", static_cast<unsigned>(p.percent()), p.current);
    lv_label_set_text(s_prog.msg, line);
    lv_bar_set_value(s_prog.bar, p.percent(), LV_ANIM_OFF);
}

/// Stop, from the button or the power button's back: the dialog goes now,
/// the worker stops after the piece at hand.
void progress_stop() {
    kit_convert_cancel();
    progress_close();
    ui_toast_show("Stopping...", 1500, UI_TOAST_WARNING);
}

bool progress_back(void*) {
    progress_stop();
    return true;
}

void progress_open() {
    if (s_prog.overlay) return;
    lv_obj_t* row = nullptr;
    s_prog.overlay = ui_modal_frame(crosspad_gui::getOverlayParent(), "Converting kit",
                                    "Reading the samples...", &row, &s_prog.msg);

    /* The bar between the text and the button. */
    lv_obj_t* card = lv_obj_get_parent(row);
    s_prog.bar = lv_bar_create(card);
    lv_obj_set_size(s_prog.bar, LV_PCT(100), 8);
    lv_bar_set_range(s_prog.bar, 0, 100);
    lv_obj_set_style_bg_color(s_prog.bar, lv_color_make(45, 45, 45), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_prog.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_prog.bar, CROSSPAD_THEME_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(s_prog.bar, 0, 0);
    lv_obj_move_to_index(s_prog.bar, lv_obj_get_index(row));

    lv_obj_t* stop = ui_modal_button(row, "Stop", UI_MODAL_BUTTON_NORMAL);
    lv_obj_add_event_cb(stop, [](lv_event_t*) { progress_stop(); }, LV_EVENT_CLICKED, nullptr);

    s_prog.group = lv_group_create();
    lv_group_add_obj(s_prog.group, stop);
    if (lv_indev_t* nav = crosspad_gui::getGuiPlatform().getNavigationEncoder()) lv_indev_set_group(nav, s_prog.group);
    s_prog.backToken = crosspad_gui::ui_back_push(progress_back, nullptr);
    s_prog.timer = lv_timer_create(progress_update, kProgressMs, nullptr);
    progress_update(nullptr);
}

void offer_choice(int index, void*) {
    s_offer = nullptr;
    char from[12], line[96];
    if (index == 0) {
        if (kit_convert_start(s_offerKit)) {
            /* Not from inside the dialog's callback: it gives the encoder back
             * to the panel after this returns, which would take it from the
             * progress dialog. */
            lv_async_call([](void*) { progress_open(); }, nullptr);
        } else {
            ui_toast_show("Cannot convert now - try again", 2500, UI_TOAST_WARNING);
        }
    } else if (index == 2) {
        if (kit_convert_keep_rate(s_offerKit, true)) {
            std::snprintf(line, sizeof(line), "This kit stays at %s kHz", khz(s_offerRate, from, sizeof(from)));
            ui_toast_show(line, 2500);
        }
    }
}

void offer_open(int kitId, const KitRateReport& r) {
    char from[12], to[12], title[32], msg[200];
    khz(r.rate, from, sizeof(from));
    khz(r.engineRate, to, sizeof(to));
    std::snprintf(title, sizeof(title), "%s kHz kit", from);
    if (r.offRate >= r.samples) {
        std::snprintf(msg, sizeof(msg),
                      "Its samples are %s kHz and the board plays %s kHz, so every voice is resampled live. "
                      "Convert the kit to %s kHz for good? It plays lighter.", from, to, to);
    } else {
        std::snprintf(msg, sizeof(msg),
                      "%u of its %u samples are %s kHz and the board plays %s kHz, so their voices are "
                      "resampled live. Convert them to %s kHz for good? They play lighter.",
                      static_cast<unsigned>(r.offRate), static_cast<unsigned>(r.samples), from, to, to);
    }
    static const ui_modal_button_t kButtons[] = {
        {"Convert", UI_MODAL_BUTTON_PRIMARY},
        {"Not now", UI_MODAL_BUTTON_NORMAL},
        {"Never", UI_MODAL_BUTTON_NORMAL},
    };
    s_offerKit = kitId;
    s_offerRate = r.rate;
    s_offer = ui_modal_choice(title, msg, kButtons, 3, 0, offer_choice, nullptr, s_panelGroup);
}

} // namespace

void kit_convert_dialog_tick(lv_group_t* panelGroup) {
    s_panelGroup = panelGroup;
    if (++s_tick < kTicksPerLook) return;
    s_tick = 0;
    /* An offer is about the kit that was loaded when it went up: once
     * another kit is, or a conversion runs, it goes (as "Not now"). Left up
     * through a kit change, Convert would convert the kit before. */
    if (s_offer) {
        auto* km = crosspad::getKitManager();
        if (kit_convert_busy() || !km || km->getCurrentKitId() != s_offerKit) {
            ui_modal_confirm_dismiss(s_offer, false);
            s_offer = nullptr;
        }
        return;
    }
    if (s_prog.overlay) return;
    // Only over the panel itself: the pad editor, a dialog or the kit
    // selector on top have the encoder, and would lose it to this.
    lv_indev_t* nav = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    if (!panelGroup || (nav && lv_indev_get_group(nav) != panelGroup)) return;

    const KitConvertStatus st = kit_convert_status();
    if (st.busy && st.job == KitConvertJob::Convert &&
        (st.progress.phase == Phase::Planning || st.progress.phase == Phase::Converting)) {
        progress_open();                      // whoever started it: the CDC verb, or this panel before
        return;
    }
    if (st.busy || kit_load_task_is_loading()) return;

    auto* km = crosspad::getKitManager();
    crosspad::KitInfo* kit = km ? km->getCurrentKit() : nullptr;
    if (!kit || kit->engine != crosspad::KitEngine::Sampler || kit->path.empty()) return;
    if (std::strcmp(s_decided, kit->path.c_str()) == 0) return;

    auto& cb = sampler_get_platform_callbacks();
    KitRateReport r;
    if (!cb.audio.kitRateReport || !cb.audio.kitRateReport(&r)) return;   // not yet: look again
    std::snprintf(s_decided, sizeof(s_decided), "%s", kit->path.c_str());
    if (!cb.audio.convertHost) return;
    if (!r.offRate) {
        /* A converted kit: what its last tidy could not remove yet (a voice
         * still read it, or the power went) goes now. The worker finds out
         * from the card; with nothing listed it is one stat. */
        char folder[crosspad::kRateFolderMax];
        const size_t n = std::strlen(crosspad::rateFolderName(r.engineRate, folder));
        for (const auto& pad : kit->pads) {
            for (const auto& smp : pad.samples) {
                if (smp.sample.compare(0, n, folder) != 0 || smp.sample.size() <= n || smp.sample[n] != '/') continue;
                kit_convert_tidy(km->getCurrentKitId());
                return;
            }
        }
        return;
    }
    if (!r.engineRate || kit->keepRate) return;
    offer_open(km->getCurrentKitId(), r);
}

void kit_convert_dialog_close() {
    if (s_offer) ui_modal_confirm_dismiss(s_offer, false);   // as "Not now"
    s_offer = nullptr;
    progress_close();
    s_panelGroup = nullptr;
}

} // namespace crosspad_sampler
