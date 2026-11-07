// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef USE_LVGL

#include <crosspad-sampler/logic/kit_gate.hpp>
#include <crosspad-sampler/logic/kit_load_task.hpp>
#include <crosspad-sampler/ui/kit_selector.hpp>
#include <crosspad-sampler/ui/kit_selector_app.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/IKitPicker.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/song/ArrangeFocus.hpp>
#include <crosspad-gui/components/app_orchestrator.h>
#include <crosspad-gui/components/mode_overlay.h>
#include <crosspad-gui/components/status_bar.h>
#include <crosspad-gui/components/ui_toast.h>
// The App class differs by platform but not by shape: the PC one derives from
// crosspad_gui::AppBase<App> just as the device one does.
#if __has_include("pc_stubs/PcApp.hpp")
  #include "pc_stubs/PcApp.hpp"
#else
  #include <crosspad-gui/app/App.hpp>
#endif

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace crosspad_sampler {

namespace {

KitGateHost s_host;

App * s_overlay = nullptr;
App * s_pendingLaunch = nullptr;
bool  s_shouldLaunch = false;

const char* kKitRequiredApps[] = {"Sampler", "Sequencer"};

lv_obj_t* app_container() { return s_host.appContainer ? s_host.appContainer() : nullptr; }

void gate_log(const char* why) {
    if (!s_host.log) return;
    char line[48];
    snprintf(line, sizeof(line), "kit gate: %s", why);
    s_host.log(line);
}

bool app_requires_kit(const char* app_name) {
    /* A seq track that plays through Pitch Play needs no kit: the sequencer
     * borrows the pitched engine for it instead. Without this, choosing
     * PITCH on Arrange's "plays with" card put the kit browser on screen
     * instead of the track's seq -- the gate saw the sequencer opening with
     * no kit loaded and asked for one that nothing was going to play. */
    const crosspad::ArrangeFocus& focus = crosspad::arrangeFocus();
    if (focus.held() && focus.seq && focus.source == crosspad::SeqSource::Pitch) return false;

    for (auto& name : kKitRequiredApps) {
        if (strcmp(app_name, name) == 0) return true;
    }
    return false;
}

/* ── Recent screens ──────────────────────────────────────────────────────────
 *
 * The swipe is the Android recents double-tap: go to the other of the two
 * screens you have been using, and doing it again comes straight back. A
 * "screen" here is an app or the kit selector — the selector is not an app the
 * launcher ever started, so nothing in the launcher can name it, which is why
 * this lives with the gate.
 *
 * The current screen is asked for, never tracked. The first version tracked it,
 * and returning to the launcher — which happens as a side effect of destroying
 * an app, not only when the user asks for it — overwrote the history the
 * transition had just written. That is the whole reason the gesture landed on
 * the wrong screen. */
struct ScreenTok {
    App* app = nullptr;      ///< the app, or null
    bool sel = false;        ///< the kit selector
    bool valid() const { return app != nullptr || sel; }
    bool operator==(const ScreenTok& o) const { return app == o.app && sel == o.sel; }
};

ScreenTok s_recent[2];    // newest first

bool selector_is_up() {
    return s_overlay && s_overlay->isStarted();
}

/// What is on screen right now. An invalid token means the bare launcher.
ScreenTok screen_current() {
    if (selector_is_up()) return ScreenTok{nullptr, true};
    auto* app = crosspad_gui::AppOrchestrator::getInstance().getForegroundApp();
    return ScreenTok{static_cast<App*>(app), false};
}

void screen_visited(ScreenTok t) {
    if (!t.valid() || t == s_recent[0]) return;
    s_recent[1] = s_recent[0];
    s_recent[0] = t;
}

/* Built once and reused: the app factory does not own this overlay, so
 * re-creating it per launch leaked one App per prompt. destroyApp() clears
 * `app`/`started_`, so start() rebuilds the UI on the same instance. Hand-off
 * goes through the selector's onClosed callback, not an LV_EVENT_DELETE hook
 * on a root the previous teardown already freed. */
void show_overlay(lv_obj_t* container) {
    if (!s_overlay) {
        s_overlay = new App(container, "Kit selector", s_host.selectorIcon,
                            lv_CreateKitSelector, lv_KitSelector_destroy);
    }
    s_overlay->start(container);
    s_overlay->resume();
    screen_visited(ScreenTok{nullptr, true});
}

void on_kit_loaded() {
    s_shouldLaunch = true;
    crosspad_gui::statusbar_process_loader_status(100, true, false, "Kit loaded");
}

// The selector reports here once torn down, whether a kit loaded or the user
// backed out. Only the first report launches what waited behind the gate.
void on_closed() {
    auto& orch = crosspad_gui::AppOrchestrator::getInstance();
    lv_obj_t* container = app_container();
    // The gate can be abandoned while the selector is up: APP_STOP rebuilds the
    // launcher, or another app takes the container. Launching then puts an app
    // on a screen nobody asked for it on, and leaves the harness believing it
    // destroyed what it asked to destroy.
    if (s_pendingLaunch && s_shouldLaunch && container &&
        orch.getRunningApp() == nullptr && !s_pendingLaunch->isStarted()) {
        s_pendingLaunch->start(container);
        s_pendingLaunch->resume();
        orch.setRunningApp(s_pendingLaunch);
        screen_visited(ScreenTok{s_pendingLaunch, false});
    } else if (!s_pendingLaunch && s_shouldLaunch) {
        /* A kit picked on the Kits screen itself -- MODE's KITS pad, not a
         * gate or an app's own button, which all park something above --
         * is picked to be played: the Sampler comes up with it. */
        crosspad_gui::mode_overlay_open_app("Sampler");
    }
    /* Backed out without picking a kit, and nothing waited behind the gate:
     * the MODE map comes up as home on its own once nothing is on screen. */
    s_pendingLaunch = nullptr;
    s_shouldLaunch = false;
}

/* The selector, opened from inside a running app rather than as a gate in
 * front of a launch. Same machinery: the app that was running is parked in
 * s_pendingLaunch and on_closed() brings it back, whether a kit was picked or
 * the user backed out. Until this existed the selector was reachable only
 * when no kit was loaded at all, which is why the sampler's own pick-kit
 * button had nothing to call. */
void open_selector_over(bool backReturnsToApp) {
    auto& orch = crosspad_gui::AppOrchestrator::getInstance();
    crosspad_gui::ILvglApp* running = orch.getForegroundApp();
    lv_obj_t* container = app_container();
    if (!container || !running) return;
    if (selector_is_up()) return;

    s_pendingLaunch = static_cast<App*>(running);
    /* Back returns to the app the button was pressed in — it is where the user
     * came from, and landing on the launcher instead was the one thing about
     * the browser that felt broken. Two openers must not return: the gate
     * (kit_gate_pre_launch, no kit loaded — the app would re-open the
     * selector at once) and the sampler's own back, whose selector is one level
     * up — returning there bounces sampler -> selector -> sampler and the power
     * button never reaches the launcher. */
    s_shouldLaunch = backReturnsToApp;

    running->destroyApp();
    orch.setRunningApp(nullptr);

    show_overlay(container);
}

void open_selector() { open_selector_over(true); }
void open_selector_for_back() { open_selector_over(false); }

/* The same selector for an app's own kit (core IKitPicker): over the running
 * app, which comes back when it closes, and the kit picked handed to the app
 * instead of loaded -- Arrange's kit tracks pick theirs here. */
class GateKitPicker final : public crosspad::IKitPicker {
public:
    bool open(PickedFn picked, void* ctx) override { return open(picked, ctx, crosspad::KitList::Any); }
    bool open(PickedFn picked, void* ctx, crosspad::KitList list) override {
        auto& orch = crosspad_gui::AppOrchestrator::getInstance();
        if (!picked || !app_container() || !orch.getForegroundApp() || selector_is_up()) return false;
        s_picked = picked;
        s_pickedCtx = ctx;
        kit_selector_app_pick_for(&on_pick, nullptr, list);
        open_selector();
        if (selector_is_up()) return true;
        kit_selector_app_pick_for(nullptr, nullptr);
        s_picked = nullptr;
        return false;
    }

private:
    static void on_pick(int kitId, void*) {
        crosspad::IKitManager* km = crosspad::getKitManager();
        const crosspad::KitInfo* k = km ? km->getKit(kitId) : nullptr;
        const PickedFn fn = s_picked;
        s_picked = nullptr;
        if (fn && k && !k->path.empty()) fn(k->path, s_pickedCtx);
    }

    static PickedFn s_picked;
    static void* s_pickedCtx;
};

GateKitPicker::PickedFn GateKitPicker::s_picked = nullptr;
void* GateKitPicker::s_pickedCtx = nullptr;
GateKitPicker s_kitPicker;

} // namespace

void kit_gate_install(const KitGateHost& host, KitSelectorAppCallbacks& cb) {
    s_host = host;
    cb.onKitLoaded         = on_kit_loaded;
    cb.onClosed            = on_closed;
    cb.openSelector        = open_selector;
    cb.openSelectorForBack = open_selector_for_back;
    crosspad::getPlatformServices().kitPicker = &s_kitPicker;
}

/* One rule: go to the most recent screen that is not the one showing. From the
 * launcher that is the last screen used, from a screen it is the other one —
 * and because arriving records the arrival, the next swipe comes back. */
bool kit_gate_previous_screen() {
    const ScreenTok cur = screen_current();
    ScreenTok target{};
    for (const ScreenTok& t : s_recent) {
        if (t.valid() && !(t == cur)) { target = t; break; }
    }
    lv_obj_t* container = app_container();
    if (!target.valid() || !container) return false;

    if (target.sel) {
        if (selector_is_up()) return false;
        open_selector();
        return true;
    }
    if (selector_is_up()) {
        /* kit_selector_close(), never destroyAppAsync() on the overlay. The
         * selector's onClosed hook — the only thing that brings the parked app
         * back — is raised by the selector's own close path and by nothing
         * else, so tearing the overlay down directly landed on the launcher
         * however the flags were set. */
        s_pendingLaunch = target.app;
        s_shouldLaunch = true;
        kit_selector_close();
        return true;
    }
    if (target.app->isStarted()) return false;
    crosspad_gui::AppOrchestrator::onAppSelected(target.app, container);
    return true;
}

bool kit_gate_pre_launch(crosspad_gui::ILvglApp* app, lv_obj_t* app_container) {
    /* "Kits" in the launcher is the same selector as an app of its own: it
     * needs no kit, but it lists the same vector. */
    const bool kits = strcmp(app->getName(), "KitSelector") == 0;
    if (!kits && !app_requires_kit(app->getName())) {
        return true;
    }

    auto* currentKit = crosspad::getKitManager()
                       ? crosspad::getKitManager()->getCurrentKit()
                       : nullptr;
    if (!kits && currentKit != nullptr) {
        screen_visited(ScreenTok{static_cast<App*>(app), false});
        return true;
    }

    /* The gate says why in the log (APP_LIST's running=- alone cannot). The
     * card is still being read: the list the selector would show is the
     * vector the scan is growing, so nothing opens and nothing is queued --
     * the user taps again. No card at all: the list alone would say "No kits
     * found" of a card full of kits; it opens, and a card that mounts later
     * refreshes it (kit_gate_refresh_if_open). */
    const bool scanning = s_host.kitScanDone && !s_host.kitScanDone();
    const bool noCard   = s_host.cardPresent && !s_host.cardPresent();
    const char* toast = scanning ? "Reading SD card..."
                      : noCard ? "No SD card - reinsert or restart" : nullptr;
    if (kits) {
        if (scanning) gate_log("scanning");
        if (toast) ui_toast_show(toast);
        return !scanning;
    }
    /* A kit already on its way (a pick or a KIT_LOAD still loading) is the
     * same answer as a scan. A selector that is up stays as it is: a second
     * start() of it was the "App already started" of the harness. */
    const bool loading = !scanning && kit_load_task_is_loading();
    const char* why = scanning ? "scanning" : loading ? "loading"
                    : selector_is_up() ? "selector open" : "selector";
    if (loading) toast = "Loading kit...";
    gate_log(why);
    if (toast) ui_toast_show(toast);
    if (scanning || loading || selector_is_up()) return false;

    // No kit loaded — show kit selector overlay
    s_pendingLaunch = static_cast<App*>(app);
    s_shouldLaunch = false;
    show_overlay(app_container);
    return false;
}

void kit_gate_reset() {
    s_pendingLaunch = nullptr;
    s_shouldLaunch = false;
}

/* After a kit scan (boot, or a card put back): an open selector shows what
 * was there when it was built -- "No kits found" with no card. Rebuilt here,
 * on the LVGL thread, unless a load is under way (the list then carries its
 * spinner). The selector is the gate's overlay or the "Kits" app; either way
 * its root is the container of whoever is showing it (kit_selector_refresh
 * ignores any other). */
void kit_gate_refresh_if_open(void*) {
    if (!kit_selector_is_open() || kit_load_task_is_loading()) return;
    crosspad_gui::ILvglApp* shown = selector_is_up()
        ? s_overlay
        : crosspad_gui::AppOrchestrator::getInstance().getForegroundApp();
    if (shown) kit_selector_refresh(shown->getAppContainer());
}

} // namespace crosspad_sampler

#endif // USE_LVGL
