// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

#include <crosspad-sampler/ui/sampler_panel.hpp>
#include <crosspad-sampler/ui/waveform_view.hpp>
#include <crosspad-sampler/ui/kit_selector.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/pad/PadLedController.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <cstdio>
#include <memory>

namespace crosspad_sampler {

// ---------------------------------------------------------------------------
// Shared static logic instances
// ---------------------------------------------------------------------------

static SamplerPadLogic  s_padLogic;
static PadColorManager  s_colorManager;
static KitOrchestrator  s_kitOrchestrator;

SamplerPadLogic& getSamplerPadLogic()   { return s_padLogic; }
PadColorManager& getPadColorManager()   { return s_colorManager; }
KitOrchestrator& getKitOrchestrator()   { return s_kitOrchestrator; }

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------

static lv_obj_t* s_padButtons[PAD_COUNT]  = {};
static lv_obj_t* s_waveformView           = nullptr;
static lv_obj_t* s_kitNameLabel           = nullptr;
static lv_obj_t* s_padIndexLabel          = nullptr;
static lv_obj_t* s_kitSelectorOverlay     = nullptr;
static lv_timer_t* s_updateTimer          = nullptr;
static int         s_selectedPad          = 0;
static bool        s_destroying           = false;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void load_waveform_for_pad(int padIdx);

// ---------------------------------------------------------------------------
// Pad button event handlers
// ---------------------------------------------------------------------------

static void on_pad_btn_pressed(lv_event_t* e)
{
    if (s_destroying) return;

    int padIdx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    s_selectedPad = padIdx;
    s_colorManager.setSelectedPad(padIdx);

    if (s_padIndexLabel) {
        char buf[16];
        snprintf(buf, sizeof(buf), "PAD: %d", padIdx + 1);
        lv_label_set_text(s_padIndexLabel, buf);
    }

    crosspad::getPadManager().handlePadPress(static_cast<uint8_t>(padIdx));
    load_waveform_for_pad(padIdx);
}

static void on_pad_btn_released(lv_event_t* e)
{
    if (s_destroying) return;

    int padIdx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    crosspad::getPadManager().handlePadRelease(static_cast<uint8_t>(padIdx));
}

// ---------------------------------------------------------------------------
// Kit selector callback
// ---------------------------------------------------------------------------

static void on_kit_selected(int kitId, void* /*userData*/)
{
    s_kitOrchestrator.changeKit(kitId);

    // Update kit name label
    if (s_kitNameLabel) {
        crosspad::IKitManager* km = crosspad::getKitManager();
        const char* name = "No Kit";
        if (km) {
            crosspad::KitInfo* kit = km->getCurrentKit();
            if (kit) name = kit->name.c_str();
        }
        lv_label_set_text(s_kitNameLabel, name);
    }

    // Close the overlay
    if (s_kitSelectorOverlay) {
        kit_selector_destroy(s_kitSelectorOverlay);
        s_kitSelectorOverlay = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Kit button event handler
// ---------------------------------------------------------------------------

static void on_kit_btn_clicked(lv_event_t* e)
{
    if (s_destroying) return;

    lv_obj_t* panel = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    if (s_kitSelectorOverlay) {
        kit_selector_destroy(s_kitSelectorOverlay);
        s_kitSelectorOverlay = nullptr;
        return;
    }

    kit_selector_set_callback(on_kit_selected, nullptr);
    s_kitSelectorOverlay = kit_selector_create(panel);
}

// ---------------------------------------------------------------------------
// Update timer callback
// ---------------------------------------------------------------------------

static void update_timer_cb(lv_timer_t* /*timer*/)
{
    if (s_destroying) return;

    crosspad::PadManager& pm = crosspad::getPadManager();

    for (int i = 0; i < PAD_COUNT; ++i) {
        if (!s_padButtons[i]) continue;

        lv_color_t color;
        if (pm.isPadPressed(static_cast<uint8_t>(i))) {
            color = lv_color_hex(DEFAULT_PRESSED_COLOR);
        } else if (pm.isPadPlaying(static_cast<uint8_t>(i))) {
            color = lv_color_hex(DEFAULT_PLAYING_COLOR);
        } else {
            color = lv_color_hex(DEFAULT_PAD_COLOR);
        }

        lv_obj_set_style_bg_color(s_padButtons[i], color, LV_PART_MAIN);
    }
}

// ---------------------------------------------------------------------------
// Waveform helper
// ---------------------------------------------------------------------------

static void load_waveform_for_pad(int padIdx)
{
    if (!s_waveformView) return;
    waveform_view_load(s_waveformView, static_cast<uint8_t>(padIdx), 0);
}

// ---------------------------------------------------------------------------
// Pad grid builder
// ---------------------------------------------------------------------------

static lv_obj_t* create_pad_grid(lv_obj_t* parent)
{
    // 4x4 grid, each cell 40px = 160x160 total
    static lv_coord_t col_dsc[] = {40, 40, 40, 40, LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {40, 40, 40, 40, LV_GRID_TEMPLATE_LAST};

    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_size(grid, 160, 160);
    lv_obj_set_style_bg_color(grid, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(grid, 2, LV_PART_MAIN);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

    for (int i = 0; i < PAD_COUNT; ++i) {
        lv_obj_t* btn = lv_button_create(grid);
        lv_obj_set_style_bg_color(btn, lv_color_hex(DEFAULT_PAD_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

        // Number label
        lv_obj_t* lbl = lv_label_create(btn);
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", i + 1);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_center(lbl);

        // Grid placement: row-major, bottom-up (row 0 = top in LVGL)
        int col = i % PAD_GRID_COLS;
        int row = i / PAD_GRID_ROWS;
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1,
                                  LV_GRID_ALIGN_STRETCH, row, 1);

        // Events — pass pad index as user data
        void* ud = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        lv_obj_add_event_cb(btn, on_pad_btn_pressed,  LV_EVENT_PRESSED,  ud);
        lv_obj_add_event_cb(btn, on_pad_btn_released, LV_EVENT_RELEASED, ud);

        s_padButtons[i] = btn;
    }

    return grid;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

lv_obj_t* sampler_panel_create(lv_obj_t* parent)
{
    s_destroying = false;

    // Init orchestrator with color manager
    s_kitOrchestrator.init(s_colorManager);

    // Root container — full parent size, black bg, non-scrollable
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 4, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    // --- Kit name label (top left, white) ---
    s_kitNameLabel = lv_label_create(panel);
    const char* kitName = "No Kit";
    crosspad::IKitManager* km = crosspad::getKitManager();
    if (km) {
        crosspad::KitInfo* kit = km->getCurrentKit();
        if (kit) kitName = kit->name.c_str();
    }
    lv_label_set_text(s_kitNameLabel, kitName);
    lv_obj_set_style_text_color(s_kitNameLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(s_kitNameLabel, LV_ALIGN_TOP_LEFT, 0, 0);

    // --- Kit select button (top right, 40x24) ---
    lv_obj_t* kitBtn = lv_button_create(panel);
    lv_obj_set_size(kitBtn, 40, 24);
    lv_obj_align(kitBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t* kitBtnLabel = lv_label_create(kitBtn);
    lv_label_set_text(kitBtnLabel, LV_SYMBOL_LIST);
    lv_obj_center(kitBtnLabel);
    lv_obj_add_event_cb(kitBtn, on_kit_btn_clicked, LV_EVENT_CLICKED, panel);

    // --- Pad index label ("PAD: 1", below kit name) ---
    s_padIndexLabel = lv_label_create(panel);
    lv_label_set_text(s_padIndexLabel, "PAD: 1");
    lv_obj_set_style_text_color(s_padIndexLabel, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_align(s_padIndexLabel, LV_ALIGN_TOP_LEFT, 0, 20);

    // --- Pad grid (bottom left) ---
    lv_obj_t* grid = create_pad_grid(panel);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // --- Waveform view (top right area, below the kit button) ---
    s_waveformView = waveform_view_create(panel);
    lv_obj_align(s_waveformView, LV_ALIGN_TOP_RIGHT, 0, 28);

    // Load colors from current kit
    if (km) {
        crosspad::KitInfo* kit = km->getCurrentKit();
        if (kit) s_colorManager.loadColorsFromKit(*kit);
    }

    // Register and activate pad logic
    crosspad::getPadManager().registerPadLogic(
        "Sampler",
        std::make_shared<SamplerPadLogic>(s_padLogic)
    );
    crosspad::getPadManager().setActivePadLogic("Sampler");

    // Start periodic update timer
    s_updateTimer = lv_timer_create(update_timer_cb, PAD_UPDATE_INTERVAL_MS, nullptr);

    // Default selection: pad 0
    s_selectedPad = 0;
    s_colorManager.setSelectedPad(0);
    load_waveform_for_pad(0);

    return panel;
}

void sampler_panel_destroy(lv_obj_t* panel)
{
    s_destroying = true;

    // Stop timer
    if (s_updateTimer) {
        lv_timer_delete(s_updateTimer);
        s_updateTimer = nullptr;
    }

    // Close kit selector if open
    if (s_kitSelectorOverlay) {
        kit_selector_destroy(s_kitSelectorOverlay);
        s_kitSelectorOverlay = nullptr;
    }

    // Clear waveform
    if (s_waveformView) {
        waveform_view_clear(s_waveformView);
    }

    // Reset color manager
    s_colorManager.resetAllColors();

    // Unregister pad logic
    crosspad::getPadManager().unregisterPadLogic("Sampler");

    // Null all UI pointers
    s_waveformView  = nullptr;
    s_kitNameLabel  = nullptr;
    s_padIndexLabel = nullptr;
    for (int i = 0; i < PAD_COUNT; ++i) {
        s_padButtons[i] = nullptr;
    }

    // Async delete the panel
    lv_obj_delete_async(panel);
}

} // namespace crosspad_sampler

#endif // USE_LVGL
