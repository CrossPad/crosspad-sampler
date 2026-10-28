// SPDX-License-Identifier: GPL-3.0-or-later
//
// Kit browser (UX 2000) -- five lists on the left, five rows of the chosen one
// on the right, what the highlighted thing is across the top, LOAD and EDIT
// along the bottom. The design is LVGL PROTO/lvgl_kitsel.
//
// The kits are filed by the folder they are in on the card (KitFolders): KITS
// /crosspad/kits, DRUMS /crosspad/drums, SLICE /crosspad/slicer, where the
// Slicer writes; a kit anywhere else by its kit.json, as before (a "drums"
// category, a Slice engine). PITCH the instruments saved in Pitch Play, in
// their groups (InstrumentGroups: LOAD opens one, back leaves it), and the
// pitched kits; SONGS the projects on the card; USER the kits made on the
// board before PERF and DRUMS could make them, and the Recorder's takes. PERF
// starts with + NEW KIT and DRUMS with + NEW DRUMS: an empty kit of one's own
// in that list's folder, and on that list. LOAD loads a kit (and goes to the
// Sampler), opens an instrument in Pitch Play, a song in Arrange, a take in
// the Slicer. EDIT is the highlighted thing's card: what it is, where it is,
// and for a kit of one's own its name, author, note and tempo to change, and
// DELETE.
//
// The encoder turns the list and a click loads; a long press leaves the list
// for the keys (LVGL's own edit mode). The pads play the highlighted kit --
// its kit.json is read on a worker of its own, never on this thread -- and
// light in its colours.
//
// Picking for someone (KitSelectorConfig::onPicked: a song's kit track) shows
// only the kits, and LOAD is PICK.

#ifdef USE_LVGL

#include <crosspad-sampler/ui/kit_selector.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/logic/kit_load_task.hpp>
#include <crosspad-sampler/ui/sampler_styles.hpp>

#include <crosspad/CrosspadCoreVersion.hpp>
#include <crosspad/instrument/IInstrumentHost.hpp>
#include <crosspad/instrument/InstrumentGroups.hpp>
#include <crosspad/instrument/InstrumentPlayState.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitFolders.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/pad/IPadLogicHandler.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#if CROSSPAD_CORE_VERSION_MAJOR >= 2
  #include <crosspad/platform/IFileSystem.hpp>
#endif
#include <crosspad/sampletools/ISampleTools.hpp>
#include <crosspad/song/ISongEngine.hpp>
#include <crosspad-gui/crosspad_gui.h>
#include <crosspad-gui/widgets/gui_helpers.h>

#include "crosspad-sampler/port/SamplerRtos.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#if CROSSPAD_CORE_VERSION_MAJOR < 2
  #include <cerrno>
  #include <sys/stat.h>
#endif

