// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <crosspad/pad/IPadLogicHandler.hpp>
#include <crosspad/pad/PadPerformance.hpp>
#include <crosspad/event/IEventBus.hpp>

namespace crosspad_sampler {

/**
 * @brief Pad logic handler for the sampler app.
 *
 * Implements crosspad::IPadLogicHandler and manages pad interactions
 * for the CrossPad sampler: triggering sample playback on press,
 * applying kit pad colors on activate, and cleaning up on deactivate.
 *
 * Playback modes (from kit pad config):
 *   0 = one-shot  — sample plays to end regardless of release
 *   1 = hold      — sample stops on pad release
 *   2 = loop      — reserved (treated as one-shot for now)
 *
 * Performance modes, MPC-style, none persisted:
 *   Full level  — every hit plays at velocity 127, whatever the pad measured.
 *   16 levels   — the pad hit last before the mode went on becomes the
 *                 source; every pad then plays that source at one of sixteen
 *                 levels:
 *     Velocity  — pad 0 the softest, pad 15 full. The LEDs show the source's
 *                 colour as a brightness ramp.
 *     Pitch     — pad 0 as it is, each pad a semitone above the one before
 *                 (pad 15 +15), at the velocity the pad measured -- the
 *                 layout Pitch Play starts with. Each pad is a note of its
 *                 own, so a chord holds. The LEDs show it as a keyboard: the
 *                 source's notes bright, the sharps dim.
 *
 * A take keeps what each hit sounded as (performance()), and its playback is
 * played that way -- a part recorded in 16 levels plays its source pad at
 * its levels with the mode off, and a plain part plays its own pads with the
 * mode on. The modes shape what a finger plays, not what was recorded.
 */
class SamplerPadLogic : public crosspad::IPadLogicHandler, public crosspad::IPadPerformance {
public:
    SamplerPadLogic() = default;
    ~SamplerPadLogic() override = default;

    void setFullLevel(bool on) { fullLevel_ = on; }
    bool fullLevel() const { return fullLevel_; }

    using Levels = crosspad::PadLevels;
    /// Turning it on takes the last pad hit as the source (pad 0 when none
    /// was hit yet) and paints the pads for it; switching between velocity
    /// and pitch keeps the source; off restores the kit's colours.
    void setLevels(Levels mode) override;
    Levels levels() const override { return levels_; }
    uint8_t levelsSource() const override { return levelsSource_; }
    bool sixteenLevels() const { return levels_ != Levels::Off; }
    uint8_t sixteenLevelsSource() const { return levelsSource_; }

    /// Repaints the pads for the active mode — the ramp, or the kit's own
    /// colours. For the kit loader, whose swap repaint ends on the kit's
    /// colours regardless of the mode.
    void refreshColors();

    /// Subscribe to EventBus events and load kit pad colors.
    void onActivate(crosspad::PadManager& padManager) override;

    /// Unsubscribe from EventBus events and reset pad colors to neutral gray.
    void onDeactivate(crosspad::PadManager& padManager) override;

    /// Trigger sample playback and provide visual press feedback.
    void onPadPress(crosspad::PadManager& padManager, uint8_t padIdx, uint8_t velocity) override;

    /// Provide visual release feedback; stop sample for Hold mode pads.
    void onPadRelease(crosspad::PadManager& padManager, uint8_t padIdx) override;

    /// Reserved for future aftertouch use — no-op.
    void onPadPressure(crosspad::PadManager& padManager, uint8_t padIdx, uint8_t pressure) override;

    /// A finger on the pad before its velocity: the engine arms the note the
    /// hit will play (player.armPad); a touch that left drops it.
    void onPadTouch(crosspad::PadManager& padManager, uint8_t padIdx, bool touching) override;

    /// What a finger on @p padIdx sounds as now: the levels' source at a
    /// level, and the velocity Full level or the ramp makes of it.
    crosspad::PadPerformance performance(uint8_t padIdx, uint8_t& velocity) const override;

private:
    /// How the press or release of @p padIdx sounds: what the recorder kept
    /// with it for its own presses, the mode the pads are in for a finger's.
    crosspad::PadPerformance pressPerformance(crosspad::PadManager& padManager, uint8_t padIdx,
                                              uint8_t& velocity) const;

    /// What a hit of a pad plays: the pad itself, or the levels' source at a
    /// pitch, as its own note.
    struct Note {
        uint8_t soundPad;
        int8_t  transpose;
        uint8_t key;
    };
    Note noteFor(const crosspad::PadPerformance& perf, uint8_t padIdx) const;

    /// Load per-pad colors from the current kit into PadLedController.
    void loadKitColors(crosspad::PadManager& padManager);

    /// The source pad's colour on every pad, brightness rising with the level.
    void loadLevelsColors();
    /// The MIDI note a pad sends, moved by @p transpose and kept in range.
    static uint8_t midiNote(crosspad::PadManager& padManager, uint8_t pad, int8_t transpose);

    /// Velocity a pad plays at under 16 levels: 7, 15, … 127.
    static uint8_t levelVelocity(uint8_t padIdx) { return static_cast<uint8_t>((padIdx + 1) * 8 - 1); }
    /// Semitones a pad plays the source at under 16 levels of pitch.
    static int8_t levelTranspose(uint8_t padIdx) { return static_cast<int8_t>(padIdx); }
    /// The key a pad's note is struck under in pitch mode: one per pad, past
    /// the pads' own, so each is a note of its own (SamplePlaybackData::key).
    static constexpr uint8_t kLevelsKey = 16;

    bool    fullLevel_     = false;
    Levels  levels_        = Levels::Off;
    uint8_t levelsSource_  = 0;
    uint8_t lastPad_       = 0;

    // EventBus subscription handles
    crosspad::SubscriptionHandle samplePlaybackHandle_ = nullptr;
    crosspad::SubscriptionHandle noteOnHandle_         = nullptr;
    crosspad::SubscriptionHandle noteOffHandle_        = nullptr;

    bool subscribed_ = false;

    // Static event callbacks
    static void onSamplePlayback(const void* event_data, crosspad::EventType type, void* user_ctx);
    static void onNoteOn(const void* event_data, crosspad::EventType type, void* user_ctx);
    static void onNoteOff(const void* event_data, crosspad::EventType type, void* user_ctx);
};

} // namespace crosspad_sampler
