#ifdef USE_LVGL

/**
 * @file pad_editor.cpp
 * @brief The pad editor -- one pad's sample, its trim and its settings, laid
 *        out as the Recorder is: the same rows, the same picture, the same
 *        START and END.
 *
 *   top     back, the pad in its colour (hit a pad to edit that one; turned,
 *           the colour), its sample (tap: pick another), REV (the sample
 *           backwards, and drawn so: the picture mirrored, START on the left
 *           where the pad now begins -- the sample's end -- and turned the way
 *           it is heard), the mode (clicked through one shot, hold and loop),
 *           COPY (then hit the pad to copy it onto)
 *   params  VOL, PAN, PITCH, HP, LP and CHOKE are turned. PITCH is -12..+12
 *           semitones, varispeed as in the Slicer; HP and LP are 12 dB/octave
 *           filters stepped in thirds of an octave, OFF at their open end. The
 *           player can neither transpose, filter nor reverse, so the pad plays
 *           a copy of its sample rendered once the values stop moving
 *           (crosspad::renderedSamplePath) -- PLAY and LOOP play them at once.
 *   wave    the sample: what the pad plays between START and END lit, the
 *           rest dimmed, and the playhead while LOOP plays
 *   tools   START and END are turned: the first click moves the marker a
 *           column of the picture per detent, the second a millisecond with
 *           the picture zoomed in round it, the third is done. PLAY plays the
 *           trim once, LOOP round and round, FULL puts the markers back at the ends, DEL -- twice --
 *           takes the sample off the pad.
 *
 * Every change lands on the kit at once and is saved with it; the pad is
 * reloaded once a marker stops moving, not per detent. The picture and LOOP
 * go through crosspad::ISampleTools, as the Recorder's do.
 */

#include <lvgl.h>
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/ui/pad_editor.hpp"
#include "crosspad-sampler/ui/sampler_panel.hpp"

#include "crosspad-gui/crosspad_gui.h"
#include "crosspad-gui/widgets/gui_helpers.h"

#include <crosspad/led/ColorList.hpp>
#include <crosspad/led/HtmlColor.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/sampletools/ISampleTools.hpp>
#include <crosspad/sampletools/SliceMath.hpp>
#include <crosspad/status/CrosspadStatus.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using crosspad::ColorList;
using crosspad::KitInfo;
using crosspad::Pad;