namespace crosspad_sampler {

namespace {

// ── Look ─────────────────────────────────────────────────────────────────────

const lv_color_t kBg    = lv_color_hex(0x16181B);
const lv_color_t kPanel = lv_color_hex(0x20242A);
const lv_color_t kPress = lv_color_hex(0x2C3138);
const lv_color_t kText  = lv_color_hex(0xE8EAED);
const lv_color_t kDim   = lv_color_hex(0x7B828B);
const lv_color_t kInk   = lv_color_hex(0x06120C);
const lv_color_t kLime  = lv_color_hex(0xC6FF47);
const lv_color_t kMint  = lv_color_hex(0x00FF88);
const lv_color_t kRed   = lv_color_hex(0xFF5C5C);

/* The 200 px under the 40 px top bar: the context row (4..26), the list and
 * the six type keys (30..166), LOAD and EDIT (170..198). */
constexpr int32_t kRows    = 5;
constexpr int32_t kRowH    = 27;
constexpr int32_t kListX   = 80;
constexpr int32_t kListW   = 230;
constexpr int32_t kBodyY   = 30;
constexpr int32_t kKeysY   = 170;
constexpr int32_t kKeyW    = 154;
constexpr int32_t kKeyH    = 28;
constexpr int32_t kTypeW   = 72;
constexpr int32_t kTypeH   = 21;
constexpr int32_t kTypeGap = 23;

/* The kit types: Performance (long and short samples), Drums (short hits,
 * kDrumVoices voices), Slice (one WAV in regions, one voice), Pitch (one
 * sample across the pads) -- then songs and what the player made. */
enum Type : uint8_t { Kits, Drums, Slice, Pitch, Songs, User, kTypes };
const uint32_t    kTypeCol[kTypes]  = {0xC6FF47, 0xFFC857, 0xFF8A3D, 0xA77BF0, 0xF06FD0, 0x4FE0B0};
const char* const kTypeName[kTypes] = {"PERF", "DRUMS", "SLICE", "PITCH", "SONGS", "USER"};

/// What a row stands for, and what LOAD does with it.
enum class Kind : uint8_t {
    Kit,        ///< a sampler kit: loaded, then the Sampler
    PitchedKit, ///< a kit with "engine": "pitched": loaded the same way
    Inst,       ///< an instrument saved in Pitch Play: opened there
    Song,       ///< a project: opened, then Arrange
    Wav,        ///< a Recorder take: the Slicer
    NewKit,     ///< + NEW KIT / + NEW DRUMS: an empty kit of one's own, in the folder in path
    Group,      ///< a group of instruments (InstrumentGroups): LOAD opens it
};

struct Item {
    Kind        kind  = Kind::Kit;
    std::string name;
    std::string path;     ///< kit/instrument: logical JSON; take: full path; a group's name
    int         kitId = -1;
    uint16_t    count = 0;    ///< a group's instruments
};

// ── Timing ───────────────────────────────────────────────────────────────────

constexpr uint32_t kTickMs     = 60;
/// The highlighted thing is heard once the list has stood still this long: a
/// spin through the list plays nothing it passed.
constexpr uint32_t kPreviewMs  = 400;
/// A message in the context row, before the row is the item's again.
constexpr uint32_t kMessageMs  = 2500;

// ── State ────────────────────────────────────────────────────────────────────

KitSelectedFn s_onPicked   = nullptr;
KitSelectedFn s_onSelected = nullptr;
KitLoadedFn   s_onLoaded   = nullptr;
KitClosedFn   s_onClose    = nullptr;
KitOpenAppFn  s_onOpenApp  = nullptr;
void*         s_userData   = nullptr;

lv_obj_t* s_overlay = nullptr;
lv_obj_t* s_info    = nullptr;
lv_obj_t* s_typ[kTypes] = {};
lv_obj_t* s_typLbl[kTypes] = {};
lv_obj_t* s_typBar[kTypes] = {};
lv_obj_t* s_list = nullptr;   ///< the knob: the encoder turns it, a finger drags it
lv_obj_t* s_row[kRows] = {};
lv_obj_t* s_rowName[kRows] = {};
lv_obj_t* s_rowMeta[kRows] = {};
lv_obj_t* s_rowLd[kRows] = {};
lv_obj_t* s_rowEq[kRows] = {};
lv_obj_t* s_sb = nullptr;
lv_obj_t* s_sbThumb = nullptr;
lv_obj_t* s_load = nullptr;
lv_obj_t* s_loadFill = nullptr;
lv_obj_t* s_loadLbl = nullptr;
lv_obj_t* s_edit = nullptr;

lv_group_t* s_group = nullptr;
lv_group_t* s_prevGroup = nullptr;
lv_timer_t* s_timer = nullptr;
uint32_t    s_backToken = 0;

std::vector<Item> s_items[kTypes];
uint16_t s_sel[kTypes] = {};
uint16_t s_off[kTypes] = {};
bool     s_seen[kTypes] = {};
uint8_t  s_type = Kits;

/* What came off the card, kept apart from the lists so a rebuild (a kit made,
 * renamed, deleted) does not have to read it again. */
std::vector<std::string> s_insts;   ///< instrument folder names
/// The group PITCH shows the instruments of; empty for the groups themselves.
std::string s_pitchGroup;
std::vector<std::string> s_takes;   ///< full paths
std::vector<std::string> s_songs;
uint32_t s_songsGen = 0;
bool     s_songsAsked = false;
enum class Listing : uint8_t { Library, Takes, Done };
Listing  s_listing = Listing::Library;
bool     s_listAsked = false;

/* Loading. */
bool        s_kitLoading = false;
int         s_loadingKit = -1;
std::string s_songOpening;
uint32_t    s_songAskedMs = 0;
const char* s_follow = nullptr;     ///< the app to open once the song is open
/// How long a song may take to open before LOAD gives up on it.
constexpr uint32_t kSongOpenMs = 15000;

/* Preview: what is heard, and the row it belongs to. */
uint32_t    s_movedMs = 0;
bool        s_previewDue = false;
std::string s_previewKey;           ///< item path whose sound is playing
std::string s_message;
uint32_t    s_messageMs = 0;

/* A touch drag over the list. */
int32_t s_dragAcc = 0;
bool    s_dragged = false;

/* The card (EDIT). */
enum class Field : uint8_t { Name, Author, Note, Bpm, Count };
constexpr int kFields = (int)Field::Count;
const char* const kFieldTag[kFields]   = {"NAME", "BY", "NOTE", "BPM"};
const char* const kFieldTitle[kFields] = {"What the kit is called", "Who made it",
                                          "What it says about itself", "Tempo it counts, BPM"};
const uint32_t    kFieldMax[kFields]   = {32, 24, 64, 3};

lv_obj_t*   s_card      = nullptr;
lv_group_t* s_cardGroup = nullptr;
lv_obj_t*   s_cardRow[kFields] = {};
lv_obj_t*   s_cardVal[kFields] = {};
lv_obj_t*   s_cardTitle = nullptr;
lv_obj_t*   s_delRow    = nullptr;
lv_obj_t*   s_typing    = nullptr;
lv_group_t* s_typeGroup = nullptr;
lv_obj_t*   s_typeArea  = nullptr;
int         s_cardKit   = -1;
Field       s_typeField = Field::Name;
bool        s_delArmed  = false;

// ── Small things ─────────────────────────────────────────────────────────────

lv_indev_t* encoder() { return crosspad_gui::getGuiPlatform().getNavigationEncoder(); }
crosspad::IKitManager* kits() { return crosspad::getKitManager(); }
crosspad::ISampleTools* tools() { return crosspad::getPlatformServices().sampleTools; }
crosspad::ISongEngine* songs() { return crosspad::getPlatformServices().songEngine; }
crosspad::IInstrumentHost* host() { return crosspad::getPlatformServices().instrumentHost; }
bool picking() { return s_onPicked != nullptr; }

bool ends_wav(const char* n) {
    const size_t l = std::strlen(n);
    if (l < 4) return false;
    const char* e = n + l - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'w' && (e[2] | 0x20) == 'a' && (e[3] | 0x20) == 'v';
}

std::string base_of(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string dir_of(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

/// A name for a recoloured label: '#' would start a colour.
std::string plain(const std::string& s) {
    std::string out = s;
    for (char& c : out) if (c == '#') c = ' ';
    return out;
}

lv_obj_t* box(lv_obj_t* p, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t c) {
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 4, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* txt(lv_obj_t* p, const lv_font_t* f, lv_color_t c, const char* s) {
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, s);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/// A key: pressed shade, the white outline the encoder's focus draws, CLICKED.
lv_obj_t* key(lv_obj_t* p, int32_t x, int32_t y, int32_t w, int32_t h, lv_event_cb_t cb, void* ud) {
    lv_obj_t* b = box(p, x, y, w, h, kPanel);
    lv_obj_set_style_bg_color(b, kPress, LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(b, lv_color_white(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void say(const char* msg) {
    s_message = msg ? msg : "";
    s_messageMs = lv_tick_get();
}

const Item* item_at(uint8_t t, uint16_t i) { return i < s_items[t].size() ? &s_items[t][i] : nullptr; }
const Item* cur() { return item_at(s_type, s_sel[s_type]); }

bool type_enabled(uint8_t t) { return !picking() || (t != Pitch && t != Songs); }

// ── What is loaded ───────────────────────────────────────────────────────────

/// The open song's folder name, read once per paint (is_loaded() asks it per row).
std::string s_openSong;

std::string open_song_name() {
    crosspad::ISongEngine* se = songs();
    if (!se || se->openState() != crosspad::SongOpState::Ready) return {};
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    return base_of(se->song().dir);
}

bool is_loaded(const Item* it) {
    if (!it) return false;
    switch (it->kind) {
        case Kind::Kit:
        case Kind::PitchedKit: {
            auto* km = kits();
            return km && it->kitId >= 0 && it->kitId == km->getCurrentKitId();
        }
        case Kind::Inst: return !it->path.empty() && crosspad::instrumentPlayState().instrumentPath == it->path;
        case Kind::Song: return !s_openSong.empty() && it->name == s_openSong;
        case Kind::Group: {
            const std::string& in = crosspad::instrumentPlayState().instrumentPath;
            return !in.empty() && crosspad::InstrumentGroups::group(base_of(dir_of(in))) == it->path;
        }
        default:         return false;
    }
}

bool type_has_loaded(uint8_t t) {
    for (const Item& it : s_items[t]) if (is_loaded(&it)) return true;
    return false;
}

// ── The kit under the highlight, read off the card ──────────────────────────
//
// One kit.json read on a worker: pads, colours and samples of a kit that is not
// loaded, for the pads to play and light. A newer ask replaces an older one
// that has not started; the answer names the path it is for.

struct Peek {
    std::mutex m;
    std::string want;
    uint32_t wantGen = 0;
    std::unique_ptr<crosspad::KitInfo> kit;   ///< the answer, for donePath
    std::string donePath;
    bool ok = false;
    std::atomic<uint32_t> doneGen{0};
};
Peek s_peek;
uint32_t s_peekSeen = 0;
std::unique_ptr<crosspad::KitInfo> s_peekKit;   ///< LVGL thread's copy
std::string s_peekPath;

crosspad_sampler::SamplerTask   s_peekTask;
crosspad_sampler::SamplerSignal s_peekWake;

void peek_fn(void*) {
    for (;;) {
        if (!s_peekWake.take(crosspad_sampler::kWaitForever)) continue;
        std::string path;
        uint32_t gen = 0;
        {
            std::lock_guard<std::mutex> l(s_peek.m);
            path = s_peek.want;
            gen = s_peek.wantGen;
        }
        if (path.empty()) continue;
        auto kit = std::make_unique<crosspad::KitInfo>();
        crosspad::IKitManager* km = kits();
        const bool ok = km && km->readKitCopy(path, *kit);
        std::lock_guard<std::mutex> l(s_peek.m);
        s_peek.kit = std::move(kit);
        s_peek.donePath = path;
        s_peek.ok = ok;
        s_peek.doneGen.store(gen, std::memory_order_release);
    }
}

/* Started once and never stopped: on the board its stack is in PSRAM, freed
 * only by vTaskDeleteWithCaps(), and one per browser visit would leak it. */
bool peek_worker() {
    if (s_peekTask.running()) return true;
    if (!s_peekWake.valid() && !s_peekWake.create()) return false;
    return s_peekTask.start(peek_fn, nullptr, "kit_peek", 6144, 2, 1);
}

void peek_ask(const std::string& path) {
    if (path == s_peekPath) return;
    if (!peek_worker()) return;
    {
        std::lock_guard<std::mutex> l(s_peek.m);
        s_peek.want = path;
        ++s_peek.wantGen;
    }
    s_peekWake.give();
}

/// The answer, once it is in: true when s_peekKit changed.
bool peek_take() {
    const uint32_t g = s_peek.doneGen.load(std::memory_order_acquire);
    if (g == s_peekSeen) return false;
    s_peekSeen = g;
    std::lock_guard<std::mutex> l(s_peek.m);
    if (!s_peek.ok || !s_peek.kit) return false;
    s_peekKit = std::move(s_peek.kit);
    s_peekPath = s_peek.donePath;
    return true;
}

/// The highlighted kit's pads: the loaded one's own, or what the worker read.
const crosspad::KitInfo* kit_of(const Item* it) {
    if (!it || (it->kind != Kind::Kit && it->kind != Kind::PitchedKit)) return nullptr;
    auto* km = kits();
    if (km && it->kitId >= 0 && it->kitId == km->getCurrentKitId()) return km->getCurrentKit();
    return s_peekKit && s_peekPath == it->path ? s_peekKit.get() : nullptr;
}

int pads_with_samples(const crosspad::KitInfo& k) {
    int n = 0;
    for (const auto& p : k.pads) n += p.sampleCount > 0 && !p.samples.empty() ? 1 : 0;
    return n;
}

// ── Sound ────────────────────────────────────────────────────────────────────

void preview_stop() {
    if (auto* st = tools()) st->stopPreview();
    s_previewKey.clear();
}

/// Pad @p pad (0..15) of the highlighted kit, or the take, through the sample
/// tools' audition.
bool audition(const Item* it, int pad) {
    crosspad::ISampleTools* st = tools();
    if (!st || !it) return false;
    if (it->kind == Kind::Wav) {
        if (!st->preview(it->path, 0, 0, false)) return false;
        s_previewKey = it->path;
        return true;
    }
    const crosspad::KitInfo* k = kit_of(it);
    if (!k) return false;
    if (pad < 0) {
        for (int i = 0; i < KIT_PADS && pad < 0; ++i) {
            if (k->pads[i].sampleCount > 0 && !k->pads[i].samples.empty()) pad = i;
        }
        if (pad < 0) return false;
    }
    const crosspad::Pad& p = k->pads[pad];
    if (p.sampleCount == 0 || p.samples.empty()) return false;
    const crosspad::SampleSlot& s = p.samples[0];
    const std::string file = crosspad::kitFsPath(crosspad::resolveKitSamplePath(k->path, s.sample));
    if (!st->preview(file, s.start, s.end, false)) return false;
    s_previewKey = it->path;
    return true;
}

// ── Pads ─────────────────────────────────────────────────────────────────────

uint32_t pad_rgb(const Item* it, int pad) {
    if (!it) return 0;
    switch (it->kind) {
        case Kind::Kit:
        case Kind::PitchedKit: {
            const crosspad::KitInfo* k = kit_of(it);
            if (!k) return 0x101214;
            const crosspad::Pad& p = k->pads[pad];
            return p.sampleCount > 0 ? p.color : 0;
        }
        case Kind::Inst: return pad % 12 == 0 ? 0xA77BF0 : 0x5B4A7E;
        case Kind::Wav:  return 0x4FE0B0;
        default:         return 0;
    }
}

void paint_pads() {
    auto& pm = crosspad::getPadManager();
    const Item* it = cur();
    for (int i = 0; i < KIT_PADS; ++i) {
        const uint32_t c = pad_rgb(it, i);
        pm.setPadColor((uint8_t)i, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
    }
}

void pad_async(void* p) {
    if (!s_overlay || s_card) return;
    audition(cur(), (int)(intptr_t)p);
}

class BrowserPadLogic final : public crosspad::IPadLogicHandler {
public:
    void onActivate(crosspad::PadManager&) override { if (s_overlay) paint_pads(); }
    void onDeactivate(crosspad::PadManager& pm) override {
        for (uint8_t i = 0; i < KIT_PADS; ++i) pm.clearPadColor(i);
    }
    void onPadPress(crosspad::PadManager& pm, uint8_t padIdx, uint8_t) override {
        /* A finger only: the sequencer and a host's notes press pads too. */
        if (pm.pressSource(padIdx) != crosspad::EventSource::PadManager) return;
        crosspad_gui::getGuiPlatform().postToGui(pad_async, (void*)(intptr_t)padIdx);
    }
    void onPadRelease(crosspad::PadManager&, uint8_t) override {}
    void onPadPressure(crosspad::PadManager&, uint8_t, uint8_t) override {}
};
constexpr const char* kPadLogic = "KitBrowser";
bool s_padLogicRegistered = false;
bool s_padLogicPushed = false;

// ── The lists ────────────────────────────────────────────────────────────────

/// Every list again from what is known; the highlight stays on the same thing.
void build_items() {
    std::string keep[kTypes];
    for (int t = 0; t < kTypes; ++t) {
        const Item* it = item_at((uint8_t)t, s_sel[t]);
        if (it) keep[t] = it->kind == Kind::NewKit ? "+" : it->path + "|" + it->name;
        s_items[t].clear();
    }

    if (!picking()) {
        s_items[Kits].push_back(Item{Kind::NewKit, "+ NEW KIT", crosspad::KitFolders::kKits, -1});
        s_items[Drums].push_back(Item{Kind::NewKit, "+ NEW DRUMS", crosspad::KitFolders::kDrums, -1});
    }
    if (auto* km = kits()) {
        for (int i = 0, n = km->getKitCount(); i < n; ++i) {
            const crosspad::KitInfo* k = km->getKit(i);
            if (!k || k->path.empty()) continue;
            Item it{Kind::Kit, k->name, k->path, i};
            if (k->engine == crosspad::KitEngine::Pitched) {
                if (picking()) continue;
                it.kind = Kind::PitchedKit;
                s_items[Pitch].push_back(it);
            } else if (k->engine == crosspad::KitEngine::Slice ||
                       crosspad::KitFolders::in(k->path, crosspad::KitFolders::kSlicer)) {
                s_items[Slice].push_back(it);
            } else if (crosspad::KitFolders::in(k->path, crosspad::KitFolders::kDrums)) {
                s_items[Drums].push_back(it);
            } else if (k->own && crosspad::KitFolders::in(k->path, crosspad::KitFolders::kKits)) {
                /* Made with PERF's + NEW KIT: on PERF. A kit of one's own
                 * from before (category "user", the Slicer's old ones) stays
                 * on USER. */
                s_items[Kits].push_back(it);
            } else if (k->isUser()) {
                s_items[User].push_back(it);
            } else if (!crosspad::KitFolders::in(k->path, crosspad::KitFolders::kKits) && k->isDrums()) {
                s_items[Drums].push_back(it);
            } else {
                s_items[Kits].push_back(it);
            }
        }
    }
    if (!picking()) {
        if (auto* h = host()) {
            const std::string root = h->libraryRoot();
            using G = crosspad::InstrumentGroups;
            if (!s_pitchGroup.empty()) {
                /* The open group alone: its instruments by name, the group
                 * left off them. */
                s_items[Pitch].clear();
                std::vector<std::string> names;
                for (const auto& n : s_insts)
                    if (G::group(n) == s_pitchGroup) names.push_back(n);
                std::sort(names.begin(), names.end());
                for (const auto& n : names) {
                    s_items[Pitch].push_back(Item{Kind::Inst, G::shortName(n),
                                                  root + "/" + n + "/" + crosspad::IInstrumentHost::kFileName, -1});
                }
            } else {
                /* The groups first, by name, then the pitched kits. */
                std::vector<Item> groups;
                for (const auto& n : s_insts) {
                    const std::string g = G::group(n);
                    auto at = std::find_if(groups.begin(), groups.end(), [&](const Item& e) { return e.path == g; });
                    if (at == groups.end()) groups.push_back(Item{Kind::Group, g, g, -1, 1});
                    else                    ++at->count;
                }
                std::sort(groups.begin(), groups.end(), [](const Item& a, const Item& b) { return a.path < b.path; });
                s_items[Pitch].insert(s_items[Pitch].begin(), groups.begin(), groups.end());
            }
        }
        for (const auto& s : s_songs) s_items[Songs].push_back(Item{Kind::Song, s, s, -1});
        for (const auto& w : s_takes) s_items[User].push_back(Item{Kind::Wav, base_of(w), w, -1});
    }

    for (int t = 0; t < kTypes; ++t) {
        uint16_t sel = 0;
        for (size_t i = 0; i < s_items[t].size() && !keep[t].empty(); ++i) {
            const Item& it = s_items[t][i];
            const std::string k = it.kind == Kind::NewKit ? "+" : it.path + "|" + it.name;
            if (k == keep[t]) { sel = (uint16_t)i; break; }
        }
        s_sel[t] = sel;
    }
}

/// A list seen for the first time opens on the thing that is loaded in it.
void land_on_loaded(uint8_t t) {
    if (s_seen[t]) return;
    s_seen[t] = true;
    for (size_t i = 0; i < s_items[t].size(); ++i) {
        if (is_loaded(&s_items[t][i])) { s_sel[t] = (uint16_t)i; return; }
    }
}

// ── Painting ─────────────────────────────────────────────────────────────────

void info_text(const Item* it, char* b, size_t n) {
    if (!it) {
        const bool waiting = (s_type == Songs && s_songsAsked) ||
                             ((s_type == Pitch || s_type == User) && s_listing != Listing::Done);
        std::snprintf(b, n, "#7B828B %s#", waiting ? "reading the card ..." : "nothing here");
        return;
    }
    switch (it->kind) {
        case Kind::Kit:
        case Kind::PitchedKit: {
            auto* km = kits();
            const crosspad::KitInfo* meta = km && it->kitId >= 0 ? km->getKit(it->kitId) : nullptr;
            const crosspad::KitInfo* k = kit_of(it);
            char pads[8], bpm[8];
            std::snprintf(pads, sizeof pads, "%d", k ? pads_with_samples(*k) : 0);
            std::snprintf(bpm, sizeof bpm, "%u", meta ? (unsigned)meta->bpm : 0u);
            const std::string by = meta ? plain(meta->author) : std::string();
            std::snprintf(b, n, "#7B828B %s# %s  #7B828B BPM# %s  #7B828B BY# %s",
                          it->kind == Kind::PitchedKit ? "PITCHED" : s_type == Slice ? "SLICES" : "PADS",
                          k ? pads : "-",
                          meta && meta->bpm ? bpm : "-", by.empty() ? "-" : by.c_str());
            break;
        }
        case Kind::Inst:
            std::snprintf(b, n, "#7B828B INSTRUMENT#  #7B828B LOAD# plays it in Pitch Play");
            break;
        case Kind::Song: {
            crosspad::ISongEngine* se = songs();
            if (is_loaded(it) && se) {
                float bpm = 0;
                {
                    std::lock_guard<std::recursive_mutex> l(se->songMutex());
                    bpm = se->song().bpm;
                }
                std::snprintf(b, n, "#7B828B BARS# %u  #7B828B BPM# %d", (unsigned)se->playEndBars(), (int)(bpm + 0.5f));
            } else {
                std::snprintf(b, n, "#7B828B SONG#  #7B828B LOAD# opens it in Arrange");
            }
            break;
        }
        case Kind::Wav:
            std::snprintf(b, n, "#7B828B RECORDING#  #7B828B LOAD# slices it");
            break;
        case Kind::NewKit:
            std::snprintf(b, n, "#7B828B %s#", it->path == crosspad::KitFolders::kDrums
                                                   ? "AN EMPTY DRUM KIT OF YOUR OWN" : "AN EMPTY KIT OF YOUR OWN");
            break;
        case Kind::Group:
            std::snprintf(b, n, "#7B828B GROUP# %u  #7B828B LOAD# opens it", (unsigned)it->count);
            break;
    }
}

void meta_text(const Item& it, char* b, size_t n) {
    b[0] = 0;
    if (it.kind == Kind::PitchedKit) std::snprintf(b, n, "pitched");
    else if (it.kind == Kind::Wav)   std::snprintf(b, n, "wav");
    else if (it.kind == Kind::Group) std::snprintf(b, n, "%u", (unsigned)it.count);
    else if (it.kind == Kind::Song && is_loaded(&it)) {
        if (crosspad::ISongEngine* se = songs()) std::snprintf(b, n, "%u bars", (unsigned)se->playEndBars());
    }
}

/// What the screen shows, in short: the tick paints only when this changes, as
/// every label set and style set is a repaint of what it covers.
struct Shown {
    int kit = -2, progress = -1, items = -1;
    uint32_t instGen = 0, peek = 0;
    bool sounding = false, message = false, opening = false;
    std::string song;
    bool operator==(const Shown& o) const {
        return kit == o.kit && progress == o.progress && items == o.items && instGen == o.instGen &&
               peek == o.peek && sounding == o.sounding && message == o.message &&
               opening == o.opening && song == o.song;
    }
};
Shown s_shown;

Shown shown_now() {
    Shown n;
    auto* km = kits();
    n.kit = km ? km->getCurrentKitId() : -1;
    n.progress = s_kitLoading ? kit_load_task_progress() : -1;
    n.items = 0;
    for (const auto& v : s_items) n.items += (int)v.size();
    n.instGen = crosspad::instrumentPlayState().generation;
    n.peek = s_peekSeen;
    crosspad::ISampleTools* st = tools();
    n.sounding = st && st->previewing() && !s_previewKey.empty();
    n.message = !s_message.empty() && lv_tick_elaps(s_messageMs) < kMessageMs;
    n.opening = !s_songOpening.empty();
    n.song = open_song_name();
    return n;
}

void set_text(lv_obj_t* l, const char* t) {
    if (std::strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

void paint_load();

void refresh() {
    if (!s_overlay) return;
    s_shown = shown_now();
    s_openSong = s_shown.song;
    const uint8_t t = s_type;
    const lv_color_t c = lv_color_hex(kTypeCol[t]);
    const Item* it = cur();
    char b[128];

    /* The context row: a message for a moment, otherwise the item. */
    if (!s_message.empty() && lv_tick_elaps(s_messageMs) < kMessageMs) {
        std::snprintf(b, sizeof b, "%s", s_message.c_str());
    } else {
        s_message.clear();
        info_text(it, b, sizeof b);
        if (is_loaded(it)) {
            const size_t l = std::strlen(b);
            std::snprintf(b + l, sizeof b - l, "   #%06lX LOADED#", (unsigned long)kTypeCol[t]);
        }
    }
    set_text(s_info, b);

    for (int i = 0; i < kTypes; ++i) {
        const bool on = i == t;
        const bool en = type_enabled((uint8_t)i);
        const lv_color_t tc = lv_color_hex(kTypeCol[i]);
        lv_obj_set_style_bg_color(s_typ[i], on ? tc : kPanel, 0);
        lv_obj_set_style_text_color(s_typLbl[i], on ? kInk : tc, 0);
        lv_obj_set_style_text_opa(s_typLbl[i], en ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_bg_color(s_typBar[i], on ? kInk : kLime, 0);
        if (type_has_loaded((uint8_t)i)) lv_obj_remove_flag(s_typBar[i], LV_OBJ_FLAG_HIDDEN);
        else                              lv_obj_add_flag(s_typBar[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* The window of five. */
    const uint16_t n = (uint16_t)s_items[t].size();
    uint16_t sel = s_sel[t] < n ? s_sel[t] : (n ? n - 1 : 0);
    uint16_t off = s_off[t];
    s_sel[t] = sel;
    if (sel < off) off = sel;
    if (sel >= off + kRows) off = (uint16_t)(sel - kRows + 1);
    if (n <= kRows) off = 0;
    else if (off > n - kRows) off = (uint16_t)(n - kRows);
    s_off[t] = off;

    crosspad::ISampleTools* st = tools();
    const bool sounding = st && st->previewing() && !s_previewKey.empty();
    for (int j = 0; j < kRows; ++j) {
        const Item* r = item_at(t, (uint16_t)(off + j));
        lv_obj_t* row = s_row[j];
        if (!r) { lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        const bool s = off + j == sel;
        lv_obj_set_style_bg_opa(row, s ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, s ? 2 : 0, 0);
        lv_obj_set_style_border_color(row, c, 0);
        set_text(s_rowName[j], r->name.c_str());
        lv_obj_set_style_text_color(s_rowName[j], r->kind == Kind::NewKit ? c : kText, 0);
        meta_text(*r, b, sizeof b);
        set_text(s_rowMeta[j], b);
        if (is_loaded(r)) lv_obj_remove_flag(s_rowLd[j], LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag(s_rowLd[j], LV_OBJ_FLAG_HIDDEN);
        if (sounding && r->path == s_previewKey) lv_obj_remove_flag(s_rowEq[j], LV_OBJ_FLAG_HIDDEN);
        else                                     lv_obj_add_flag(s_rowEq[j], LV_OBJ_FLAG_HIDDEN);
    }
    if (n > kRows) {
        const int32_t H = kRows * kRowH - 3;
        int32_t th = H * kRows / n;
        if (th < 14) th = 14;
        lv_obj_remove_flag(s_sb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_sbThumb, th);
        lv_obj_set_y(s_sbThumb, (H - th) * off / (n - kRows));
        lv_obj_set_style_bg_color(s_sbThumb, c, 0);
    } else {
        lv_obj_add_flag(s_sb, LV_OBJ_FLAG_HIDDEN);
    }

    paint_load();
}

/// LOAD: what it will do, how far a load is, or that this one is in.
void paint_load() {
    const Item* it = cur();
    const lv_color_t c = lv_color_hex(kTypeCol[s_type]);
    char b[24];
    const bool kitBusy = s_kitLoading && it && it->kitId == s_loadingKit &&
                         (it->kind == Kind::Kit || it->kind == Kind::PitchedKit);
    const bool songBusy = !s_songOpening.empty() && it && it->kind == Kind::Song && it->name == s_songOpening;
    if (kitBusy || songBusy) {
        const int pct = kitBusy ? kit_load_task_progress() : 50;
        lv_obj_set_style_bg_color(s_load, kPanel, 0);
        lv_obj_remove_flag(s_loadFill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_loadFill, kKeyW * pct / 100);
        lv_obj_set_style_bg_color(s_loadFill, c, 0);
        if (kitBusy) std::snprintf(b, sizeof b, "LOADING %d%%", pct);
        else         std::snprintf(b, sizeof b, "OPENING");
        set_text(s_loadLbl, b);
        lv_obj_set_style_text_color(s_loadLbl, pct > 50 ? kInk : c, 0);
    } else {
        lv_obj_add_flag(s_loadFill, LV_OBJ_FLAG_HIDDEN);
        const bool ld = is_loaded(it) && !picking();
        const char* what = !it ? "LOAD"
                         : picking() ? "PICK"
                         : it->kind == Kind::NewKit ? LV_SYMBOL_PLUS " MAKE"
                         : it->kind == Kind::Wav ? "SLICE"
                         : it->kind == Kind::Group ? "OPEN"
                         : ld ? "LOADED" : "LOAD";
        lv_obj_set_style_bg_color(s_load, ld || !it ? kPanel : c, 0);
        set_text(s_loadLbl, what);
        lv_obj_set_style_text_color(s_loadLbl, ld ? c : (it ? kInk : kDim), 0);
    }
    lv_obj_center(s_loadLbl);
}

// ── Moving ───────────────────────────────────────────────────────────────────

/// The highlight moved: the new thing is asked for, heard once it stands still.
void moved() {
    const Item* it = cur();
    if (it && (it->kind == Kind::Kit || it->kind == Kind::PitchedKit) && !kit_of(it)) peek_ask(it->path);
    s_movedMs = lv_tick_get();
    s_previewDue = it && it->kind != Kind::NewKit;
    if (!s_previewKey.empty() && (!it || it->path != s_previewKey)) preview_stop();
    if (s_padLogicPushed) paint_pads();
    refresh();
}

void step(int d) {
    const int n = (int)s_items[s_type].size();
    if (!n) return;
    int s = (int)s_sel[s_type] + d;
    if (s < 0) s = 0;
    if (s >= n) s = n - 1;
    if (s == s_sel[s_type]) return;
    s_sel[s_type] = (uint16_t)s;
    moved();
}

void focus_list(void*) {
    if (!s_group || !s_list) return;
    lv_group_focus_obj(s_list);
    lv_group_set_editing(s_group, true);
}

void select_type(uint8_t t) {
    if (t >= kTypes || !type_enabled(t)) return;
    land_on_loaded(t);
    s_type = t;
    preview_stop();
    moved();
}

// ── Loading ──────────────────────────────────────────────────────────────────

void open_app(const char* app) {
    if (s_onOpenApp) s_onOpenApp(app, s_userData);
}

void load_kit(const Item& it) {
    auto* km = kits();
    if (!km || it.kitId < 0) return;
    if (picking()) { s_onPicked(it.kitId, s_userData); return; }
    /* The one already in: go and play it. */
    if (it.kitId == km->getCurrentKitId()) {
        if (s_onLoaded) s_onLoaded(s_userData);
        return;
    }
    const auto& cb = sampler_get_platform_callbacks();
    if (!cb.audio.startLoadKit || s_kitLoading) return;
    preview_stop();
    if (s_onSelected) s_onSelected(it.kitId, s_userData);
    cb.audio.startLoadKit(it.kitId, false);
    s_kitLoading = true;
    s_loadingKit = it.kitId;
}

void new_kit(const Item& it);

/// PITCH shows the instruments of group @p g, or the groups again for an
/// empty @p g -- on the group it came out of.
void open_group(std::string g) {
    const std::string left = s_pitchGroup;
    s_pitchGroup = g;
    s_sel[Pitch] = s_off[Pitch] = 0;
    build_items();
    for (size_t i = 0; g.empty() && i < s_items[Pitch].size(); ++i) {
        if (s_items[Pitch][i].kind == Kind::Group && s_items[Pitch][i].path == left) { s_sel[Pitch] = (uint16_t)i; break; }
    }
    moved();
}

void do_load() {
    const Item* it = cur();
    if (!it || s_card) return;
    switch (it->kind) {
        case Kind::Kit:
        case Kind::PitchedKit: load_kit(*it); break;
        case Kind::Inst:
            preview_stop();
            crosspad::instrumentOpenRequest(it->path);
            open_app("PitchPlay");
            break;
        case Kind::Song: {
            if (is_loaded(it)) { open_app("Arrange"); break; }
            crosspad::ISongEngine* se = songs();
            if (!se || !s_songOpening.empty()) break;
            if (!se->requestOpen(it->name)) { say("Busy -- try again"); break; }
            s_songOpening = it->name;
            s_songAskedMs = lv_tick_get();
            s_follow = "Arrange";
            break;
        }
        case Kind::Wav:
            if (auto* st = tools()) {
                preview_stop();
                st->setHandoff(it->path, 0, 0);
                open_app("Slicer");
            }
            break;
        case Kind::NewKit: new_kit(*it); break;
        case Kind::Group:  open_group(it->path); break;
    }
    refresh();
}

// ── + NEW KIT, + NEW DRUMS ───────────────────────────────────────────────────

/// Sixteen hues round the wheel, the colours a sliced kit gets, so a new kit's
/// pads are told apart on the grid instead of being black.
uint32_t pad_hue(int i) {
    const float h = (float)(i % KIT_PADS) / (float)KIT_PADS * 6.0f;
    const int   k = (int)h;
    const float f = h - (float)k;
    float r = 0, g = 0, b = 0;
    switch (k) {
        case 0:  r = 1;     g = f;     break;
        case 1:  r = 1 - f; g = 1;     break;
        case 2:  g = 1;     b = f;     break;
        case 3:  g = 1 - f; b = 1;     break;
        case 4:  r = f;     b = 1;     break;
        default: r = 1;     b = 1 - f; break;
    }
    return ((uint32_t)(r * 255.0f) << 16) | ((uint32_t)(g * 255.0f) << 8) | (uint32_t)(b * 255.0f);
}

/// A kit of one's own, empty: a folder with a kit.json in it, sixteen coloured
/// pads and no samples. What fills it is the pad editor in the Sampler.
///
/// It goes in the folder of the list it was made on (@p it's path: PERF's
/// KitFolders::kKits, DRUMS' kDrums), so it is on that list; a drum kit says
/// "drums", for its voices, and both say they are the user's ("own"). The name
/// is tried against the card as well as against the list: a folder nobody has
/// scanned yet still holds somebody's kit.json, and writing over it would take
/// their kit.
void new_kit(const Item& it) {
    auto* mgr = kits();
    if (!mgr) return;

    const std::string root = it.path;
    const bool drums = root == crosspad::KitFolders::kDrums;
    const std::string base = drums ? "NEW DRUMS" : "NEW KIT";
    const uint8_t list = drums ? Drums : Kits;

    std::string name, path;
    bool free_name = false;
    bool probe_failed = false;
    for (int n = 1; n <= 99 && !free_name; ++n) {
        name = n == 1 ? base : base + " " + std::to_string(n);
        path = root + "/" + name + "/kit.json";
        /* Neither there nor absent -- a card error: counted as taken, so a
         * kit that may be there is never written over. Logged once, not per
         * name: this runs on the LVGL task, and the console blocks it. */
#if CROSSPAD_CORE_VERSION_MAJOR >= 2
        const crosspad::FsStat probe = crosspad::getFileSystem().stat(crosspad::kitFsPath(path).c_str(), nullptr);
        if (probe == crosspad::FsStat::Found) continue;
        if (probe == crosspad::FsStat::Missing) { free_name = true; continue; }
        if (!probe_failed) printf("[KitSel] %s: could not be read\n", path.c_str());
#else
        struct stat st;
        if (stat(crosspad::kitFsPath(path).c_str(), &st) == 0) continue;
        if (errno == ENOENT) { free_name = true; continue; }
        if (!probe_failed) printf("[KitSel] %s: %s\n", path.c_str(), std::strerror(errno));
#endif
        probe_failed = true;
    }
    if (!free_name) { say(probe_failed ? "The card would not read" : "No free name left"); return; }

    crosspad::KitInfo info;
    info.name     = name;
    info.category = drums ? "drums" : "user";   // drums: KitInfo::voiceCap()
    info.own      = true;                       // KitInfo::isUser(): its card edits it
    info.version  = "1.0";
    info.path     = path;
    for (int p = 0; p < KIT_PADS; ++p) info.pads[p].color = info.pads[p].color_pressed = pad_hue(p);
    if (!mgr->saveKit(info)) { say("The card would not write"); return; }
    const int id = mgr->addKit(info.path);

    build_items();
    for (size_t i = 0; i < s_items[list].size() && id >= 0; ++i) {
        if (s_items[list][i].kitId == id) { s_sel[list] = (uint16_t)i; break; }
    }
    say(id >= 0 ? "Made -- EDIT names it, the Sampler fills it" : "Made -- listed after a restart");
    moved();
}

// ── The card (EDIT) ──────────────────────────────────────────────────────────

crosspad::KitInfo* card_kit() {
    auto* mgr = kits();
    if (!mgr || s_cardKit < 0) return nullptr;
    crosspad::KitInfo* k = mgr->getKit(s_cardKit);
    return k && !k->path.empty() ? k : nullptr;
}

void card_paint() {
    crosspad::KitInfo* kit = card_kit();
    if (!kit) return;
    if (s_cardTitle) lv_label_set_text(s_cardTitle, kit->name.c_str());
    char bpm[8];
    std::snprintf(bpm, sizeof(bpm), "%u", (unsigned)kit->bpm);
    const char* value[kFields] = {
        kit->name.c_str(),
        kit->author.empty() ? "--" : kit->author.c_str(),
        kit->description.empty() ? "--" : kit->description.c_str(),
        kit->bpm ? bpm : "--",            // a kit is allowed not to say
    };
    for (int i = 0; i < kFields; ++i) {
        if (s_cardVal[i]) lv_label_set_text(s_cardVal[i], value[i]);
    }
    if (s_delRow) {
        lv_obj_t* l = lv_obj_get_child(s_delRow, 0);
        if (l) lv_label_set_text(l, s_delArmed ? LV_SYMBOL_TRASH "  once more and it is gone"
                                               : LV_SYMBOL_TRASH "  delete this kit");
        lv_obj_set_style_bg_color(s_delRow, s_delArmed ? kRed : kPanel, 0);
        if (l) lv_obj_set_style_text_color(l, s_delArmed ? kInk : kText, 0);
    }
}

void typing_close() {
    if (!s_typing) return;
    if (lv_indev_t* enc = encoder()) lv_indev_set_group(enc, s_cardGroup ? s_cardGroup : s_group);
    /* Deferred: the click that closed it is still being read out of this group. */
    if (s_typeGroup) { lv_group_delete_deferred(s_typeGroup); s_typeGroup = nullptr; }
    lv_obj_delete_async(s_typing);
    s_typing   = nullptr;
    s_typeArea = nullptr;
    if (s_cardRow[(int)s_typeField]) lv_group_focus_obj(s_cardRow[(int)s_typeField]);
}

void card_close() {
    typing_close();
    if (!s_card) return;
    if (lv_indev_t* enc = encoder()) lv_indev_set_group(enc, s_group);
    if (s_cardGroup) { lv_group_delete_deferred(s_cardGroup); s_cardGroup = nullptr; }
    lv_obj_delete_async(s_card);
    s_card = s_cardTitle = s_delRow = nullptr;
    for (int i = 0; i < kFields; ++i) s_cardRow[i] = s_cardVal[i] = nullptr;
    s_delArmed = false;
    s_cardKit = -1;
    /* Back on EDIT, not on the list: the click that closed the card is still
     * being read, and on the list it would load. */
    if (s_edit) lv_group_focus_obj(s_edit);
    if (s_padLogicPushed) paint_pads();
    refresh();
}

bool card_back() {
    if (s_typing) { typing_close(); return true; }
    if (s_card)   { card_close();   return true; }
    return false;
}

/// The keyboard's OK writes the field to the card; its X leaves the kit alone.
/// Each field on its own, so nothing is held unsaved: what the screen says and
/// what the file says are never two different things.
void typed_cb(lv_event_t* e) {
    const bool ok = lv_event_get_code(e) == LV_EVENT_READY;
    const std::string text = s_typeArea ? lv_textarea_get_text(s_typeArea) : "";
    const Field field = s_typeField;
    const int kitId = s_cardKit;
    typing_close();
    if (!ok) return;

    auto* mgr = kits();
    crosspad::KitInfo* kit = card_kit();
    if (!mgr || !kit) return;

    /* Four fields, not the kit: saveKitInfo() keeps the pads off the file it
     * reads, and a KitInfo copied whole here would carry sixteen of them. */
    crosspad::KitInfo meta;
    meta.name        = kit->name;
    meta.author      = kit->author;
    meta.description = kit->description;
    meta.bpm         = kit->bpm;
    switch (field) {
        case Field::Name:
            if (text.empty()) return;          // a kit with no name is no kit
            meta.name = text;
            break;
        case Field::Author: meta.author      = text; break;
        case Field::Note:   meta.description = text; break;
        case Field::Bpm: {
            const int bpm = std::atoi(text.c_str());
            meta.bpm = (uint16_t)(bpm >= 20 && bpm <= 300 ? bpm : 0);   // 0 = does not say
            break;
        }
        default: return;
    }
    if (!mgr->saveKitInfo(kitId, meta)) { say("The card would not write"); return; }
    card_paint();
    build_items();
    say("Saved");
}

/// The field, under a keyboard. A finger types on it; the encoder walks the
/// keys, which is why the keyboard is the one thing in its group and starts
/// edited.
void type_field(Field field) {
    crosspad::KitInfo* kit = card_kit();
    if (!kit || s_typing || !s_card) return;
    s_typeField = field;

    s_typing = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_typing);
    lv_obj_set_size(s_typing, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_typing, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(s_typing, kBg, 0);
    lv_obj_set_style_bg_opa(s_typing, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_typing, 4, 0);
    lv_obj_set_style_pad_row(s_typing, 3, 0);
    lv_obj_set_flex_flow(s_typing, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_typing, LV_OBJ_FLAG_SCROLLABLE);

    s_typeGroup = lv_group_create();
    if (lv_indev_t* enc = encoder()) lv_indev_set_group(enc, s_typeGroup);

    lv_obj_t* title = txt(s_typing, &lv_font_montserrat_10, lv_color_hex(kTypeCol[s_type]),
                          kFieldTitle[(int)field]);
    (void)title;

    char bpm[8];
    std::snprintf(bpm, sizeof(bpm), "%u", (unsigned)kit->bpm);
    const char* start[kFields] = {kit->name.c_str(), kit->author.c_str(),
                                  kit->description.c_str(), kit->bpm ? bpm : ""};
    s_typeArea = lv_textarea_create(s_typing);
    lv_textarea_set_one_line(s_typeArea, true);
    lv_textarea_set_max_length(s_typeArea, kFieldMax[(int)field]);
    lv_textarea_set_text(s_typeArea, start[(int)field]);
    lv_obj_set_width(s_typeArea, LV_PCT(100));

    lv_obj_t* kb = lv_keyboard_create(s_typing);
    lv_keyboard_set_mode(kb, field == Field::Bpm ? LV_KEYBOARD_MODE_NUMBER
                                                 : LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_keyboard_set_textarea(kb, s_typeArea);
    lv_obj_add_event_cb(kb, typed_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, typed_cb, LV_EVENT_CANCEL, nullptr);
    lv_group_add_obj(s_typeGroup, kb);
    lv_group_focus_obj(kb);
    lv_group_set_editing(s_typeGroup, true);
}

void field_cb(lv_event_t* e) { type_field((Field)(intptr_t)lv_event_get_user_data(e)); }

/// Asked once, then done. The kit that is playing cannot go: it is the one the
/// pads are sounding, and the engine reads its samples off the card.
void delete_cb(lv_event_t*) {
    auto* mgr = kits();
    if (!mgr || !card_kit()) return;
    if (s_cardKit == mgr->getCurrentKitId()) {
        if (lv_obj_t* l = s_delRow ? lv_obj_get_child(s_delRow, 0) : nullptr) {
            lv_label_set_text(l, "This kit is playing -- load another first");
        }
        s_delArmed = false;
        return;
    }
    if (!s_delArmed) { s_delArmed = true; card_paint(); return; }
    const int kitId = s_cardKit;
    card_close();
    if (!mgr->removeKit(kitId)) { say("Could not delete it"); return; }
    s_peekPath.clear();
    s_peekKit.reset();
    build_items();
    say("Deleted");
    moved();
}

/// One row of the card: what it is on the left, what it has on the right. With
/// no callback the row is only read: not clickable, not in the encoder's walk.
lv_obj_t* card_row(const char* tag, const char* value, lv_event_cb_t cb, void* ud, lv_obj_t** valueOut) {
    lv_obj_t* row = lv_obj_create(s_card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 24);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_bg_color(row, kPanel, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, kPress, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(row, 7, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_outline_color(row, lv_color_white(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(row, 2, LV_STATE_FOCUSED);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);
        if (s_cardGroup) lv_group_add_obj(s_cardGroup, row);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* name = txt(row, &lv_font_montserrat_10, kDim, tag);
    lv_obj_set_width(name, 40);

    lv_obj_t* val = txt(row, &lv_font_montserrat_10, kText, value ? value : "");
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(val, 1);
    if (cb) {
        lv_obj_t* chev = txt(row, &lv_font_montserrat_10, kDim, LV_SYMBOL_EDIT);
        (void)chev;
    }
    if (valueOut) *valueOut = val;
    return row;
}

/// A full-width key with one label: the card's actions and the way back.
lv_obj_t* card_button(const char* text, lv_event_cb_t cb, bool accent) {
    lv_obj_t* row = lv_obj_create(s_card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 26);
    lv_obj_set_style_radius(row, 4, 0);
    const lv_color_t c = lv_color_hex(kTypeCol[s_type]);
    lv_obj_set_style_bg_color(row, accent ? c : kPanel, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, kPress, LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(row, lv_color_white(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(row, 2, LV_STATE_FOCUSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = txt(row, &lv_font_montserrat_14, accent ? kInk : kText, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_center(l);
    if (s_cardGroup) lv_group_add_obj(s_cardGroup, row);
    return row;
}

void card_load_cb(lv_event_t*) {
    /* The card goes first, deferred like every close here; LOAD then acts on
     * the row the card was opened from. */
    card_close();
    do_load();
}

void card_play_cb(lv_event_t*) { audition(cur(), -1); }

/// A kit made on the board (KitInfo::isUser): its card renames and deletes it.
bool own_kit(const Item* it) {
    auto* km = kits();
    const crosspad::KitInfo* meta = km && it && it->kitId >= 0 ? km->getKit(it->kitId) : nullptr;
    return meta && meta->isUser();
}

/// EDIT: what the highlighted thing is, where it lives, and what it can do.
void open_card() {
    const Item* it = cur();
    if (s_card || !s_overlay || !it || it->kind == Kind::NewKit || it->kind == Kind::Group) return;

    s_delArmed = false;
    s_cardKit = -1;
    s_card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(s_card, kBg, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_card, 4, 0);
    lv_obj_set_style_pad_row(s_card, 4, 0);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_card, LV_SCROLLBAR_MODE_OFF);

    s_cardGroup = lv_group_create();
    if (lv_indev_t* enc = encoder()) lv_indev_set_group(enc, s_cardGroup);

    /* The title: the name, and what kind of thing it is, in the list's colour. */
    lv_obj_t* head = lv_obj_create(s_card);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), 22);
    lv_obj_set_style_radius(head, 4, 0);
    lv_obj_set_style_bg_color(head, kPanel, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* mark = box(head, 2, 4, 3, 14, lv_color_hex(kTypeCol[s_type]));
    lv_obj_set_style_radius(mark, 2, 0);
    s_cardTitle = txt(head, &lv_font_montserrat_14, kText, it->name.c_str());
    lv_label_set_long_mode(s_cardTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_cardTitle, 220);
    lv_obj_align(s_cardTitle, LV_ALIGN_LEFT_MID, 10, 0);
    const char* kind = it->kind == Kind::PitchedKit ? "PITCHED KIT"
                     : it->kind == Kind::Inst ? "INSTRUMENT"
                     : it->kind == Kind::Song ? "SONG"
                     : it->kind == Kind::Wav ? "RECORDING"
                     : s_type == Drums ? (own_kit(it) ? "YOUR DRUM KIT" : "DRUM KIT")
                     : s_type == Slice ? "SLICE KIT"
                     : own_kit(it) ? "YOUR KIT" : "KIT";
    lv_obj_t* kl = txt(head, &lv_font_montserrat_10, lv_color_hex(kTypeCol[s_type]), kind);
    lv_obj_align(kl, LV_ALIGN_RIGHT_MID, -6, 0);

    char b[48];
    lv_obj_t* first = nullptr;
    switch (it->kind) {
        case Kind::Kit:
        case Kind::PitchedKit: {
            auto* km = kits();
            crosspad::KitInfo* meta = km ? km->getKit(it->kitId) : nullptr;
            if (!meta) break;
            s_cardKit = it->kitId;
            /* A kit of one's own can be renamed and retold; a factory kit's
             * folder is not ours to rewrite, so its card is only read. */
            const bool own = meta->isUser();
            for (int i = 0; i < kFields; ++i) {
                s_cardRow[i] = card_row(kFieldTag[i], "", own ? field_cb : nullptr,
                                        (void*)(intptr_t)i, &s_cardVal[i]);
            }
            const crosspad::KitInfo* k = kit_of(it);
            if (k) std::snprintf(b, sizeof b, "%d of 16 with a sample", pads_with_samples(*k));
            else   std::snprintf(b, sizeof b, "--");
            card_row("PADS", b, nullptr, nullptr, nullptr);
            std::snprintf(b, sizeof b, "%s%s%s", meta->version.c_str(),
                          meta->version.empty() || meta->created_at.empty() ? "" : "  ",
                          meta->created_at.c_str());
            card_row("MADE", b[0] ? b : "--", nullptr, nullptr, nullptr);
            card_row("FOLDER", dir_of(meta->path).c_str(), nullptr, nullptr, nullptr);
            const bool ld = is_loaded(it) && !picking();
            first = card_button(picking() ? "PICK" : ld ? "PLAY IT IN THE SAMPLER" : "LOAD", card_load_cb, true);
            if (own && !picking()) s_delRow = card_button("", delete_cb, false);
            card_paint();
            break;
        }
        case Kind::Inst:
            card_row("FOLDER", dir_of(it->path).c_str(), nullptr, nullptr, nullptr);
            card_row("STATE", is_loaded(it) ? "in Pitch Play now" : "saved", nullptr, nullptr, nullptr);
            first = card_button("OPEN IN PITCH PLAY", card_load_cb, true);
            break;
        case Kind::Song: {
            crosspad::ISongEngine* se = songs();
            if (is_loaded(it) && se) {
                float bpm = 0;
                std::string kit;
                {
                    std::lock_guard<std::recursive_mutex> l(se->songMutex());
                    bpm = se->song().bpm;
                    kit = se->song().kit;
                }
                std::snprintf(b, sizeof b, "%u", (unsigned)se->playEndBars());
                card_row("BARS", b, nullptr, nullptr, nullptr);
                std::snprintf(b, sizeof b, "%d", (int)(bpm + 0.5f));
                card_row("BPM", b, nullptr, nullptr, nullptr);
                card_row("KIT", kit.empty() ? "--" : base_of(dir_of(kit)).c_str(), nullptr, nullptr, nullptr);
            } else {
                card_row("STATE", "on the card -- open it to see more", nullptr, nullptr, nullptr);
            }
            first = card_button("OPEN IN ARRANGE", card_load_cb, true);
            break;
        }
        case Kind::Wav:
            card_row("FOLDER", dir_of(it->path).c_str(), nullptr, nullptr, nullptr);
            first = card_button(LV_SYMBOL_PLAY "  PLAY", card_play_cb, false);
            card_button("SLICE IT", card_load_cb, true);
            break;
        case Kind::NewKit:
        case Kind::Group:  break;   // no card: open_card() turned both away
    }
    lv_obj_t* done = card_button(LV_SYMBOL_OK "  DONE", [](lv_event_t*) { card_close(); }, false);
    /* A kit of one's own opens on its name; anything else on its action. */
    lv_obj_t* focus = s_cardRow[0] && lv_obj_has_flag(s_cardRow[0], LV_OBJ_FLAG_CLICKABLE) ? s_cardRow[0]
                    : first ? first : done;
    lv_group_focus_obj(focus);
}

// ── Events ───────────────────────────────────────────────────────────────────

void type_cb(lv_event_t* e) {
    select_type((uint8_t)(intptr_t)lv_event_get_user_data(e));
    /* Back to turning the list -- after this click has been read. */
    lv_async_call(focus_list, nullptr);
}

void load_cb(lv_event_t*) { do_load(); }
void edit_cb(lv_event_t*) { open_card(); }

/// The encoder, turning: one row a detent.
void list_key_cb(lv_event_t* e) {
    const uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_RIGHT || k == LV_KEY_DOWN)    step(1);
    else if (k == LV_KEY_LEFT || k == LV_KEY_UP)  step(-1);
}

/// A finger on the list: a drag moves it a row per row's height, a tap picks
/// a row, a tap on the highlighted one loads it. The encoder's click loads.
void list_press_cb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* in = lv_indev_active();
    const bool pointer = in && lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER;
    if (code == LV_EVENT_PRESSED) {
        s_dragAcc = 0;
        s_dragged = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (!pointer) return;
        lv_point_t v;
        lv_indev_get_vect(in, &v);
        s_dragAcc += v.y;
        while (s_dragAcc >= kRowH)  { s_dragAcc -= kRowH; step(-1); s_dragged = true; }
        while (s_dragAcc <= -kRowH) { s_dragAcc += kRowH; step(1);  s_dragged = true; }
        return;
    }
    if (code != LV_EVENT_CLICKED) return;
    if (!pointer) { do_load(); return; }
    if (s_dragged) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(s_list, &a);
    const int j = (int)((p.y - a.y1) / kRowH);
    if (j < 0 || j >= kRows) return;
    const uint16_t idx = (uint16_t)(s_off[s_type] + j);
    if (idx >= s_items[s_type].size()) return;
    if (idx == s_sel[s_type]) { do_load(); return; }
    s_sel[s_type] = idx;
    moved();
}

/// The power button's short click leaves through the browser's own close path:
/// the hook that puts the app the user came from back on screen is raised by it
/// and by nothing else.
bool selector_back_cb(void*) {
    if (!s_overlay) return false;
    if (card_back()) return true;
    if (s_type == Pitch && !s_pitchGroup.empty()) { open_group({}); return true; }
    kit_selector_close();
    return true;
}

// ── The tick: answers from the card, the load, the preview ──────────────────

void listing_tick() {
    crosspad::ISampleTools* st = tools();
    crosspad::IInstrumentHost* h = host();
    if (s_listing == Listing::Done || picking()) { s_listing = Listing::Done; return; }
    if (!st) { s_listing = Listing::Done; return; }
    if (!s_listAsked) {
        bool asked = false;
        if (s_listing == Listing::Library) {
            if (!h) { s_listing = Listing::Takes; return; }
            asked = st->requestFolder(st->cardRoot() + h->libraryRoot());
        } else {
            asked = st->requestFolder("");
        }
        s_listAsked = asked;   // refused while another job runs: asked again next tick
        return;
    }
    const crosspad::ToolJob job = st->job();
    if (job == crosspad::ToolJob::Busy) return;
    const crosspad::CardFolder f = job == crosspad::ToolJob::Ready ? st->folder() : crosspad::CardFolder{};
    s_listAsked = false;
    if (s_listing == Listing::Library) {
        s_insts.clear();
        for (size_t i = 0; i < f.folders; ++i) s_insts.push_back(f.name(i));
        s_listing = Listing::Takes;
    } else {
        s_takes.clear();
        for (size_t i = f.folders; i < f.size(); ++i) {
            if (ends_wav(f.name(i))) s_takes.push_back(f.path + "/" + f.name(i));
        }
        s_listing = Listing::Done;
    }
    build_items();
    refresh();
}

void songs_tick() {
    crosspad::ISongEngine* se = songs();
    if (!se || picking()) return;
    if (s_songsAsked && se->listGeneration() != s_songsGen) {
        s_songsAsked = false;
        s_songs = se->projects();
        build_items();
        refresh();
    }
    if (s_songOpening.empty()) return;
    /* Ready with the old song is a request the worker has not reached yet. */
    const crosspad::SongOpState os = se->openState();
    const bool ok = os == crosspad::SongOpState::Ready && open_song_name() == s_songOpening;
    if (!ok && os != crosspad::SongOpState::Failed && lv_tick_elaps(s_songAskedMs) < kSongOpenMs) return;
    s_songOpening.clear();
    if (!ok) { s_follow = nullptr; say("That song would not open"); refresh(); return; }
    const char* app = s_follow;
    s_follow = nullptr;
    if (app) open_app(app);
}

void tick_cb(lv_timer_t*) {
    if (!s_overlay) return;
    listing_tick();
    songs_tick();

    if (s_kitLoading && !kit_load_task_is_loading()) {
        s_kitLoading = false;
        s_loadingKit = -1;
        /* The host owns teardown from here (KitSelectorApp destroys the App). */
        if (s_onLoaded) { s_onLoaded(s_userData); return; }
    }

    const bool fresh = peek_take();
    const Item* it = cur();
    if (fresh && kit_of(it)) {
        if (s_padLogicPushed && !s_card) paint_pads();
    }
    /* Heard once the highlight stands still -- and, for a kit, once its pads
     * are known. */
    if (s_previewDue && !s_card && lv_tick_elaps(s_movedMs) >= kPreviewMs) {
        if (!it || it->kind == Kind::Song || it->kind == Kind::Inst || it->kind == Kind::Group) s_previewDue = false;
        else if (it->kind == Kind::Wav || kit_of(it)) { s_previewDue = false; audition(it, -1); }
    }
    /* A load moving on repaints its key, not the screen. */
    Shown now = shown_now();
    const int progress = now.progress;
    now.progress = s_shown.progress;
    if (!(now == s_shown)) refresh();
    else if (progress != s_shown.progress) { s_shown.progress = progress; paint_load(); }
}

void reset_state();
void overlay_deleted_cb(lv_event_t* e) {
    if (lv_event_get_target_obj(e) == s_overlay) reset_state();
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

lv_obj_t* kit_selector_create(const KitSelectorConfig& config) {
    s_onPicked   = config.onPicked;
    s_onSelected = config.onSelected;
    s_onLoaded   = config.onLoaded;
    s_onClose    = config.onClose;
    s_onOpenApp  = config.onOpenApp;
    s_userData   = config.userData;
    s_kitLoading = false;
    s_loadingKit = -1;
    s_songOpening.clear();
    s_follow = nullptr;
    s_message.clear();
    s_previewKey.clear();
    s_listing = Listing::Library;
    s_listAsked = false;
    for (int t = 0; t < kTypes; ++t) { s_sel[t] = s_off[t] = 0; s_seen[t] = false; }

    lv_obj_t* root = lv_obj_create(config.parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, kBg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, overlay_deleted_cb, LV_EVENT_DELETE, nullptr);
    s_overlay = root;

    s_group = lv_group_create();
    lv_group_set_wrap(s_group, true);
    if (lv_indev_t* enc = encoder()) {
        s_prevGroup = lv_indev_get_group(enc);
        lv_indev_set_group(enc, s_group);
    }

    /* The context row. */
    lv_obj_t* ctx = box(root, 4, 4, 312, 22, kPanel);
    s_info = txt(ctx, &lv_font_montserrat_14, kText, "");
    lv_label_set_recolor(s_info, true);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_DOT);
    /* One line, cut with dots: without a height a dotted label wraps, and a
     * long author pushed LOADED down over the type keys. */
    lv_obj_set_size(s_info, 300, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_obj_align(s_info, LV_ALIGN_LEFT_MID, 8, 0);

    /* The list: one object the encoder turns, five rows drawn in it. */
    s_list = lv_CreateEditableButton(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_pos(s_list, kListX, kBodyY);
    lv_obj_set_size(s_list, kListW, kRows * kRowH);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(s_list, 4, 0);
    /* Left for the keys (a long press), it says it is still there. */
    lv_obj_set_style_outline_color(s_list, kDim, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(s_list, 1, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(s_list, 0, LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    lv_obj_add_event_cb(s_list, list_key_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_list, list_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_list, list_press_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_list, list_press_cb, LV_EVENT_CLICKED, nullptr);
    lv_group_add_obj(s_group, s_list);
    for (int j = 0; j < kRows; ++j) {
        lv_obj_t* r = box(s_list, 0, j * kRowH, kListW, kRowH - 3, kPanel);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        s_rowLd[j] = box(r, 2, 6, 3, kRowH - 15, kLime);
        lv_obj_set_style_radius(s_rowLd[j], 2, 0);
        s_rowName[j] = txt(r, &lv_font_montserrat_14, kText, "");
        lv_obj_set_width(s_rowName[j], 160);
        lv_label_set_long_mode(s_rowName[j], LV_LABEL_LONG_DOT);
        lv_obj_align(s_rowName[j], LV_ALIGN_LEFT_MID, 9, 0);
        s_rowMeta[j] = txt(r, &lv_font_montserrat_10, kDim, "");
        lv_obj_align(s_rowMeta[j], LV_ALIGN_RIGHT_MID, -6, 0);
        s_rowEq[j] = lv_obj_create(r);
        lv_obj_remove_style_all(s_rowEq[j]);
        lv_obj_remove_flag(s_rowEq[j], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(s_rowEq[j], 8, 10);
        lv_obj_align(s_rowEq[j], LV_ALIGN_RIGHT_MID, -50, 0);
        for (int k = 0; k < 3; ++k) {
            lv_obj_t* bar = box(s_rowEq[j], k * 3, 2 + (k % 2) * 4, 2, 8 - (k % 2) * 4, kMint);
            lv_obj_set_style_radius(bar, 0, 0);
        }
        s_row[j] = r;
    }
    s_sb = box(root, 313, kBodyY, 2, kRows * kRowH - 3, kPanel);
    lv_obj_set_style_radius(s_sb, 1, 0);
    s_sbThumb = box(s_sb, 0, 0, 2, 14, kLime);
    lv_obj_set_style_radius(s_sbThumb, 1, 0);

    /* LOAD and EDIT, then the lists: the encoder's walk after the list. */
    s_load = key(root, 4, kKeysY, kKeyW, kKeyH, load_cb, nullptr);
    lv_obj_set_style_clip_corner(s_load, true, 0);
    s_loadFill = box(s_load, 0, 0, 0, 30, kLime);
    s_loadLbl = txt(s_load, &lv_font_montserrat_14, kInk, "LOAD");
    lv_group_add_obj(s_group, s_load);
    s_edit = key(root, 162, kKeysY, kKeyW, kKeyH, edit_cb, nullptr);
    lv_obj_center(txt(s_edit, &lv_font_montserrat_14, kText, "EDIT"));
    lv_group_add_obj(s_group, s_edit);
    for (int i = 0; i < kTypes; ++i) {
        s_typ[i] = key(root, 4, kBodyY + i * kTypeGap, kTypeW, kTypeH, type_cb, (void*)(intptr_t)i);
        s_typLbl[i] = txt(s_typ[i], &lv_font_montserrat_14, lv_color_hex(kTypeCol[i]), kTypeName[i]);
        lv_obj_align(s_typLbl[i], LV_ALIGN_LEFT_MID, 9, 0);
        s_typBar[i] = box(s_typ[i], 2, (kTypeH - 14) / 2, 3, 14, kLime);
        lv_obj_set_style_radius(s_typBar[i], 2, 0);
        if (type_enabled((uint8_t)i)) lv_group_add_obj(s_group, s_typ[i]);
    }

    /* What there is. The card's lists are read once per visit; the kits are in
     * memory already. */
    s_peekPath.clear();
    s_peekKit.reset();
    build_items();
    if (crosspad::ISongEngine* se = songs(); se && !picking()) {
        s_songsGen = se->listGeneration();
        s_songsAsked = se->requestProjects();
        if (s_songsAsked) s_songs = se->projects();   // the last refresh, until this one is in
        build_items();
    }

    /* Open on the list the loaded kit is in, on the kit -- or, picking for a
     * track of one kind (Arrange's DRUMS, SLICE), on that list. */
    s_type = Kits;
    if (config.list != crosspad::KitList::Any) {
        s_type = config.list == crosspad::KitList::Drums ? Drums
               : config.list == crosspad::KitList::Slice ? Slice : Kits;
    } else if (auto* km = kits()) {
        const int id = km->getCurrentKitId();
        for (int t = 0; t < kTypes; ++t) {
            for (const Item& it : s_items[t]) {
                if (it.kitId == id && id >= 0 && it.kind != Kind::NewKit) s_type = (uint8_t)t;
            }
        }
    }
    land_on_loaded(s_type);

    if (!s_padLogicRegistered) {
        crosspad::getPadManager().registerPadLogic(kPadLogic, std::make_shared<BrowserPadLogic>());
        s_padLogicRegistered = true;
    }
    crosspad::getPadManager().pushPadLogic(kPadLogic);
    s_padLogicPushed = true;

    s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);
    s_backToken = crosspad_gui::ui_back_push(selector_back_cb, nullptr);

    lv_group_focus_obj(s_list);
    lv_group_set_editing(s_group, true);
    moved();
    s_previewDue = false;   // opening the browser plays nothing
    return root;
}

namespace {

/* Everything that must not outlive the widget tree. Shared with the
 * LV_EVENT_DELETE fallback, so a host that drops the tree without calling
 * kit_selector_destroy cannot leave the timer writing into freed memory. */
void reset_state() {
    preview_stop();
    if (s_padLogicPushed) {
        crosspad::getPadManager().popPadLogic();
        s_padLogicPushed = false;
    }

    /* A card or its keyboard still up: their groups are not children of the
     * screen, so they go here, and the encoder may be reading one of them --
     * it goes back to the list's group before either is freed. */
    if (lv_indev_t* enc = encoder()) {
        lv_group_t* g = lv_indev_get_group(enc);
        if (g == s_typeGroup || g == s_cardGroup) lv_indev_set_group(enc, s_group);
    }
    if (s_typeGroup) { lv_group_delete(s_typeGroup); s_typeGroup = nullptr; }
    if (s_cardGroup) { lv_group_delete(s_cardGroup); s_cardGroup = nullptr; }
    s_card = s_typing = s_typeArea = s_cardTitle = s_delRow = nullptr;
    for (int i = 0; i < kFields; ++i) s_cardRow[i] = s_cardVal[i] = nullptr;
    s_cardKit  = -1;
    s_delArmed = false;

    if (s_backToken) { crosspad_gui::ui_back_remove(s_backToken); s_backToken = 0; }
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }

    if (lv_indev_t* enc = encoder()) {
        if (lv_indev_get_group(enc) == s_group) lv_indev_restore_group(enc, s_prevGroup);
    }
    s_prevGroup = nullptr;
    if (s_group) { lv_group_delete(s_group); s_group = nullptr; }

    /* Swapped out, not cleared: a cleared vector keeps its block. */
    for (auto& v : s_items) std::vector<Item>().swap(v);
    std::vector<std::string>().swap(s_insts);
    std::string().swap(s_pitchGroup);   // the groups again next time, on the one played
    std::vector<std::string>().swap(s_takes);
    std::vector<std::string>().swap(s_songs);
    s_songsAsked = false;
    s_kitLoading = false;
    s_songOpening.clear();
    s_follow = nullptr;
    s_peekKit.reset();
    s_peekPath.clear();
    s_overlay = s_info = s_list = s_sb = s_sbThumb = nullptr;
    s_load = s_loadFill = s_loadLbl = s_edit = nullptr;
    for (int i = 0; i < kTypes; ++i) s_typ[i] = s_typLbl[i] = s_typBar[i] = nullptr;
    for (int j = 0; j < kRows; ++j) s_row[j] = s_rowName[j] = s_rowMeta[j] = s_rowLd[j] = s_rowEq[j] = nullptr;
}

} // namespace

void kit_selector_destroy(lv_obj_t* selector) {
    reset_state();
    if (selector) lv_obj_delete_async(selector);
}

void kit_selector_refresh(lv_obj_t* selector) {
    if (selector != s_overlay || !s_overlay) return;
    s_peekPath.clear();
    s_peekKit.reset();
    build_items();
    moved();
}

void kit_selector_set_callback(KitSelectedFn fn, void* userData) {
    s_onSelected = fn;
    s_userData = userData;
}

bool kit_selector_is_open() {
    return s_overlay != nullptr;
}

void kit_selector_close() {
    if (!s_overlay) return;
    if (s_onClose) {
        s_onClose(s_userData);
        return;
    }
    kit_selector_destroy(s_overlay);
}

} // namespace crosspad_sampler

#endif // USE_LVGL
