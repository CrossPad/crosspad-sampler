// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

#include <crosspad-sampler/ui/kit_selector.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad-gui/crosspad_gui.h>

namespace crosspad_sampler {

// ── Static state ─────────────────────────────────────────────────────────────

static KitSelectedFn s_onSelected  = nullptr;
static KitLoadedFn   s_onLoaded    = nullptr;
static void*         s_userData    = nullptr;
static bool          s_showCacheDots = true;

static lv_group_t*   s_mainGroup   = nullptr;
static lv_group_t*   s_listGroup   = nullptr;
static lv_obj_t*     s_loadBtn     = nullptr;
static lv_obj_t*     s_overlay     = nullptr;
static int           s_selectedKitId = -1;
static lv_obj_t*     s_lastFocused = nullptr;
static bool          s_loadingStarted = false;
static lv_timer_t*   s_loadPollTimer = nullptr;

static lv_indev_t* getEncoder() {
    return crosspad_gui::getGuiPlatform().getNavigationEncoder();
}

// ── Internal callbacks ───────────────────────────────────────────────────────

static void on_back_clicked(lv_event_t* e) {
    (void)e;
    lv_obj_t* overlay = s_overlay;
    if (overlay) kit_selector_destroy(overlay);
}

static void on_list_item_clicked(lv_event_t* e) {
    lv_obj_t* list = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target_obj(e);
    const char* kitName = lv_list_get_button_text(list, obj);

    if (s_lastFocused && s_lastFocused != obj && lv_obj_is_valid(s_lastFocused)) {
        lv_obj_set_state(s_lastFocused, LV_STATE_PRESSED, false);
    }

    bool confirmed = (s_lastFocused == obj);
    s_lastFocused = obj;

    // Find kit ID by name
    auto* mgr = crosspad::getKitManager();
    if (mgr) {
        for (int i = 0; i < mgr->getKitCount(); i++) {
            auto* kit = mgr->getKit(i);
            if (kit && kit->name == kitName) {
                s_selectedKitId = i;
                break;
            }
        }
    }

    // Encoder click = immediate confirm
    if (getEncoder() && lv_indev_active() == getEncoder()) {
        if (s_loadBtn) lv_obj_send_event(s_loadBtn, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_set_state(obj, LV_STATE_PRESSED, true);
        if (s_listGroup) {
            lv_indev_t* enc = getEncoder();
            if (enc) lv_indev_set_group(enc, s_listGroup);
        }
        lv_group_focus_obj(obj);
        if (confirmed && s_loadBtn) {
            lv_obj_send_event(s_loadBtn, LV_EVENT_CLICKED, nullptr);
        }
    }
}

static void on_load_clicked(lv_event_t* e) {
    (void)e;
    if (s_selectedKitId < 0) return;

    // Notify platform (sequencer stop etc happens in the callback)
    if (s_onSelected) {
        s_onSelected(s_selectedKitId, s_userData);
    }

    // Start loading via platform callback
    const auto& cb = sampler_get_platform_callbacks();
    if (cb.kit.startLoadKit) {
        cb.kit.startLoadKit(s_selectedKitId, false);
        s_loadingStarted = true;
        if (s_loadBtn) lv_obj_set_state(s_loadBtn, LV_STATE_PRESSED, true);
    }
}

static void load_poll_timer_cb(lv_timer_t* timer) {
    (void)timer;
    if (!s_loadingStarted) return;

    auto* mgr = crosspad::getKitManager();
    if (mgr && !mgr->isLoading()) {
        s_loadingStarted = false;
        // Notify caller — caller is responsible for destroying the overlay
        // (e.g. via thisApp->destroyApp() which calls kit_selector_destroy)
        if (s_onLoaded) {
            s_onLoaded(s_userData);
        }
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

lv_obj_t* kit_selector_create(const KitSelectorConfig& config) {
    s_onSelected = config.onSelected;
    s_onLoaded = config.onLoaded;
    s_userData = config.userData;
    s_showCacheDots = config.showCacheDots;
    s_selectedKitId = -1;
    s_lastFocused = nullptr;
    s_loadingStarted = false;

    lv_obj_t* parent = config.parent;

    // Main container
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_obj_get_width(parent), lv_obj_get_height(parent));
    lv_obj_center(cont);
    lv_obj_add_style(cont, &styleAppContainer, 0);
    s_overlay = cont;

    // Title
    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text(title, "Choose kit");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    // Groups
    s_mainGroup = lv_group_create();
    s_listGroup = lv_group_create();

    lv_indev_t* enc = getEncoder();
    if (enc) lv_indev_set_group(enc, s_listGroup);

    // Back button
    lv_obj_t* backBtn = lv_CreateFooterButton(cont, "Back", -50, on_back_clicked);
    lv_group_add_obj(s_mainGroup, backBtn);

    // Load button
    s_loadBtn = lv_CreateFooterButton(cont, "Load", 50, on_load_clicked);
    lv_group_add_obj(s_mainGroup, s_loadBtn);

    // Kit list
    kit_selector_refresh(cont);

    // Poll timer for load completion
    s_loadPollTimer = lv_timer_create(load_poll_timer_cb, 100, nullptr);

    return cont;
}

void kit_selector_destroy(lv_obj_t* selector) {
    s_selectedKitId = -1;
    s_lastFocused = nullptr;
    s_loadBtn = nullptr;
    s_loadingStarted = false;

    if (s_loadPollTimer) {
        lv_timer_delete(s_loadPollTimer);
        s_loadPollTimer = nullptr;
    }

    // Detach encoder from our groups before deleting them
    lv_indev_t* enc = getEncoder();
    if (enc) {
        lv_group_t* cur = lv_indev_get_group(enc);
        if (cur == s_mainGroup || cur == s_listGroup) {
            lv_indev_set_group(enc, nullptr);
        }
    }

    if (s_mainGroup) {
        lv_group_delete(s_mainGroup);
        s_mainGroup = nullptr;
    }
    if (s_listGroup) {
        lv_group_delete(s_listGroup);
        s_listGroup = nullptr;
    }

    s_overlay = nullptr;
    lv_obj_delete_async(selector);
}

void kit_selector_refresh(lv_obj_t* selector) {
    static const int LIST_TAG = 0x4B4C; // "KL"

    lv_obj_t* list = nullptr;
    uint32_t childCount = lv_obj_get_child_count(selector);
    for (uint32_t i = 0; i < childCount; i++) {
        lv_obj_t* child = lv_obj_get_child(selector, (int32_t)i);
        if (lv_obj_get_user_data(child) == (void*)(intptr_t)LIST_TAG) {
            list = child;
            uint32_t listChildren = lv_obj_get_child_count(list);
            for (uint32_t j = listChildren; j > 0; --j) {
                lv_obj_delete(lv_obj_get_child(list, (int32_t)(j - 1)));
            }
            break;
        }
    }

    if (!list) {
        list = lv_list_create(selector);
        lv_obj_set_size(list, 230, 130);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 30);
        lv_obj_add_style(list, &styleAppContainer, 0);
        lv_obj_add_style(list, &styleKitListBox, LV_PART_MAIN);
        lv_obj_add_style(list, &styleFileExplorerScrollbar, LV_PART_SCROLLBAR);
        lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(list, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        // Focused border
        lv_obj_set_style_border_color(list, lv_color_white(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(list, 1, LV_STATE_FOCUSED);
        lv_obj_set_user_data(list, (void*)(intptr_t)LIST_TAG);

        if (s_mainGroup) lv_group_add_obj(s_mainGroup, list);
        lv_obj_add_event_cb(list, [](lv_event_t* e) {
            (void)e;
            lv_indev_t* enc = getEncoder();
            if (enc && s_listGroup) lv_indev_set_group(enc, s_listGroup);
        }, LV_EVENT_CLICKED, nullptr);
    }

    auto* mgr = crosspad::getKitManager();
    if (!mgr) return;

    int kitCount = mgr->getKitCount();
    int currentKitId = mgr->getCurrentKitId();
    const auto& cb = sampler_get_platform_callbacks();

    for (int i = 0; i < kitCount; i++) {
        auto* kit = mgr->getKit(i);
        if (!kit) continue;

        lv_obj_t* btn = lv_list_add_button(list, LV_SYMBOL_FILE, kit->name.c_str());
        lv_obj_add_style(btn, &styleKitListItem, 0);
        lv_obj_add_style(btn, &styleKitListItemFocused, LV_STATE_FOCUSED | LV_PART_SELECTED);
        lv_obj_add_style(btn, &styleKitListItemSelected, LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, on_list_item_clicked, LV_EVENT_CLICKED, list);
        if (s_listGroup) lv_group_add_obj(s_listGroup, btn);

        // Highlight loaded kit
        if (i == currentKitId) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x08F020), 0);
        }

        // Cache status dot
        if (s_showCacheDots && cb.kit.hasCacheFile) {
            bool cached = cb.kit.hasCacheFile(i);
            lv_obj_t* label = lv_obj_get_child(btn, 1);
            if (label) lv_obj_set_flex_grow(label, 1);

            lv_obj_t* dot = lv_obj_create(btn);
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, cached ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF0000), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_pad_all(dot, 0, 0);
            lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        }
    }
}

void kit_selector_set_callback(KitSelectedFn fn, void* userData) {
    s_onSelected = fn;
    s_userData = userData;
}

} // namespace crosspad_sampler

#endif // USE_LVGL