namespace crosspad_sampler {

extern lv_subject_t s_pad_sampler; // the pad on show, 1-based (sampler_panel.cpp)
/// A pad hit while the editor is up, stamped by the pad thread and taken here,
/// on the LVGL thread.
extern std::atomic<int> g_padPressPending;

namespace {

constexpr uint16_t kCols       = 300;     // columns of the picture
constexpr uint32_t kTickMs     = 50;
constexpr uint32_t kZoomFrames = 12000;   // each side of a marker in fine mode, ~0.25 s
constexpr uint32_t kSettleMs   = 150;     // a marker still this long: the pad reloads, a loop restarts
constexpr uint32_t kFxSettleMs = 400;     // PITCH, HP, LP still this long: the pad's copy is rendered

/* The Recorder's colours. */
const lv_color_t kBg     = lv_color_hex(0x16181B);
const lv_color_t kPanel  = lv_color_hex(0x20242A);
const lv_color_t kText   = lv_color_hex(0xE8EAED);
const lv_color_t kDim    = lv_color_hex(0x7B828B);
const lv_color_t kInk    = lv_color_hex(0x16181B);
const lv_color_t kAccent = lv_color_hex(0xC6FF47);
const lv_color_t kRec    = lv_color_hex(0xFF5C5C);
const lv_color_t kLoopBg = lv_color_hex(0x263019);
const lv_color_t kWave   = lv_color_hex(0x9FD63A);
const lv_color_t kWaveOff= lv_color_hex(0x4A515B);
const lv_color_t kHead   = lv_color_hex(0xFFFFFF);
const lv_color_t kStartC = lv_color_hex(0x4FE0B0);

const char* const kModes[] = {"1-SHOT", "HOLD", "LOOP"};   // Pad::playbackMode

/// How a marker is being turned.
enum class Edit : uint8_t { None, Coarse, Fine };

lv_obj_t* s_cont = nullptr;
lv_obj_t* s_wave = nullptr;
lv_obj_t* s_busy = nullptr;               // on the picture: what runs in the background, and how far
lv_obj_t* s_busyText = nullptr;
lv_obj_t* s_busyBar = nullptr;
lv_obj_t* s_info = nullptr;
lv_obj_t* s_padBtn = nullptr;            // the pad, in its colour; turned, the colour
lv_obj_t* s_fileBtn = nullptr;
lv_obj_t* s_recBtn  = nullptr;      // record straight onto this pad
lv_obj_t* s_meter   = nullptr;      // input level while a take runs
lv_obj_t* s_meterBar[2] = {};
lv_obj_t* s_meterText = nullptr;
uint32_t  s_recStart = 0;           // tick the take began, for the time it shows

/// Recording onto the pad. Stopping is its own step because the take is not on
/// the card the moment stopRecording() returns -- the writer has a buffer to
/// finish -- and the file is only worth anything once recStatus() says so.
enum class Rec : uint8_t { Off, Running, Stopping };
Rec  s_rec = Rec::Off;
bool s_trimPending = false;         ///< the next picture to land gets trimmed to its sound
lv_obj_t* s_revBtn = nullptr;
lv_obj_t* s_copyBtn = nullptr;
lv_obj_t* s_volBtn = nullptr;
lv_obj_t* s_panBtn = nullptr;
lv_obj_t* s_pitchBtn = nullptr;
lv_obj_t* s_hpBtn = nullptr;
lv_obj_t* s_lpBtn = nullptr;
lv_obj_t* s_chokeBtn = nullptr;
lv_obj_t* s_modeBtn = nullptr;
lv_obj_t* s_startBtn = nullptr;
lv_obj_t* s_endBtn = nullptr;
lv_obj_t* s_loopBtn = nullptr;
lv_obj_t* s_delBtn = nullptr;
lv_obj_t* s_overlay = nullptr;
lv_timer_t* s_timer = nullptr;
lv_group_t* s_group = nullptr;
lv_group_t* s_priorGroup = nullptr;       // restored on destroy
lv_group_t* s_overlayGroup = nullptr;
uint32_t s_backToken = 0;
crosspad_gui::ILvglApp* s_app = nullptr;

int      s_pad = -1;                      // 0-based, the pad on screen
std::string s_sample;                     // its sample as the kit and the player name it; "" = none
std::string s_file;                       // the same on the filesystem, for the picture and LOOP
uint32_t s_frames = 0;
uint32_t s_rate = 48000;
uint32_t s_start = 0, s_end = 0;          // what the pad plays
crosspad::WaveOverview s_ov;              // the picture drawn
bool     s_ovPending = false;
bool     s_viewWanted = false;            // a picture asked for while a job ran
uint32_t s_wantA = 0, s_wantB = 0;
bool     s_loading = false;               // the markers take the pad's trim when the picture comes
Edit     s_editStart = Edit::None, s_editEnd = Edit::None;
bool     s_looping = false;               // LOOP: round and round
bool     s_playing = false;               // PLAY: once
bool     s_fxMoved = false;               // PITCH, HP or LP turned; the copy is asked for once it settles
uint32_t s_fxAt = 0;
bool     s_renderWanted = false;          // a copy to render, waiting for the tools
bool     s_renderPending = false;         // the tools are rendering it
int      s_renderPad = -1;                // whose copy
std::string s_renderSrc, s_renderOut;     // filesystem paths: the sample, its transposed copy
bool     s_moved = false;                 // a marker moved; the pad is reloaded once it settles
uint32_t s_movedAt = 0;
int32_t  s_headX = -1;                    // playhead column as drawn, -1 = none
bool     s_copying = false;               // COPY: the next pad hit is the target
int      s_copyFrom = -1, s_copyTo = -1;
bool     s_delArmed = false;              // DEL clicked once
int      s_color = 0;                     // index into ColorList, the COLOR button's

inline lv_indev_t* encoder() { return crosspad_gui::getGuiPlatform().getNavigationEncoder(); }
inline crosspad::ISampleTools* tools() { return crosspad::getPlatformServices().sampleTools; }

KitInfo* kit()
{
    auto* mgr = crosspad::getKitManager();
    return mgr ? mgr->getCurrentKit() : nullptr;
}

Pad* pad()
{
    KitInfo* k = kit();
    return k && s_pad >= 0 && s_pad < KIT_PADS ? &k->pads[s_pad] : nullptr;
}

/// What pad @p idx is played with; nothing when there is no such pad.
crosspad::SampleFx pad_fx(int idx)
{
    KitInfo* k = kit();
    if (!k || idx < 0 || idx >= KIT_PADS) return {};
    const Pad& p = k->pads[idx];
    return crosspad::SampleFx{p.pitch, p.hp, p.lp, p.reverse};
}

/// A cutoff as a button says it: 200, 1k, 1.3k, 12.5k.
std::string fmt_hz(uint16_t hz)
{
    char buf[12];
    if (hz < 1000)            std::snprintf(buf, sizeof(buf), "%u", (unsigned)hz);
    else if (hz % 1000 == 0)  std::snprintf(buf, sizeof(buf), "%uk", (unsigned)(hz / 1000));
    else                      std::snprintf(buf, sizeof(buf), "%.1fk", hz / 1000.0);
    return buf;
}

/// @p hz's place among the knob's cutoffs, the nearest one.
int cutoff_index(uint16_t hz)
{
    int best = 0;
    for (int i = 1; i < crosspad::kFxCutoffCount; ++i) {
        if (std::abs((int)crosspad::kFxCutoffs[i] - hz) < std::abs((int)crosspad::kFxCutoffs[best] - hz)) best = i;
    }
    return best;
}

std::string base_of(const std::string& p)
{
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

void set_text(lv_obj_t* l, const char* t)
{
    if (l && std::strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

void set_bg(lv_obj_t* o, lv_color_t c)
{
    if (o && !lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), c)) lv_obj_set_style_bg_color(o, c, 0);
}

void set_ink(lv_obj_t* l, lv_color_t c)
{
    if (l && !lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), c)) lv_obj_set_style_text_color(l, c, 0);
}

/// The name ColorList gives colour @p i, upper case.
std::string color_name(int i)
{
    ColorList cl;
    const char* s = cl.Get();
    for (int n = 0; n < i && s; ++n) {
        s = std::strchr(s, '\n');
        if (s) ++s;
    }
    std::string name;
    for (; s && *s && *s != '\n'; ++s) name += (char)std::toupper((unsigned char)*s);
    return name;
}

/// The pad's colour's place in ColorList; 0 when it is not one of them.
int color_index(uint32_t rgb)
{
    ColorList cl;
    for (int i = 0; i < (int)cl.GetCount(); ++i) {
        if ((uint32_t)cl.Get(i) == rgb) return i;
    }
    return 0;
}

// ── the picture ────────────────────────────────────────────────────────────

void request_view(uint32_t a, uint32_t b)
{
    auto* st = tools();
    if (!st || s_file.empty()) return;
    if (st->requestOverview(s_file, a, b, kCols)) {
        s_ovPending = true;
        s_viewWanted = false;
    } else {
        s_viewWanted = true;           // a job is running; asked again from the timer
        s_wantA = a;
        s_wantB = b;
    }
}

void fill(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

bool reversed()
{
    const Pad* p = pad();
    return p && p->reverse;
}

/// Where frame @p f of the picture's range is drawn in @p box: mirrored when
/// the pad plays the sample backwards, so the picture reads as it is heard.
int32_t frame_x(uint32_t f, const lv_area_t& box, bool rev)
{
    const int32_t w = lv_area_get_width(&box);
    const uint32_t a = s_ov.start, len = s_ov.end - s_ov.start;
    int32_t x;
    if (f <= a) x = box.x1;
    else if (f >= s_ov.end) x = box.x2;
    else x = box.x1 + (int32_t)((uint64_t)(f - a) * w / len);
    return rev ? box.x1 + box.x2 - x : x;
}

void wave_draw_cb(lv_event_t* e)
{
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!layer || s_ov.lo.empty() || s_ov.end <= s_ov.start) return;
    lv_area_t box;
    lv_obj_get_content_coords(obj, &box);
    const int32_t w = lv_area_get_width(&box);
    const int32_t h = lv_area_get_height(&box);
    const int32_t mid = box.y1 + h / 2;
    const uint32_t a = s_ov.start, len = s_ov.end - s_ov.start;
    const bool rev = reversed();
    /* What the pad plays lit, the rest dimmed. */
    const int32_t xs = frame_x(s_start, box, rev), xe = frame_x(s_end, box, rev);
    if (xs != xe) fill(layer, std::min(xs, xe), box.y1, std::max(xs, xe), box.y2, kLoopBg);
    const size_t cols = s_ov.lo.size();
    for (size_t c = 0; c < cols; ++c) {
        int32_t x = box.x1 + (int32_t)(c * w / cols);
        if (rev) x = box.x1 + box.x2 - x;
        const uint32_t f = a + (uint32_t)((uint64_t)c * len / cols);
        const int32_t y1 = mid - (int32_t)s_ov.hi[c] * (h / 2) / 128;
        const int32_t y2 = mid - (int32_t)s_ov.lo[c] * (h / 2) / 128;
        fill(layer, x, std::min(y1, y2), x, std::max(y1, y2), f >= s_start && f < s_end ? kWave : kWaveOff);
    }
    /* START where the pad begins, END where it stops: reversed, those are the
     * sample's end and start. */
    const uint32_t begin = rev ? s_end : s_start, stop = rev ? s_start : s_end;
    const int32_t xb = rev ? xe : xs, xt = rev ? xs : xe;
    if (begin >= a && begin <= s_ov.end) fill(layer, xb, box.y1, xb + 1, box.y2, kStartC);
    if (stop >= a && stop <= s_ov.end) fill(layer, xt - 1, box.y1, xt, box.y2, kRec);
    if (s_headX >= 0) fill(layer, s_headX, box.y1, s_headX, box.y2, kHead);
}

/// The playhead's screen x, -1 when LOOP is not playing or it is off the
/// picture. Only its old and new columns are redrawn while it moves.
int32_t head_x()
{
    auto* st = tools();
    if (!st || !s_wave || !(s_looping || s_playing) || !st->previewing() || s_ov.end <= s_ov.start) return -1;
    const uint32_t p = st->previewFrame();
    if (p < s_ov.start || p > s_ov.end) return -1;
    lv_area_t box;
    lv_obj_get_content_coords(s_wave, &box);
    return frame_x(p, box, reversed());
}

void move_head()
{
    const int32_t x = head_x();
    if (x == s_headX || !s_wave) return;
    lv_area_t box;
    lv_obj_get_content_coords(s_wave, &box);
    for (int32_t at : {s_headX, x}) {
        if (at < 0) continue;
        lv_area_t ar = {at - 1, box.y1, at + 1, box.y2};
        lv_obj_invalidate_area(s_wave, &ar);
    }
    s_headX = x;
}

// ── what the screen says ───────────────────────────────────────────────────

/// On the picture itself, where it is looked at: the pad's copy being made
/// (PITCH, HP, LP, REV -- which, and how far) or the sample being read. One
/// line and a thin bar at the top; the dots move, so a job that waits for the
/// card still reads as alive.
void show_busy()
{
    if (!s_busy) return;
    const bool rendering = s_fxMoved || s_renderWanted || s_renderPending;
    const bool loading = !s_file.empty() && !s_frames;
    if (!rendering && !loading) {
        if (!lv_obj_has_flag(s_busy, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(s_busy, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    static const char* const kDots[] = {".  ", ".. ", "...", "   "};
    const char* dots = kDots[(lv_tick_get() / 300) % 4];
    auto* st = tools();
    /* A figure only when something is reporting one. Two different waits end up
     * here and only one of them counts: the card reader says how far through
     * the file it is, while the engine taking the sample in says nothing at all
     * -- and showing that one as "0%" read as a hang that never moved, which is
     * exactly how it was reported. So the percentage appears when there is a
     * job behind it and the dots carry the rest. */
    const bool counted = st && (s_renderPending || s_ovPending);
    const int pct = counted ? (int)st->jobProgress() : 0;
    std::string text;
    char buf[40];
    if (rendering) {
        std::snprintf(buf, sizeof(buf), "PROCESSING %d%%%s", pct, dots);
        text = buf;
        if (const Pad* p = pad()) {
            if (p->reverse) text += "  REV";
            if (p->pitch) { std::snprintf(buf, sizeof(buf), "  PITCH %+d", p->pitch); text += buf; }
            if (p->hp) text += "  HP " + fmt_hz(p->hp);
            if (p->lp) text += "  LP " + fmt_hz(p->lp);
        }
    } else {
        if (counted) std::snprintf(buf, sizeof(buf), "READING %d%%%s  ", pct, dots);
        else         std::snprintf(buf, sizeof(buf), "LOADING%s  ", dots);
        text = buf + base_of(s_file);
    }
    set_text(s_busyText, text.c_str());
    lv_bar_set_value(s_busyBar, pct, LV_ANIM_OFF);
    if (counted) lv_obj_remove_flag(s_busyBar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_busyBar, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_has_flag(s_busy, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(s_busy, LV_OBJ_FLAG_HIDDEN);
}

void show_info()
{
    if (!s_info) return;
    char buf[112];
    Pad* p = pad();
    if (!p) {
        std::snprintf(buf, sizeof(buf), "Hit a pad to edit it.");
    } else if (s_copying) {
        std::snprintf(buf, sizeof(buf), "COPY: hit the pad to copy PAD %d onto -- COPY again to stop.", s_copyFrom + 1);
    } else if (s_delArmed) {
        std::snprintf(buf, sizeof(buf), "DEL again takes %s off PAD %d.", base_of(s_file).c_str(), s_pad + 1);
    } else if (s_file.empty()) {
        std::snprintf(buf, sizeof(buf), "PAD %d has no sample -- the file button picks one.", s_pad + 1);
    } else if (!s_frames) {
        std::snprintf(buf, sizeof(buf), "%s  reading ...", base_of(s_file).c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "%s  %.2f s | plays %.3f %s %.3f s (%.3f s)", base_of(s_file).c_str(),
                      (double)s_frames / s_rate, (double)(p->reverse ? s_end : s_start) / s_rate,
                      p->reverse ? "back to" : "-", (double)(p->reverse ? s_start : s_end) / s_rate,
                      (double)(s_end - s_start) / s_rate);
    }
    set_text(s_info, buf);
}

void show_marker_labels()
{
    auto label = [](lv_obj_t* b, const char* name, Edit e) {
        if (!b) return;
        char buf[16];
        if (e == Edit::None) std::snprintf(buf, sizeof(buf), "%s", name);
        else std::snprintf(buf, sizeof(buf), "%c %s", name[0], e == Edit::Coarse ? "<>" : "fine");
        set_text(lv_obj_get_child(b, 0), buf);
    };
    label(s_startBtn, "START", s_editStart);
    label(s_endBtn, "END", s_editEnd);
}

/// REC while it is off, STOP while it runs, and red either way it is busy.
void show_rec()
{
    if (s_recBtn) {
        const bool live = s_rec != Rec::Off;
        set_text(lv_obj_get_child(s_recBtn, 0), live ? LV_SYMBOL_STOP " STOP" : LV_SYMBOL_AUDIO " REC");
        lv_obj_set_style_bg_color(s_recBtn, live ? lv_color_hex(0xFF5C5C) : CROSSPAD_THEME_CARD_DEEP, 0);
    }
    if (!s_meter) return;
    if (s_rec == Rec::Off) {
        if (!lv_obj_has_flag(s_meter, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(s_meter, LV_OBJ_FLAG_HIDDEN);
        /* Back to the editor's own pace: the meter is the only thing here that
         * wants more than five frames a second. */
        if (s_timer) lv_timer_set_period(s_timer, kTickMs);
        return;
    }
    if (lv_obj_has_flag(s_meter, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(s_meter, LV_OBJ_FLAG_HIDDEN);
    if (s_timer) lv_timer_set_period(s_timer, 50);
}

/// The input, while it is being recorded: both peaks since the last look, and
/// how long the take is. Reading peaks() clears them, so nothing else may ask.
void show_meter()
{
    auto* st = tools();
    if (!st || !s_meter || s_rec == Rec::Off) return;
    float l = 0.0f, r = 0.0f;
    st->peaks(l, r);
    const float v[2] = {l, r};
    for (int i = 0; i < 2; ++i) {
        if (!s_meterBar[i]) continue;
        int pct = (int)(v[i] * 100.0f + 0.5f);
        lv_bar_set_value(s_meterBar[i], pct < 0 ? 0 : (pct > 100 ? 100 : pct), LV_ANIM_OFF);
    }
    if (s_meterText) {
        const uint32_t ms = lv_tick_elaps(s_recStart);
        char buf[24];
        std::snprintf(buf, sizeof(buf), LV_SYMBOL_AUDIO " %lu:%02lu.%lu",
                      (unsigned long)(ms / 60000), (unsigned long)((ms / 1000) % 60),
                      (unsigned long)((ms % 1000) / 100));
        set_text(s_meterText, buf);
    }
}

/// Every button that shows a value of the pad.
void show_pad()
{
    show_rec();
    Pad* p = pad();
    char buf[32];
    if (s_padBtn) {
        /* Turned, it says which colour; otherwise which pad, in that colour. */
        if (lv_obj_has_state(s_padBtn, LV_STATE_EDITED)) std::snprintf(buf, sizeof(buf), "%s", color_name(s_color).c_str());
        else if (p) std::snprintf(buf, sizeof(buf), "PAD %d", s_pad + 1);
        else        std::snprintf(buf, sizeof(buf), "PAD -");
        set_text(lv_obj_get_child(s_padBtn, 0), buf);
        const uint32_t c = p ? p->color : 0;
        const lv_color_t bg = c ? lv_color_hex(c) : kPanel;
        set_bg(s_padBtn, bg);
        lv_obj_set_style_bg_color(s_padBtn, bg, LV_STATE_EDITED);
        const uint32_t luma = ((c >> 16) & 0xFF) * 299 + ((c >> 8) & 0xFF) * 587 + (c & 0xFF) * 114;
        const lv_color_t ink = c && luma > 128000 ? kInk : kText;
        /* On the label: the pad's colour is the background in both states. */
        set_ink(lv_obj_get_child(s_padBtn, 0), ink);
    }
    if (s_fileBtn) {
        const std::string name = s_file.empty() ? std::string(LV_SYMBOL_DIRECTORY " pick a sample")
                                                : std::string(LV_SYMBOL_AUDIO " ") + base_of(s_file);
        set_text(lv_obj_get_child(s_fileBtn, 0), name.c_str());
    }
    if (s_volBtn) {
        std::snprintf(buf, sizeof(buf), "VOL %d", p ? p->volume : 0);
        set_text(lv_obj_get_child(s_volBtn, 0), buf);
    }
    if (s_panBtn) {
        const int pan = p ? p->pan : 0;
        if (!pan)         std::snprintf(buf, sizeof(buf), "PAN C");
        else              std::snprintf(buf, sizeof(buf), "PAN %c%d", pan < 0 ? 'L' : 'R', pan < 0 ? -pan : pan);
        set_text(lv_obj_get_child(s_panBtn, 0), buf);
    }
    if (s_pitchBtn) {
        if (p && p->pitch) std::snprintf(buf, sizeof(buf), "PITCH %+d", p->pitch);
        else               std::snprintf(buf, sizeof(buf), "PITCH 0");
        set_text(lv_obj_get_child(s_pitchBtn, 0), buf);
    }
    if (s_hpBtn) {
        const std::string v = p && p->hp ? fmt_hz(p->hp) : std::string("OFF");
        std::snprintf(buf, sizeof(buf), "HP %s", v.c_str());
        set_text(lv_obj_get_child(s_hpBtn, 0), buf);
    }
    if (s_lpBtn) {
        const std::string v = p && p->lp ? fmt_hz(p->lp) : std::string("OFF");
        std::snprintf(buf, sizeof(buf), "LP %s", v.c_str());
        set_text(lv_obj_get_child(s_lpBtn, 0), buf);
    }
    if (s_chokeBtn) {
        if (p && p->choke_group) std::snprintf(buf, sizeof(buf), "CHOKE %d", p->choke_group);
        else                     std::snprintf(buf, sizeof(buf), "CHOKE -");
        set_text(lv_obj_get_child(s_chokeBtn, 0), buf);
    }
    if (s_modeBtn) set_text(lv_obj_get_child(s_modeBtn, 0), kModes[p && p->playbackMode < 3 ? p->playbackMode : 0]);
    if (s_revBtn) {
        const bool rev = p && p->reverse;
        set_bg(s_revBtn, rev ? kAccent : kPanel);
        set_ink(lv_obj_get_child(s_revBtn, 0), rev ? kInk : kText);
    }
    if (s_copyBtn) {
        set_bg(s_copyBtn, s_copying ? kAccent : kPanel);
        set_ink(lv_obj_get_child(s_copyBtn, 0), s_copying ? kInk : kText);
    }
    if (s_loopBtn) {
        set_bg(s_loopBtn, s_looping ? kAccent : kPanel);
        set_ink(lv_obj_get_child(s_loopBtn, 0), s_looping ? kInk : kText);
    }
    if (s_delBtn) {
        set_bg(s_delBtn, s_delArmed ? kRec : kPanel);
        set_ink(lv_obj_get_child(s_delBtn, 0), s_delArmed ? kInk : kText);
    }
}

/// The selected pad at full colour on the LEDs, the rest dimmed.
void light_pads()
{
    KitInfo* k = kit();
    if (!k) return;
    auto& pm = crosspad::getPadManager();
    for (int i = 0; i < KIT_PADS; i++) {
        const uint32_t c = k->pads[i].color;
        if (i == s_pad) pm.setPadColor((uint8_t)i, crosspad::HtmlColor(c));
        else pm.setPadColor((uint8_t)i, ((c >> 16) & 0xFF) / 16, ((c >> 8) & 0xFF) / 16, (c & 0xFF) / 16);
    }
}

void stop_loop()
{
    if (s_looping || s_playing) {
        if (auto* st = tools()) st->stopPreview();
        s_looping = s_playing = false;
    }
}

/// What PLAY and LOOP play: the trim, transposed and filtered as the pad is.
crosspad::PreviewSound pad_sound()
{
    crosspad::PreviewSound s;
    if (Pad* p = pad()) {
        s.semitones = (int8_t)p->pitch;
        s.hpHz = p->hp;
        s.lpHz = p->lp;
        s.reverse = p->reverse;
    }
    return s;
}

/// Pad @p idx on screen: its settings, its sample, its trim.
void open_pad(int idx)
{
    stop_loop();
    s_pad = idx;
    s_editStart = s_editEnd = Edit::None;
    s_delArmed = false;
    s_moved = false;
    s_sample.clear();
    s_file.clear();
    s_frames = 0;
    s_ov = crosspad::WaveOverview{};
    s_ovPending = s_viewWanted = false;
    s_loading = false;
    Pad* p = pad();
    KitInfo* k = kit();
    if (p && k && p->GetSampleCount()) {
        s_sample = crosspad::resolveKitSamplePath(k->path, p->samples[0].sample);
        s_file = crosspad::kitFsPath(s_sample);
        s_start = p->samples[0].start;
        s_end = p->samples[0].end;
        s_loading = true;
        request_view(0, 0);
    }
    s_color = p ? color_index(p->color) : 0;
    if (s_group) lv_group_set_editing(s_group, false);
    light_pads();
    show_pad();
    show_marker_labels();
    show_info();
    if (s_wave) lv_obj_invalidate(s_wave);
}

// ── editing ────────────────────────────────────────────────────────────────

/// The encoder's detent on a turned button, 0 for anything else; a click
/// starts or ends turning it.
int number_step(lv_event_t* e)
{
    lv_obj_t* b = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_group_t* g = b ? lv_obj_get_group(b) : nullptr;
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (g) {
            const bool editing = lv_group_get_editing(g) && lv_group_get_focused(g) == b;
            lv_group_focus_obj(b);
            lv_group_set_editing(g, !editing);
        }
        return 0;
    }
    const uint32_t key = lv_event_get_key(e);
    return key == LV_KEY_RIGHT || key == LV_KEY_UP ? 1 : (key == LV_KEY_LEFT || key == LV_KEY_DOWN ? -1 : 0);
}

void vol_cb(lv_event_t* e)
{
    const int d = number_step(e);
    Pad* p = pad();
    if (!d || !p) return;
    p->volume = std::max(0, std::min(127, p->volume + d));
    p->dirtyModified = true;               // applied at once, no reload
    auto& cbs = sampler_get_platform_callbacks();
    if (cbs.player.setVolume) cbs.player.setVolume((uint8_t)s_pad, p->volume);
    show_pad();
}

/// Kept signed with 0 the centre, as the kit file has it; the player gets its
/// own 0..127 at its boundary.
void pan_cb(lv_event_t* e)
{
    const int d = number_step(e);
    Pad* p = pad();
    if (!d || !p) return;
    p->pan = std::max(-64, std::min(63, p->pan + d));
    p->dirtyModified = true;
    auto& cbs = sampler_get_platform_callbacks();
    if (cbs.player.setPan) cbs.player.setPan((uint8_t)s_pad, p->pan);
    show_pad();
}

void choke_cb(lv_event_t* e)
{
    const int d = number_step(e);
    Pad* p = pad();
    if (!d || !p) return;
    p->choke_group = std::max(0, std::min(7, p->choke_group + d));
    p->dirty = true;                       // the engine takes it on a reload
    p->dirtyModified = true;
    auto& cbs = sampler_get_platform_callbacks();
    if (cbs.audio.reloadPad) cbs.audio.reloadPad((uint8_t)s_pad);
    show_pad();
}

void mode_cb(lv_event_t*)
{
    Pad* p = pad();
    if (!p) return;
    p->playbackMode = (uint8_t)((p->playbackMode + 1) % 3);
    p->dirty = true;
    p->dirtyModified = true;
    auto& cbs = sampler_get_platform_callbacks();
    if (p->playbackMode == 2) { if (cbs.player.setLoopEnd) cbs.player.setLoopEnd((uint8_t)s_pad, SAMPLE_END_MAX); }
    else if (cbs.player.clearLoop) cbs.player.clearLoop((uint8_t)s_pad);
    show_pad();
}

/// The pad button, turned: its colour. The click that starts or ends turning
/// it switches what it says (the colour's name, the pad), so it is shown
/// either way.
void color_cb(lv_event_t* e)
{
    const int d = number_step(e);
    Pad* p = pad();
    if (!p) return;
    if (d) {
        ColorList cl;
        s_color = std::max(0, std::min((int)cl.GetCount() - 1, s_color + d));
        p->color = (uint32_t)cl.Get(s_color);
        p->dirtyModified = true;
        light_pads();
    }
    show_pad();
}

/// The markers onto the pad: END at the file's end is "to the end", as a kit
/// says it. The engine is reloaded once they stop moving.
void write_trim()
{
    Pad* p = pad();
    if (!p || !p->GetSampleCount()) return;
    p->samples[0].start = s_start;
    p->samples[0].end = s_end >= s_frames ? 0 : s_end;
    p->dirty = true;
    p->dirtyModified = true;
    s_moved = true;
    s_movedAt = lv_tick_get();
}

void marker_moved(uint32_t at, Edit e)
{
    if (e == Edit::Fine && (at < s_ov.start + kZoomFrames / 4 || at > s_ov.end - kZoomFrames / 4)) {
        request_view(at > kZoomFrames ? at - kZoomFrames : 0, std::min(s_frames, at + kZoomFrames));
    }
    write_trim();
    show_info();
    if (s_wave) lv_obj_invalidate(s_wave);
}

/// START and END are where the pad begins and stops. Reversed, those are the
/// sample's end and start, and turning right moves them the way it is heard --
/// back through the sample.
void marker_cb(lv_event_t* e)
{
    const bool isStart = lv_event_get_user_data(e) != nullptr;
    const bool rev = reversed();
    const bool movesStart = isStart != rev;          // which of the sample's own markers
    Edit& edit = isStart ? s_editStart : s_editEnd;
    uint32_t& m = movesStart ? s_start : s_end;
    lv_obj_t* b = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_group_t* g = lv_obj_get_group(b);
    const lv_event_code_t code = lv_event_get_code(e);
    if (!s_frames) return;
    if (code == LV_EVENT_FOCUSED) {
        /* The encoder's first click puts the button in edit mode; LVGL says
         * so with FOCUSED, not CLICKED. */
        const bool editing = g && lv_group_get_editing(g);
        if (editing && edit == Edit::None) edit = Edit::Coarse;
        else if (!editing && edit != Edit::None) {
            if (edit == Edit::Fine) request_view(0, 0);
            edit = Edit::None;
        }
        show_marker_labels();
        return;
    }
    if (code == LV_EVENT_CLICKED) {
        if (edit == Edit::Coarse) {
            edit = Edit::Fine;
            request_view(m > kZoomFrames ? m - kZoomFrames : 0, std::min(s_frames, m + kZoomFrames));
            if (g) lv_group_set_editing(g, true);
        } else if (edit == Edit::Fine) {
            edit = Edit::None;
            request_view(0, 0);
            if (g) lv_group_set_editing(g, false);
        } else {
            edit = Edit::Coarse;       // a tap: turn it with the encoder now
            if (g) { lv_group_focus_obj(b); lv_group_set_editing(g, true); }
        }
        show_marker_labels();
        return;
    }
    const uint32_t key = lv_event_get_key(e);
    int dir = key == LV_KEY_RIGHT || key == LV_KEY_UP ? 1 : (key == LV_KEY_LEFT || key == LV_KEY_DOWN ? -1 : 0);
    if (!dir || edit == Edit::None) return;
    if (rev) dir = -dir;
    const uint32_t viewLen = s_ov.end > s_ov.start ? s_ov.end - s_ov.start : s_frames;
    const uint32_t step = edit == Edit::Coarse ? std::max<uint32_t>(1, viewLen / kCols)
                                               : std::max<uint32_t>(1, s_rate / 1000);
    int64_t v = (int64_t)m + (int64_t)dir * step;
    const int64_t lo = movesStart ? 0 : (int64_t)s_start + 1;
    const int64_t hi = movesStart ? (int64_t)s_end - 1 : (int64_t)s_frames;
    v = std::max(lo, std::min(hi, v));
    m = (uint32_t)v;
    marker_moved(m, edit);
}

// ── actions ────────────────────────────────────────────────────────────────

void close_cb(lv_event_t*)
{
    if (s_cont) lv_obj_delete_async(s_cont);   // everything else goes from its DELETE
}

/// PLAY: the trim once, through the sampler, at the pad's volume and pan.
/// PLAY: the trim once, transposed as the pad is -- at once, before the
/// pad's own copy is rendered.
void play_cb(lv_event_t*)
{
    auto* st = tools();
    if (!st || !s_frames) return;
    stop_loop();
    s_playing = st->previewAs(s_file, s_start, s_end, false, pad_sound());
    show_pad();
}

void loop_cb(lv_event_t*)
{
    auto* st = tools();
    if (!st || !s_frames) return;
    if (s_looping) { st->stopPreview(); s_looping = false; }
    else { s_playing = false; s_looping = st->previewAs(s_file, s_start, s_end, true, pad_sound()); }
    show_pad();
}

/// PITCH, HP or LP moved on @p p: a loop plays it at once, the pad's own copy
/// is rendered once nothing has moved for kFxSettleMs.
void fx_moved(Pad* p)
{
    p->dirty = true;
    p->dirtyModified = true;
    s_fxMoved = true;
    s_fxAt = lv_tick_get();
    s_renderPad = s_pad;
    auto* st = tools();
    if (st && s_looping) st->previewAs(s_file, s_start, s_end, true, pad_sound());
    show_pad();
    show_info();
}

/// PITCH: turned, in semitones.
void pitch_cb(lv_event_t* e)
{
    const int d = number_step(e);
    Pad* p = pad();
    if (!d || !p) return;
    p->pitch = std::max(-crosspad::SliceMath::kMaxSemitones,
                        std::min(crosspad::SliceMath::kMaxSemitones, p->pitch + d));
    fx_moved(p);
}

/// REV: the sample backwards, on or off.
void rev_cb(lv_event_t*)
{
    Pad* p = pad();
    if (!p || !p->GetSampleCount()) return;
    p->reverse = !p->reverse;
    fx_moved(p);
    if (s_wave) lv_obj_invalidate(s_wave);          // drawn the other way round now
}

/// HP: turned up from OFF through the cutoffs.
void hp_cb(lv_event_t* e)
{
    const int d = number_step(e);
    Pad* p = pad();
    if (!d || !p) return;
    const int i = std::max(-1, std::min(crosspad::kFxCutoffCount - 1, (p->hp ? cutoff_index(p->hp) : -1) + d));
    p->hp = i < 0 ? 0 : crosspad::kFxCutoffs[i];
    fx_moved(p);
}

/// LP: turned down from OFF through the cutoffs.
void lp_cb(lv_event_t* e)
{
    const int d = number_step(e);
    Pad* p = pad();
    if (!d || !p) return;
    const int n = crosspad::kFxCutoffCount;
    const int i = std::max(0, std::min(n, (p->lp ? cutoff_index(p->lp) : n) + d));
    p->lp = i >= n ? 0 : crosspad::kFxCutoffs[i];
    fx_moved(p);
}

void full_cb(lv_event_t*)
{
    if (!s_frames) return;
    s_start = 0;
    s_end = s_frames;
    s_editStart = s_editEnd = Edit::None;
    if (s_ov.start != 0 || s_ov.end != s_frames) request_view(0, 0);
    write_trim();
    show_marker_labels();
    show_info();
    if (s_wave) lv_obj_invalidate(s_wave);
}

void del_cb(lv_event_t*)
{
    Pad* p = pad();
    if (!p || !p->GetSampleCount()) return;
    if (!s_delArmed) {
        s_delArmed = true;
        show_pad();
        show_info();
        return;
    }
    stop_loop();
    auto& cbs = sampler_get_platform_callbacks();
    if (cbs.player.wipeOut) cbs.player.wipeOut((uint8_t)s_pad);
    p->RemoveAllSamples();
    p->dirtyModified = true;
    if (cbs.audio.reloadPad) cbs.audio.reloadPad((uint8_t)s_pad);
    open_pad(s_pad);
}

void copy_cb(lv_event_t*)
{
    Pad* p = pad();
    if (s_copying) {
        s_copying = false;
    } else if (p && p->GetSampleCount()) {
        s_copying = true;
        s_copyFrom = s_pad;
    }
    show_pad();
    show_info();
}

// ── windows over the editor ────────────────────────────────────────────────

void close_overlay()
{
    if (!s_overlay) return;
    lv_indev_set_group(encoder(), s_group);
    lv_group_delete_deferred(s_overlayGroup);     // the click that closed it is still being read
    s_overlayGroup = nullptr;
    lv_obj_delete_async(s_overlay);
    s_overlay = nullptr;
}

void open_overlay(const char* title)
{
    s_overlay = lv_obj_create(s_cont);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(s_overlay, kBg, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_overlay, 6, 0);
    lv_obj_set_style_pad_row(s_overlay, 3, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    s_overlayGroup = lv_group_create();
    lv_indev_set_group(encoder(), s_overlayGroup);
    lv_obj_t* t = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(t, kText, 0);
    lv_label_set_text(t, title);
}

lv_obj_t* overlay_item(const char* text, lv_event_cb_t cb, bool accent = false)
{
    lv_obj_t* b = lv_button_create(s_overlay);
    lv_obj_set_size(b, LV_PCT(100), 24);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, accent ? kAccent : kPanel, 0);
    lv_obj_set_style_outline_color(b, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_color(l, accent ? kInk : kText, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_group_add_obj(s_overlayGroup, b);
    return b;
}

/// COPY's target hit: asked once more, "Keep" first.
void ask_copy(int to)
{
    s_copyTo = to;
    char title[48];
    std::snprintf(title, sizeof(title), "Copy PAD %d onto PAD %d?", s_copyFrom + 1, to + 1);
    open_overlay(title);
    lv_obj_t* hint = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, kDim, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_text(hint, "Its sample, trim and settings take the place of what is on it now.");
    overlay_item(LV_SYMBOL_LEFT " Keep it", [](lv_event_t*) {
        close_overlay();
        s_copying = false;
        show_pad();
        show_info();
    }, true);
    overlay_item(LV_SYMBOL_COPY " Copy", [](lv_event_t*) {
        close_overlay();
        s_copying = false;
        KitInfo* k = kit();
        if (!k || s_copyFrom < 0 || s_copyTo < 0) return;
        auto& cbs = sampler_get_platform_callbacks();
        if (cbs.player.wipeOut) cbs.player.wipeOut((uint8_t)s_copyTo);
        k->pads[s_copyTo].CopyFrom(k->pads[s_copyFrom]);
        if (cbs.audio.reloadPad) cbs.audio.reloadPad((uint8_t)s_copyTo);
        lv_subject_set_int(&s_pad_sampler, s_copyTo + 1);
        open_pad(s_copyTo);
    });
}

// ── the sample ─────────────────────────────────────────────────────────────

/// The browser's Load: the file onto the pad.
/// @p path onto this pad, as the only sample it plays. The browser's Load and a
/// finished take both end here, so a recording lands exactly the way a file
/// picked by hand does -- same slot check, same reload, same reopen.
/// @p reloadNow false when a trim is already on its way: the markers settling
/// reload the engine themselves (see s_moved), and taking the sample in twice
/// for one recording is most of the wait after a take.
bool put_on_pad(const std::string& path, bool reloadNow = true)
{
    Pad* p = pad();
    if (!p || path.empty()) return false;
    auto& cbs = sampler_get_platform_callbacks();
    if (cbs.player.wipeOut) cbs.player.wipeOut((uint8_t)s_pad);
    p->RemoveAllSamples();
    if (cbs.player.getFreeSlotCount && cbs.player.getFreeSlotCount() <= 0) {
        LV_LOG_ERROR("No free sample slots available!");
        return false;
    }
    p->AddSample(path);
    p->dirtyModified = true;
    if (reloadNow && cbs.audio.reloadPad) cbs.audio.reloadPad((uint8_t)s_pad);
    open_pad(s_pad);
    return true;
}

void load_cb(lv_event_t* e)
{
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_delete_async(lv_obj_get_parent(obj));      // the browser; its DELETE brings the editor back
    const crosspad_gui::FileItem f = GetCurrentFile();
    if (f.path.empty() || f.name.empty() || f.isFolder) return;
    put_on_pad(f.path);
}

/// REC/STOP. The input is whichever the Recorder was last set to -- MIC, LINE,
/// PADS or USB -- so the two screens are one setting and this one needs no
/// picker of its own. PADS is the interesting one here: it takes the engine's
/// own output, so a pad can be resampled through its filters and pitch with no
/// cable at all.
void rec_cb(lv_event_t*)
{
    auto* st = tools();
    if (!st || !pad() || s_overlay) return;
    if (s_rec == Rec::Running) {
        st->stopRecording();
        s_rec = Rec::Stopping;
        show_pad();
        return;
    }
    if (s_rec != Rec::Off) return;      // still closing the last one
    stop_loop();                        // nothing of ours playing into the take
    st->selectInput(st->input());
    if (!st->startRecording()) {
        set_text(s_info, "Cannot record now -- is the card in?");
        return;
    }
    s_rec = Rec::Running;
    s_recStart = lv_tick_get();
    show_pad();
}

/// The file button: the card's browser. Nothing under it needs drawing, so
/// the sampler panel behind the editor is hidden while it is up.
void file_cb(lv_event_t*)
{
    if (!pad() || s_overlay) return;
    stop_loop();
    lv_obj_t* parent = s_app ? s_app->getAppContainer() : s_cont;
    lv_obj_t* explorer = lv_CreateFileExplorer(parent, FILTER_AUDIO, DEFAULT_KIT_PATH, load_cb, nullptr);
    if (!explorer) return;
    lv_timer_pause(s_timer);
    lv_obj_t* panel = lv_obj_get_parent(s_cont);
    const bool sibling = parent != s_cont;
    if (sibling && panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    /* Load, Close or an app exit: whichever way it goes, the editor wakes. */
    lv_obj_add_event_cb(explorer, [](lv_event_t*) {
        if (!s_cont) return;
        if (lv_obj_t* p = lv_obj_get_parent(s_cont)) lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
        if (s_timer) lv_timer_resume(s_timer);
        lv_indev_set_group(encoder(), s_group);
    }, LV_EVENT_DELETE, nullptr);
}

// ── the timer ──────────────────────────────────────────────────────────────

void tick_cb(lv_timer_t*)
{
    /* The pad you hit is the pad you edit -- or, in COPY, where it goes. */
    if (const int hit = g_padPressPending.exchange(0, std::memory_order_relaxed)) {
        /* While a take runs the pads are the material, not the navigation: a
         * hit that moved the editor would land the recording on whichever pad
         * was struck last instead of the one it was started for. */
        if (s_rec != Rec::Off) { /* played into the take, nothing to select */ }
        else if (s_copying && !s_overlay && hit - 1 != s_copyFrom) ask_copy(hit - 1);
        else if (!s_copying) lv_subject_set_int(&s_pad_sampler, hit);
    }
    const int shown = lv_subject_get_int(&s_pad_sampler) - 1;
    if (shown >= 0 && shown < KIT_PADS && shown != s_pad && !s_overlay) open_pad(shown);

    auto* st = tools();

    /* A take in flight. stopRecording() returns before the file is closed, so
     * the status is what says when there is something to use. */
    if (st && s_rec != Rec::Off) {
        show_meter();
        const crosspad::RecStatus rs = st->recStatus();
        if (!rs.recording && !rs.waiting) {
            const bool stopped = s_rec == Rec::Stopping;
            s_rec = Rec::Off;
            show_rec();
            if (!stopped) {
                /* It ended on its own -- the card filled, or the writer gave
                 * up. Whatever landed is still worth keeping. */
                set_text(s_info, rs.error.empty() ? "The take stopped by itself." : rs.error.c_str());
            }
            if (!rs.file.empty() && rs.frames && put_on_pad(rs.file, false)) {
                /* The picture is on its way: trim it when it lands. */
                s_trimPending = true;
            } else if (rs.file.empty() || !rs.frames) {
                set_text(s_info, rs.error.empty() ? "Nothing was recorded." : rs.error.c_str());
            }
        }
    }

    if (st && st->job() != crosspad::ToolJob::Busy) {
        if (s_ovPending) {
            s_ovPending = false;
            if (st->job() == crosspad::ToolJob::Ready) {
                crosspad::WaveOverview ov = st->overview();
                if (ov.file == s_file) {
                    s_ov = std::move(ov);
                    s_frames = s_ov.frames;
                    if (s_ov.sampleRate) s_rate = s_ov.sampleRate;
                    if (s_loading) {
                        /* End 0, or past the file, is "to the end". */
                        if (!s_end || s_end > s_frames) s_end = s_frames;
                        if (s_start >= s_end) s_start = 0;
                        s_loading = false;
                    }
                    /* A take of our own: the markers go on the sound rather
                     * than on the file, so the pad plays from the attack the
                     * moment it is struck. Written into the pad, not the file
                     * -- the take stays whole on the card and the trim is two
                     * numbers the markers can still be turned by hand. */
                    if (s_trimPending) {
                        s_trimPending = false;
                        const crosspad::SliceMath::TrimPoints t =
                            crosspad::SliceMath::autoTrim(s_ov.envelope, s_ov.hop, s_ov.start,
                                                          s_frames, s_rate);
                        s_start = t.start;
                        s_end   = t.end;
                        write_trim();
                        show_info();
                        set_text(s_info, t.start || t.end != s_frames
                                             ? "Recorded, trimmed to the sound."
                                             : "Recorded; nothing to trim off it.");
                    }
                }
                show_info();
            } else {
                set_text(s_info, st->jobResult().c_str());
            }
            if (s_wave) lv_obj_invalidate(s_wave);
        } else if (s_renderPending) {
            /* The copy is on the card: the pad plays it from now on. */
            s_renderPending = false;
            auto& cbs = sampler_get_platform_callbacks();
            if (st->job() == crosspad::ToolJob::Ready) {
                if (cbs.audio.reloadPad && s_renderPad >= 0) cbs.audio.reloadPad((uint8_t)s_renderPad);
                show_info();
            } else {
                set_text(s_info, st->jobResult().c_str());
            }
        } else if (s_viewWanted) {
            request_view(s_wantA, s_wantB);
        } else if (s_renderWanted && st->requestRendered(s_renderSrc, s_renderOut, pad_fx(s_renderPad))) {
            s_renderWanted = false;
            s_renderPending = true;
        }
    }
    show_busy();

    /* PITCH, HP, LP have stopped: back to the sample, or its copy asked for. */
    if (s_fxMoved && lv_tick_elaps(s_fxAt) >= kFxSettleMs) {
        s_fxMoved = false;
        KitInfo* k = kit();
        const crosspad::SampleFx fx = pad_fx(s_renderPad);
        auto& cbs = sampler_get_platform_callbacks();
        if (k && s_renderPad >= 0 && k->pads[s_renderPad].GetSampleCount() && fx.any()) {
            const std::string src = crosspad::resolveKitSamplePath(k->path, k->pads[s_renderPad].samples[0].sample);
            s_renderSrc = crosspad::kitFsPath(src);
            s_renderOut = crosspad::kitFsPath(crosspad::renderedSamplePath(src, fx));
            s_renderWanted = true;
        } else if (cbs.audio.reloadPad && s_renderPad >= 0) {
            cbs.audio.reloadPad((uint8_t)s_renderPad);
        }
        show_info();
    }

    /* A marker that has stopped: the pad takes the trim, a loop restarts. */
    if (s_moved && lv_tick_elaps(s_movedAt) >= kSettleMs) {
        s_moved = false;
        auto& cbs = sampler_get_platform_callbacks();
        if (cbs.audio.reloadPad && s_pad >= 0) cbs.audio.reloadPad((uint8_t)s_pad);
        if (s_looping && st) st->previewAs(s_file, s_start, s_end, true, pad_sound());
    }
    if ((s_looping || s_playing) && st && !st->previewing()) { s_looping = s_playing = false; show_pad(); }
    move_head();
}

// ── building ───────────────────────────────────────────────────────────────

void style_button(lv_obj_t* b, int32_t w, int32_t h)
{
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, kPanel, 0);
    lv_obj_set_style_bg_color(b, kAccent, LV_STATE_EDITED);
    /* On the button, not the label: only the button is ever EDITED, and the
     * label takes the colour from it -- dark on the accent while turned. */
    lv_obj_set_style_text_color(b, kText, 0);
    lv_obj_set_style_text_color(b, kInk, LV_STATE_EDITED);
    lv_obj_set_style_outline_color(b, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
}

lv_obj_t* add_label(lv_obj_t* b, const char* text)
{
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(l, LV_PCT(100), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return l;
}

lv_obj_t* make_button(lv_obj_t* parent, int32_t w, const char* text, lv_event_cb_t cb)
{
    lv_obj_t* b = lv_button_create(parent);
    style_button(b, w, 22);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    add_label(b, text);
    lv_group_add_obj(s_group, b);
    return b;
}

/// A value the encoder turns (number_step).
lv_obj_t* make_number(lv_obj_t* parent, int32_t w, lv_event_cb_t cb)
{
    lv_obj_t* b = lv_CreateEditableButton(parent);
    style_button(b, w, 22);
    lv_obj_add_event_cb(b, cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = add_label(b, "");
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_group_add_obj(s_group, b);
    return b;
}

lv_obj_t* make_marker(lv_obj_t* parent, const char* text, bool isStart)
{
    lv_obj_t* b = lv_CreateEditableButton(parent);
    style_button(b, 56, 22);
    void* ud = isStart ? (void*)1 : nullptr;
    lv_obj_add_event_cb(b, marker_cb, LV_EVENT_KEY, ud);
    lv_obj_add_event_cb(b, marker_cb, LV_EVENT_CLICKED, ud);
    lv_obj_add_event_cb(b, marker_cb, LV_EVENT_FOCUSED, ud);
    add_label(b, text);
    lv_group_add_obj(s_group, b);
    return b;
}

lv_obj_t* make_row(lv_obj_t* parent, int32_t h)
{
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 3, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/// The power button's click: a value being turned stops first, then the
/// editor closes -- one level, back to the sampler panel.
bool back_cb(void*)
{
    if (!s_cont) return false;
    if (s_overlay) {
        close_overlay();
        s_copying = false;
        show_pad();
        show_info();
        return true;
    }
    if (s_group && lv_group_get_editing(s_group)) {
        if (s_editStart == Edit::Fine || s_editEnd == Edit::Fine) request_view(0, 0);
        s_editStart = s_editEnd = Edit::None;
        lv_group_set_editing(s_group, false);
        show_marker_labels();
        show_pad();
        return true;
    }
    lv_obj_delete_async(s_cont);
    return true;
}

void destroy_cb(lv_event_t*)
{
    sampler_panel_set_covered(false);   // the panel is on show again
    crosspad_gui::ui_back_remove(s_backToken);
    s_backToken = 0;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (auto* st = tools()) st->stopPreview();
    /* A take still running is finished and kept -- the same as the Recorder
     * does on its way out -- and the codec input goes back to whatever the
     * capture preset had, or it stays held after this screen is gone. */
    if (auto* st = tools()) {
        if (s_rec != Rec::Off) st->stopRecording();
        st->releaseInput();
    }
    s_rec = Rec::Off;
    s_trimPending = false;

    /* Before the groups go, or the indev reads freed memory. Guarded: this is
     * also reached from the app's own teardown, which deletes the panel's
     * group first. */
    lv_indev_restore_group(encoder(), s_priorGroup);
    s_priorGroup = nullptr;
    if (s_overlayGroup) { lv_group_delete(s_overlayGroup); s_overlayGroup = nullptr; }
    if (s_group) { lv_group_delete(s_group); s_group = nullptr; }

    s_cont = s_wave = s_info = s_padBtn = s_fileBtn = s_revBtn = s_copyBtn = nullptr;
    s_busy = s_busyText = s_busyBar = nullptr;
    s_meter = s_meterText = s_meterBar[0] = s_meterBar[1] = nullptr;
    s_volBtn = s_panBtn = s_pitchBtn = s_hpBtn = s_lpBtn = s_chokeBtn = s_modeBtn = nullptr;
    s_startBtn = s_endBtn = s_loopBtn = s_delBtn = s_overlay = nullptr;
    s_recBtn = nullptr;
    s_pad = -1;
    s_sample.clear();
    s_file.clear();
    s_ov = crosspad::WaveOverview{};
    s_frames = 0;
    s_looping = s_playing = s_moved = s_copying = s_delArmed = s_ovPending = s_viewWanted = s_loading = false;
    /* A pad whose copy with its fx was not made yet: the kit load task makes it
     * and sets the pad up again, off this thread. */
    const int unrendered = s_fxMoved || s_renderWanted || s_renderPending ? s_renderPad : -1;
    s_fxMoved = s_renderWanted = s_renderPending = false;
    s_renderPad = -1;
    s_editStart = s_editEnd = Edit::None;
    s_headX = -1;

    /* Every pad at its own colour again. */
    if (KitInfo* k = kit()) {
        auto& pm = crosspad::getPadManager();
        for (int i = 0; i < KIT_PADS; i++) pm.setPadColor((uint8_t)i, crosspad::HtmlColor(k->pads[i].color));
    }
    /* Only for what needs the sampler set up again; volume, pan and colour
     * were applied as they changed. */
    auto& cbs = sampler_get_platform_callbacks();
    if (cbs.audio.reloadPads) cbs.audio.reloadPads();
    KitInfo* k = kit();
    if (unrendered >= 0 && k && cbs.audio.startLoadKit) {
        k->pads[unrendered].dirty = true;
        cbs.audio.startLoadKit(-1, true);
    }

    crosspad::getPadManager().setActivePadLogic("Sampler");
    if (auto* status = crosspad::getPlatformServices().status) status->isSamplerPlaying = true;
    LV_LOG_INFO("Pad editor destroyed");
}

} // namespace

lv_obj_t* pad_editor_create(lv_obj_t* parent, crosspad_gui::ILvglApp* app)
{
    s_app = app;
    /* The panel behind is fully covered from here on; leaving it polling costs
     * a repaint of whatever the editor draws over it, twenty times a second. */
    sampler_panel_set_covered(true);
    crosspad::getPadManager().setActivePadLogic("Pad editor");
    if (auto* status = crosspad::getPlatformServices().status) status->isSamplerPlaying = false;

    s_group = lv_group_create();
    lv_group_set_wrap(s_group, true);
    s_priorGroup = lv_indev_get_group(encoder());
    lv_indev_set_group(encoder(), s_group);

    s_cont = lv_obj_create(parent);
    lv_obj_set_size(s_cont, lv_obj_get_content_width(parent), lv_obj_get_content_height(parent));
    lv_obj_center(s_cont);
    lv_obj_set_style_bg_color(s_cont, kBg, 0);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cont, 0, 0);
    lv_obj_set_style_radius(s_cont, 0, 0);
    lv_obj_set_style_pad_all(s_cont, 5, 0);
    lv_obj_set_style_pad_row(s_cont, 4, 0);
    lv_obj_set_flex_flow(s_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_cont, destroy_cb, LV_EVENT_DELETE, nullptr);

    // -- the pad and its sample --
    lv_obj_t* top = make_row(s_cont, 22);
    make_button(top, 30, LV_SYMBOL_LEFT, close_cb);
    s_padBtn = make_number(top, 56, color_cb);
    s_fileBtn = make_button(top, 100, "", file_cb);
    lv_obj_set_flex_grow(s_fileBtn, 1);
    s_recBtn = make_button(top, 54, LV_SYMBOL_AUDIO " REC", rec_cb);
    s_revBtn = make_button(top, 34, "REV", rev_cb);
    s_modeBtn = make_button(top, 46, "", mode_cb);
    /* The symbol alone: REC beside it needs the room, and the file button --
     * which grows into whatever is left -- had the least to spare. */
    s_copyBtn = make_button(top, 28, LV_SYMBOL_COPY, copy_cb);

    // -- its settings --
    lv_obj_t* params = make_row(s_cont, 22);
    s_volBtn = make_number(params, 46, vol_cb);
    s_panBtn = make_number(params, 46, pan_cb);
    s_pitchBtn = make_number(params, 56, pitch_cb);
    s_hpBtn = make_number(params, 46, hp_cb);
    s_lpBtn = make_number(params, 46, lp_cb);
    s_chokeBtn = make_number(params, 48, choke_cb);

    // -- the sample --
    s_wave = lv_obj_create(s_cont);
    lv_obj_remove_style_all(s_wave);
    lv_obj_set_width(s_wave, LV_PCT(100));
    lv_obj_set_flex_grow(s_wave, 1);
    lv_obj_set_style_bg_color(s_wave, kPanel, 0);
    lv_obj_set_style_bg_opa(s_wave, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_wave, 6, 0);
    lv_obj_set_style_pad_all(s_wave, 3, 0);
    lv_obj_remove_flag(s_wave, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_wave, wave_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);

    /* The input, over the picture: while a take runs there is nothing to read
     * in the old waveform anyway, and this is where the eye already is. */
    s_meter = lv_obj_create(s_wave);
    lv_obj_remove_style_all(s_meter);
    lv_obj_set_size(s_meter, LV_PCT(70), LV_SIZE_CONTENT);
    lv_obj_align(s_meter, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(s_meter, kBg, 0);
    lv_obj_set_style_bg_opa(s_meter, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_meter, 4, 0);
    lv_obj_set_style_pad_hor(s_meter, 6, 0);
    lv_obj_set_style_pad_ver(s_meter, 3, 0);
    lv_obj_set_style_pad_row(s_meter, 2, 0);
    lv_obj_set_flex_flow(s_meter, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_meter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_meter, LV_OBJ_FLAG_HIDDEN);
    s_meterText = lv_label_create(s_meter);
    lv_obj_set_style_text_font(s_meterText, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_meterText, lv_color_hex(0xFF5C5C), 0);
    lv_label_set_text(s_meterText, LV_SYMBOL_AUDIO " 0:00.0");
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* b = lv_bar_create(s_meter);
        lv_obj_set_size(b, LV_PCT(100), 4);
        lv_bar_set_range(b, 0, 100);
        lv_obj_set_style_radius(b, 2, 0);
        lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(b, kPanel, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, kAccent, LV_PART_INDICATOR);
        lv_obj_set_style_anim_duration(b, 0, 0);
        s_meterBar[i] = b;
    }

    s_busy = lv_obj_create(s_wave);
    lv_obj_remove_style_all(s_busy);
    lv_obj_set_size(s_busy, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_busy, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_color(s_busy, kBg, 0);
    lv_obj_set_style_bg_opa(s_busy, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_busy, 4, 0);
    lv_obj_set_style_border_color(s_busy, kAccent, 0);
    lv_obj_set_style_border_width(s_busy, 1, 0);
    lv_obj_set_style_pad_hor(s_busy, 6, 0);
    lv_obj_set_style_pad_ver(s_busy, 3, 0);
    lv_obj_set_style_pad_row(s_busy, 2, 0);
    lv_obj_set_flex_flow(s_busy, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_busy, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_busy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_busy, LV_OBJ_FLAG_CLICKABLE);
    s_busyText = lv_label_create(s_busy);
    lv_obj_set_style_text_font(s_busyText, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_busyText, kAccent, 0);
    lv_label_set_text(s_busyText, "");
    s_busyBar = lv_bar_create(s_busy);
    lv_obj_set_size(s_busyBar, LV_PCT(100), 3);
    lv_bar_set_range(s_busyBar, 0, 100);
    lv_obj_set_style_bg_color(s_busyBar, kWaveOff, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_busyBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_busyBar, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_busyBar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_busyBar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(s_busyBar, 0, 0);
    lv_obj_add_flag(s_busy, LV_OBJ_FLAG_HIDDEN);

    s_info = lv_label_create(s_cont);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_info, kDim, 0);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_info, LV_PCT(100), 12);     // one line: a long one ends in dots, the picture keeps its height
    lv_label_set_text(s_info, "");

    // -- the trim --
    lv_obj_t* tools_row = make_row(s_cont, 24);
    s_startBtn = make_marker(tools_row, "START", true);
    s_endBtn = make_marker(tools_row, "END", false);
    make_button(tools_row, 44, LV_SYMBOL_PLAY " PLAY", play_cb);
    s_loopBtn = make_button(tools_row, 48, LV_SYMBOL_LOOP " LOOP", loop_cb);
    make_button(tools_row, 36, "FULL", full_cb);
    s_delBtn = make_button(tools_row, 36, "DEL", del_cb);

    s_pad = -1;
    const int shown = lv_subject_get_int(&s_pad_sampler) - 1;
    if (shown >= 0 && shown < KIT_PADS) open_pad(shown);
    else { show_pad(); show_marker_labels(); show_info(); }
    lv_group_focus_obj(s_startBtn);

    s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);
    s_backToken = crosspad_gui::ui_back_push(back_cb, nullptr);
    return s_cont;
}

void pad_editor_destroy(lv_obj_t* editor)
{
    if (editor) lv_obj_delete(editor);
}

void pad_editor_set_pad(lv_obj_t*, uint8_t padIdx)
{
    lv_subject_set_int(&s_pad_sampler, padIdx + 1);
}

} // namespace crosspad_sampler

#endif // USE_LVGL
