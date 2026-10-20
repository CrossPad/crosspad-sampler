// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad-sampler/PreviewPlayer.hpp"

#include "crosspad/pad/PadManager.hpp"

#include <cstdio>
#include <cstring>

namespace crosspad_sampler {

namespace {

constexpr uint8_t  kDrumBaseNote = 36;   // GM drum base; note 36 is pad 0
constexpr uint32_t kDefaultTempoUs = 500000;  // 120 BPM, the SMF default

struct Reader {
    const uint8_t* p; size_t n; size_t i = 0;
    bool has(size_t k) const { return i + k <= n; }
    uint8_t  u8()  { return p[i++]; }
    uint16_t u16() { uint16_t v = (uint16_t)((p[i] << 8) | p[i + 1]); i += 2; return v; }
    uint32_t u32() { uint32_t v = ((uint32_t)p[i] << 24) | ((uint32_t)p[i+1] << 16)
                                | ((uint32_t)p[i+2] << 8) | p[i+3]; i += 4; return v; }
    /// Variable-length quantity: seven bits per byte, high bit continues.
    bool vlq(uint32_t& out) {
        out = 0;
        for (int k = 0; k < 4; ++k) {
            if (!has(1)) return false;
            uint8_t c = u8();
            out = (out << 7) | (uint8_t)(c & 0x7F);
            if (!(c & 0x80)) return true;
        }
        return false;
    }
};

} // namespace

bool PreviewPlayer::load(const char* midiPath, uint16_t fallbackBpm)
{
    events_.clear();
    lengthMs_ = 0;
    next_ = 0;
    running_ = false;
    if (!midiPath) return false;

    FILE* f = fopen(midiPath, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* A preview beat is a few hundred bytes; the cap keeps a stray file from
     * eating the heap the kit needs. */
    if (sz <= 14 || sz > 64 * 1024) { fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)sz);
    size_t got = fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) return false;

    Reader r{buf.data(), buf.size()};
    if (memcmp(r.p, "MThd", 4) != 0) return false;
    r.i = 4;
    uint32_t hdrLen = r.u32();
    if (hdrLen < 6 || !r.has(hdrLen)) return false;
    size_t afterHdr = r.i + hdrLen;
    (void)r.u16();                       // format: 0 and 1 both parse the same here
    uint16_t ntrk = r.u16();
    uint16_t division = r.u16();
    /* SMPTE timing divides differently and no preview uses it. */
    if (division == 0 || (division & 0x8000)) return false;
    r.i = afterHdr;

    /* The kit's tempo stands in until the file says otherwise; if neither
     * knows, the SMF default is all that is left. */
    uint32_t tempoUs = fallbackBpm ? (uint32_t)(60000000UL / fallbackBpm) : kDefaultTempoUs;
    bool fileSaidTempo = false;
    uint32_t lastTick = 0;

    for (uint16_t t = 0; t < ntrk; ++t) {
        if (!r.has(8) || memcmp(r.p + r.i, "MTrk", 4) != 0) break;
        r.i += 4;
        uint32_t trkLen = r.u32();
        if (!r.has(trkLen)) break;
        size_t end = r.i + trkLen;
        uint32_t tick = 0;
        uint8_t  status = 0;

        while (r.i < end) {
            uint32_t dt = 0;
            if (!r.vlq(dt)) break;
            tick += dt;
            if (!r.has(1)) break;
            uint8_t b = r.p[r.i];

            if (b == 0xFF) {                       // meta
                r.i++;
                if (!r.has(1)) break;
                uint8_t type = r.u8();
                uint32_t len = 0;
                if (!r.vlq(len) || !r.has(len)) break;
                if (type == 0x51 && len == 3) {
                    tempoUs = ((uint32_t)r.p[r.i] << 16) | ((uint32_t)r.p[r.i+1] << 8) | r.p[r.i+2];
                    fileSaidTempo = true;
                }
                r.i += len;
                continue;
            }
            if (b == 0xF0 || b == 0xF7) {          // sysex, skipped whole
                r.i++;
                uint32_t len = 0;
                if (!r.vlq(len) || !r.has(len)) break;
                r.i += len;
                continue;
            }
            if (b & 0x80) { status = b; r.i++; }   // else running status
            if (!status) break;

            uint8_t cmd = (uint8_t)(status & 0xF0);
            uint8_t need = (cmd == 0xC0 || cmd == 0xD0) ? 1 : 2;
            if (!r.has(need)) break;
            uint8_t d1 = r.u8();
            uint8_t d2 = (need == 2) ? r.u8() : 0;

            /* Note-on with velocity 0 is a note-off by convention; the preview
             * only needs the hits, since a pad plays its sample to the end. */
            if (cmd == 0x90 && d2 > 0 && d1 >= kDrumBaseNote) {
                uint8_t pad = (uint8_t)(d1 - kDrumBaseNote);
                if (pad < 16) {
                    /* Ticks -> ms with the tempo in force at parse time. A file
                     * that changes tempo mid-beat would drift; none does, and
                     * carrying a tempo map is not worth it for a preview. */
                    uint64_t ms = ((uint64_t)tick * tempoUs) / ((uint64_t)division * 1000ULL);
                    events_.push_back(Note{(uint32_t)ms, pad, d2});
                }
            }
            if (tick > lastTick) lastTick = tick;
        }
        r.i = end;
    }

    if (events_.empty()) return false;

    /* Sort by time: format 1 interleaves tracks, and even format 0 is only
     * ordered per track. Insertion sort -- a preview is tens of events. */
    for (size_t a = 1; a < events_.size(); ++a) {
        Note key = events_[a];
        size_t b = a;
        while (b > 0 && events_[b - 1].atMs > key.atMs) { events_[b] = events_[b - 1]; --b; }
        events_[b] = key;
    }

    (void)fileSaidTempo;   /* kept for clarity: the file has already overwritten
                            * tempoUs by here, so nothing more to decide. */
    uint64_t endMs = ((uint64_t)lastTick * tempoUs) / ((uint64_t)division * 1000ULL);
    /* Round the loop up to a whole bar of 4/4 so the repeat lands on the beat
     * even when the file stops on the last hit rather than at the bar line. */
    uint32_t barMs = (uint32_t)((4ULL * tempoUs) / 1000ULL);
    if (barMs == 0) barMs = 1;
    lengthMs_ = (uint32_t)(((endMs + barMs - 1) / barMs) * barMs);
    if (lengthMs_ == 0) lengthMs_ = barMs;
    return true;
}

void PreviewPlayer::start(uint32_t nowMs)
{
    if (events_.empty()) return;
    startedAt_ = nowMs;
    next_ = 0;
    running_ = true;
}

void PreviewPlayer::poll(uint32_t nowMs)
{
    if (!running_ || events_.empty() || lengthMs_ == 0) return;

    uint32_t into = nowMs - startedAt_;
    if (into >= lengthMs_) {                 // wrap, keeping the phase
        startedAt_ += lengthMs_;
        into -= lengthMs_;
        next_ = 0;
    }
    while (next_ < events_.size() && events_[next_].atMs <= into) {
        const Note& n = events_[next_];
        /* App: the kit playing itself, not a player -- a take running meanwhile
         * does not record it. */
        crosspad::getPadManager().handlePadPress(n.pad, n.velocity, crosspad::EventSource::App);
        ++next_;
    }
}

PreviewPlayer& previewPlayer()
{
    static PreviewPlayer p;
    return p;
}

} // namespace crosspad_sampler
