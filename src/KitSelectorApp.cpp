// SPDX-License-Identifier: GPL-3.0-or-later
//
// Kit Selector app — wraps kit_selector UI with platform callbacks
// and LVGL app lifecycle. Registered via a named platform entry point.

#ifdef USE_LVGL

#include <crosspad-sampler/ui/kit_selector_app.hpp>
#include <crosspad-sampler/ui/kit_selector.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/waveform/waveform_loader.hpp>
#include <crosspad-sampler/waveform/waveform_cache.hpp>
#include <crosspad/app/AppRegistry.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad-gui/platform/IGuiPlatform.h>
// The App class differs by platform but not by shape: the PC one derives from
// crosspad_gui::AppBase<App> just as the device one does, so isStarted() /
// destroyApp() mean the same thing on both.
#if __has_include("pc_stubs/PcApp.hpp")
  #include "pc_stubs/PcApp.hpp"
#else
  #include <crosspad-gui/app/App.hpp>
#endif
#include <crosspad-gui/components/app_orchestrator.h>
#include <crosspad-gui/components/mode_overlay.h>

#include <lvgl.h>
#include <lvgl_private.h>   // lv_image_cache_drop, private API on LVGL master

static App* s_thisApp = nullptr;
/* A pick asked for the next selector (kit_selector_app_pick_for()). */
static void (*s_pickFn)(int, void*) = nullptr;
static void* s_pickCtx = nullptr;
static crosspad::KitList s_pickList = crosspad::KitList::Any;
static void (*s_pickingFn)(int, void*) = nullptr;   // the open selector's
static void* s_pickingCtx = nullptr;
/* Where to go once the browser is closed (KitSelectorConfig::onOpenApp). */
static const char* s_follow = nullptr;
/* Closed with back: the app it was opened from, to go back to. */
static crosspad_gui::ILvglApp* s_back = nullptr;

void kit_selector_app_pick_for(void (*fn)(int kitId, void* ctx), void* ctx, crosspad::KitList list) {
    s_pickFn = fn;
    s_pickCtx = ctx;
    s_pickList = fn ? list : crosspad::KitList::Any;
}

static void on_kit_selected(int kitId, void* /*userData*/) {
    auto& cb = crosspad_sampler::sampler_get_platform_callbacks().kitSelector;
    if (cb.onBeforeKitLoad) cb.onBeforeKitLoad(kitId);

    auto* kitMgr = crosspad::getPlatformServices().kitManager;
    if (kitMgr) {
        auto* kit = kitMgr->getKit(kitId);
        /* The loader is not stopped here any more. Its stack is 6 kB of
         * internal RAM in one piece, and by the time a kit is picked the
         * largest free internal block is often smaller than that: the task
         * died on every kit pick and the restart failed silently, so the kit
         * ended up with no waveform at all. It stays alive; the queue is
         * cleared inside clear_if_kit_changed(), which is what the stop was
         * really for. */
        crosspad_sampler::waveform_cache_clear_if_kit_changed(
            kit, kit ? kit->path.c_str() : nullptr);
    }
}

/* Both exits go through here, deferred: the caller is an LVGL callback on a
 * widget the teardown deletes. onClosed fires after destroyApp() so whoever
 * opened the selector as a gate can launch what was waiting behind it. */
static void close_app_async() {
    lv_async_call([](void*) {
        if (s_thisApp && s_thisApp->isStarted()) {
            s_thisApp->destroyApp();
        }
        /* Now, not in the teardown's own async call: onClosed starts the app
         * that waited behind the gate right here, and the covers still in the
         * image cache left the Sampler no PSRAM for its waveform. */
        lv_image_cache_drop(nullptr);
        auto& cb = crosspad_sampler::sampler_get_platform_callbacks().kitSelector;
        if (cb.onClosed) cb.onClosed();
        /* After onClosed: whatever the host brings back gives way to it. */
        const char* app = s_follow;
        crosspad_gui::ILvglApp* back = s_back;
        s_follow = nullptr;
        s_back = nullptr;
        if (app) crosspad_gui::mode_overlay_open_app(app);
        /* One step back, as the power button's click is everywhere -- unless
         * the host already brought back what the browser was opened over. */
        else if (back && !crosspad_gui::AppOrchestrator::getInstance().getForegroundApp())
            crosspad_gui::mode_overlay_open_app(back->getName());
    }, nullptr);
}

static void on_kit_loaded(void* /*userData*/) {
    auto& cb = crosspad_sampler::sampler_get_platform_callbacks().kitSelector;
    if (cb.onKitLoaded) cb.onKitLoaded();
    close_app_async();
}

static void on_kit_closed(void* /*userData*/) {
    auto& orch = crosspad_gui::AppOrchestrator::getInstance();
    if (s_thisApp && orch.getForegroundApp() == s_thisApp) s_back = orch.historyBack(s_thisApp);
    close_app_async();
}

static void on_open_app(const char* app, void* /*userData*/) {
    if (s_follow) return;   /* a second click before the close */
    s_follow = app;
    close_app_async();
}

static void on_kit_picked(int kitId, void* /*userData*/) {
    if (!s_pickingFn) return;   /* a second click before the close: picked already */
    s_pickingFn(kitId, s_pickingCtx);
    s_pickingFn = nullptr;
    close_app_async();
}

lv_obj_t* lv_CreateKitSelector(lv_obj_t* parent, App* app) {
    s_thisApp = app;

    crosspad_sampler::KitSelectorConfig config;
    config.parent        = parent;
    s_pickingFn  = s_pickFn;
    s_pickingCtx = s_pickCtx;
    s_pickFn = nullptr;
    s_pickCtx = nullptr;
    config.onPicked      = s_pickingFn ? on_kit_picked : nullptr;
    config.list          = s_pickList;
    s_pickList = crosspad::KitList::Any;
    config.onSelected    = on_kit_selected;
    config.onLoaded      = on_kit_loaded;
    config.onClose       = on_kit_closed;
    config.onOpenApp     = on_open_app;
    return crosspad_sampler::kit_selector_create(config);
}

void lv_KitSelector_destroy(lv_obj_t* app) {
    s_thisApp = nullptr;
    s_pickingFn = nullptr;
    crosspad_sampler::kit_selector_destroy(app);
}

static crosspad::AppEntry kit_selector_get_entry() {
    crosspad::AppEntry entry;
    entry.name        = "KitSelector";
    entry.icon        = "notepick.png";
    entry.createLVGL  = lv_CreateKitSelector;
    entry.destroyLVGL = lv_KitSelector_destroy;
    entry.priority    = 50;
    entry.recolorIcon = true;  // notepick.png is a black glyph
    return entry;
}

void _register_KitSelector_app() {
    crosspad::AppRegistry::getInstance().registerApp(kit_selector_get_entry());
}

#endif // USE_LVGL
