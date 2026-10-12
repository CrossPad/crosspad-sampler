// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef USE_LVGL

#include <crosspad-sampler/ui/sampler_styles.hpp>
#include <crosspad-gui/theme/crosspad_theme.h>
#include <crosspad-gui/widgets/gui_helpers.h>

lv_style_t styleWaveformBox;
lv_style_t styleKitBrowserScreen;
lv_style_t styleKitBrowserStatus;
lv_style_t styleKitBrowserRow;
lv_style_t styleKitBrowserRowSelected;
lv_style_t styleKitBrowserCover;
lv_style_t styleKitBrowserThumb;

namespace crosspad_sampler {

namespace {

bool       s_built = false;
lv_color_t s_builtPrimary;

void reset_all()
{
    lv_style_reset(&styleWaveformBox);
    lv_style_reset(&styleKitBrowserScreen);
    lv_style_reset(&styleKitBrowserStatus);
    lv_style_reset(&styleKitBrowserRow);
    lv_style_reset(&styleKitBrowserRowSelected);
    lv_style_reset(&styleKitBrowserCover);
    lv_style_reset(&styleKitBrowserThumb);
}

} // namespace

void sampler_styles_init()
{
    const lv_color_t primary = CROSSPAD_THEME_PRIMARY;
    if (s_built && lv_color_eq(primary, s_builtPrimary)) return;
    if (s_built) reset_all();
    s_built = true;
    s_builtPrimary = primary;

    // waveform box style
    lv_style_init(&styleWaveformBox);
    lv_style_set_bg_color(&styleWaveformBox, lv_color_black());
    lv_style_set_bg_opa(&styleWaveformBox, LV_OPA_20);
    lv_style_set_border_width(&styleWaveformBox, 2);
    lv_style_set_border_color(&styleWaveformBox, CROSSPAD_THEME_SECONDARY);
    lv_style_set_radius(&styleWaveformBox, 7);
    lv_style_set_pad_all(&styleWaveformBox, 5);
    lv_style_set_outline_width(&styleWaveformBox, 1);
    lv_style_set_text_color(&styleWaveformBox, CROSSPAD_THEME_SECONDARY);
    lv_style_set_text_font(&styleWaveformBox, &lv_font_montserrat_14);
    lv_style_set_text_opa(&styleWaveformBox, LV_OPA_100);

    // kit browser: the whole screen
    lv_style_init(&styleKitBrowserScreen);
    lv_style_set_bg_color(&styleKitBrowserScreen, CROSSPAD_THEME_BG_DEEP);
    lv_style_set_bg_opa(&styleKitBrowserScreen, LV_OPA_COVER);
    lv_style_set_pad_all(&styleKitBrowserScreen, 0);
    lv_style_set_border_width(&styleKitBrowserScreen, 0);
    lv_style_set_radius(&styleKitBrowserScreen, 0);

    // kit browser: status bar. A dark bar with the accent spent on a hairline
    // — a full primary slab reads as a neon stripe and pulls the eye off the
    // artwork, which is what the screen is for.
    lv_style_init(&styleKitBrowserStatus);
    lv_style_set_bg_color(&styleKitBrowserStatus, CROSSPAD_THEME_CARD_DEEP);
    lv_style_set_bg_opa(&styleKitBrowserStatus, LV_OPA_COVER);
    lv_style_set_text_color(&styleKitBrowserStatus, CROSSPAD_THEME_TEXT);
    lv_style_set_text_font(&styleKitBrowserStatus, &lv_font_montserrat_14);
    lv_style_set_radius(&styleKitBrowserStatus, 0);
    lv_style_set_border_width(&styleKitBrowserStatus, 1);
    lv_style_set_border_side(&styleKitBrowserStatus, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&styleKitBrowserStatus, primary);
    lv_style_set_border_opa(&styleKitBrowserStatus, LV_OPA_40);
    lv_style_set_pad_hor(&styleKitBrowserStatus, 9);
    lv_style_set_pad_ver(&styleKitBrowserStatus, 0);

    // kit browser: one row per kit
    lv_style_init(&styleKitBrowserRow);
    lv_style_set_bg_color(&styleKitBrowserRow, CROSSPAD_THEME_CARD_DEEP);
    lv_style_set_bg_opa(&styleKitBrowserRow, LV_OPA_COVER);
    lv_style_set_text_color(&styleKitBrowserRow, CROSSPAD_THEME_TEXT);
    lv_style_set_radius(&styleKitBrowserRow, 9);
    lv_style_set_border_width(&styleKitBrowserRow, 0);
    lv_style_set_pad_hor(&styleKitBrowserRow, 7);
    lv_style_set_pad_ver(&styleKitBrowserRow, 0);
    lv_style_set_pad_column(&styleKitBrowserRow, 7);

    // kit browser: the selected row
    lv_style_init(&styleKitBrowserRowSelected);
    lv_style_set_bg_color(&styleKitBrowserRowSelected, primary);
    lv_style_set_text_color(&styleKitBrowserRowSelected, lv_color_bw_from_color(primary));

    // kit browser: cover panel (clips the artwork to rounded corners)
    lv_style_init(&styleKitBrowserCover);
    lv_style_set_radius(&styleKitBrowserCover, 14);
    lv_style_set_clip_corner(&styleKitBrowserCover, true);
    lv_style_set_bg_color(&styleKitBrowserCover, CROSSPAD_THEME_CARD_DEEP);
    lv_style_set_bg_opa(&styleKitBrowserCover, LV_OPA_COVER);
    lv_style_set_text_color(&styleKitBrowserCover, primary);
    lv_style_set_text_font(&styleKitBrowserCover, &lv_font_montserrat_18);

    // kit browser: row thumbnail
    lv_style_init(&styleKitBrowserThumb);
    lv_style_set_radius(&styleKitBrowserThumb, 5);
    lv_style_set_clip_corner(&styleKitBrowserThumb, true);
    lv_style_set_bg_color(&styleKitBrowserThumb, CROSSPAD_THEME_BG_DEEP);
    lv_style_set_bg_opa(&styleKitBrowserThumb, LV_OPA_COVER);
    lv_style_set_text_color(&styleKitBrowserThumb, primary);
    lv_style_set_text_font(&styleKitBrowserThumb, &lv_font_montserrat_10);
}

} // namespace crosspad_sampler

#endif // USE_LVGL
