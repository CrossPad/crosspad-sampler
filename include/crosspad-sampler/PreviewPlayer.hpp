#pragma once
#include <cstdint>
#include <vector>

namespace crosspad_sampler {

/// Plays a kit's `preview.mid` back through the pads, looping, so a kit can
/// audition itself without a host.
///
/// Deliberately portable: parsing and scheduling are plain C++ and file access
/// is stdio, so the PC simulator runs the same code. It is driven by poll()
/// from whatever timer the UI already has rather than owning a task -- a
/// preview beat does not justify a thread, and an LVGL timer's few ms of jitter
/// is inaudible against a 125 ms sixteenth at 120 BPM.
///
/// Notes map as `pad = note - 36`, the General MIDI drum base the files are
/// written against; anything outside 0..15 after that is ignored rather than
/// wrapped, so a melodic file cannot fire random pads.
class PreviewPlayer {
public:
    /// Parses the file into a schedule. Safe to call again; replaces the old one.
    /// `fallbackBpm` is used only when the file carries no tempo event, which is
    /// common -- many DAWs keep tempo in the project and export none. A file
    /// that does carry one always wins, because it is the more specific answer.
    /// 0 means "no idea either", and then the SMF default of 120 applies.
    bool load(const char* midiPath, uint16_t fallbackBpm = 0);
    bool loaded() const { return !events_.empty(); }
    /// Length of one pass, which is what the loop repeats.
    uint32_t lengthMs() const { return lengthMs_; }

    void start(uint32_t nowMs);
    void stop() { running_ = false; }
    bool running() const { return running_; }

    /// Fires every note due since the last call and wraps at the loop point.
    void poll(uint32_t nowMs);

private:
    struct Note { uint32_t atMs; uint8_t pad; uint8_t velocity; };
    std::vector<Note> events_;
    uint32_t lengthMs_ = 0;
    uint32_t startedAt_ = 0;
    size_t   next_ = 0;
    bool     running_ = false;
};

PreviewPlayer& previewPlayer();

} // namespace crosspad_sampler
