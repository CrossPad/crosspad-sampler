#include "crosspad-sampler/logic/pad_color_manager.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"

#include "crosspad/pad/PadManager.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"

namespace crosspad_sampler {

static_assert(PadColorManager::kPads == PAD_COUNT,
              "PadColorManager tracks a different pad count than the sampler");

// ── Private helper ────────────────────────────────────────────────────────────

namespace {
constexpr uint8_t kBreathMinPct  = 15;
constexpr uint8_t kBreathMaxPct  = 55;
constexpr uint8_t kBreathHalfLen = 2;    // ticks up, then the same down: 2.5 Hz at 100 ms
constexpr uint32_t kEmptyColor   = 0x000000;   // nothing to play: dark
constexpr uint32_t kFailedColor  = 0x400000;   // a sample that did not load: dim red
} // namespace


crosspad::RgbColor PadColorManager::uint32ToRgb(uint32_t color) {
    uint8_t r = static_cast<uint8_t>((color >> 16) & 0xFF);
    uint8_t g = static_cast<uint8_t>((color >>  8) & 0xFF);
    uint8_t b = static_cast<uint8_t>( color        & 0xFF);
    return crosspad::RgbColor(r, g, b);
}

void PadColorManager::restorePadKitColor(int padIdx) {
    if (padIdx < 0 || padIdx >= PAD_COUNT) return;

    crosspad::IKitManager* km = crosspad::getKitManager();
    if (!km) return;

    crosspad::KitInfo* kit = km->getCurrentKit();
    if (!kit) return;

    // Mid-swap the current kit is already the incoming one, which a pad that
    // has not been reloaded does not play yet: give it the pending color back.
    if (swapActive_ &&
        (swapPending_.load(std::memory_order_acquire) & (1u << padIdx))) {
        crosspad::getPadManager().setPadColor(static_cast<uint8_t>(padIdx),
                                              dim(staleColor_[padIdx], kBreathMinPct));
        return;
    }

    crosspad::getPadManager().setPadColor(
        static_cast<uint8_t>(padIdx),
        uint32ToRgb(shownColor(static_cast<uint8_t>(padIdx), kit->pads[padIdx].color))
    );
}

uint32_t PadColorManager::shownColor(uint8_t padIdx, uint32_t kitColor) const {
    if (padIdx >= PAD_COUNT) return kitColor;
    // Empty from the model, which the pad editor keeps current: a pad given a
    // sample there lights without waiting for a kit load to say so.
    crosspad::IKitManager* km = crosspad::getKitManager();
    const crosspad::KitInfo* kit = km ? km->getCurrentKit() : nullptr;
    if (kit && kit->parsed && kit->engine != crosspad::KitEngine::Pitched &&
        kit->pads[padIdx].sampleCount == 0) {
        return kEmptyColor;
    }
    // Failed only while the engine still cannot play it: a pad the editor has
    // since pointed at a file that loads is not shown as broken.
    if (failedMask_.load(std::memory_order_acquire) & (1u << padIdx)) {
        auto& cb = sampler_get_platform_callbacks();
        if (!cb.audio.padPlayable || !cb.audio.padPlayable(padIdx)) return kFailedColor;
    }
    return kitColor;
}

// ── Public API ────────────────────────────────────────────────────────────────

void PadColorManager::loadColorsFromKit(const crosspad::KitInfo& kit) {
    crosspad::PadManager& pm = crosspad::getPadManager();
    for (int i = 0; i < PAD_COUNT; ++i) {
        kitColor_[i] = kit.pads[i].color;
        pm.setPadColor(static_cast<uint8_t>(i), uint32ToRgb(kitColor_[i]));
    }
}

void PadColorManager::applyPadColor(uint8_t padIdx, const crosspad::Pad& pad) {
    if (padIdx >= PAD_COUNT) return;
    kitColor_[padIdx] = pad.color;
    crosspad::getPadManager().setPadColor(padIdx, uint32ToRgb(pad.color));
}

void PadColorManager::setSelectedPad(int padIdx) {
    if (padIdx == selectedPad_) return;

    // Restore previous selection's kit color before highlighting the new one.
    if (selectedPad_ >= 0) {
        restorePadKitColor(selectedPad_);
    }

    selectedPad_ = padIdx;

    if (padIdx >= 0 && padIdx < PAD_COUNT) {
        crosspad::getPadManager().setPadColor(
            static_cast<uint8_t>(padIdx),
            crosspad::RgbColor(255, 255, 255)
        );
    }
}

int PadColorManager::getSelectedPad() const {
    return selectedPad_;
}

// ── Live kit swap ─────────────────────────────────────────────────────────────
// A pad is only honest about which kit it plays once its sample has actually
// been reloaded, which under a performance can be seconds after the parse. So
// a pad that is still pending keeps the outgoing color, dimmed and breathing;
// it snaps to full brightness in the incoming color at the hand-over.

crosspad::RgbColor PadColorManager::dim(uint32_t color, uint8_t percent) {
    const uint32_t r = ((color >> 16) & 0xFF) * percent / 100;
    const uint32_t g = ((color >>  8) & 0xFF) * percent / 100;
    const uint32_t b = ( color        & 0xFF) * percent / 100;
    return crosspad::RgbColor(static_cast<uint8_t>(r),
                              static_cast<uint8_t>(g),
                              static_cast<uint8_t>(b));
}

void PadColorManager::paintSwapPad(uint8_t padIdx, uint16_t pending, uint8_t breathPct) {
    if (static_cast<int>(padIdx) == selectedPad_) return;   // the white highlight wins

    const uint16_t bit = static_cast<uint16_t>(1u << padIdx);
    crosspad::PadManager& pm = crosspad::getPadManager();

    if (pending & bit) {
        swapSettled_ &= static_cast<uint16_t>(~bit);
        pm.setPadColor(padIdx, dim(staleColor_[padIdx], breathPct));
        return;
    }
    if (swapSettled_ & bit) return;      // already handed over; nothing to write
    swapSettled_ |= bit;
    pm.setPadColor(padIdx, uint32ToRgb(shownColor(padIdx, kitColor_[padIdx])));
}

void PadColorManager::beginKitSwap(const crosspad::KitInfo& incoming, uint16_t pendingMask) {
    for (int i = 0; i < PAD_COUNT; ++i) {
        // Before the first load there is nothing to fade from: the incoming
        // color dimmed still reads as "not this pad yet".
        staleColor_[i] = kitColor_[i] ? kitColor_[i] : incoming.pads[i].color;
        kitColor_[i]   = incoming.pads[i].color;
    }
    swapSettled_ = 0;
    breathStep_  = 0;
    swapActive_  = true;
    swapPending_.store(pendingMask, std::memory_order_release);
}

void PadColorManager::markPadReloaded(uint8_t padIdx, PadLoad state) {
    if (padIdx >= PAD_COUNT) return;
    const uint16_t bit = static_cast<uint16_t>(1u << padIdx);
    // The state before the pad leaves the pending set: the LVGL thread paints
    // a settled pad once, from what it says.
    if (state == PadLoad::Failed) failedMask_.fetch_or(bit, std::memory_order_release);
    else                          failedMask_.fetch_and(static_cast<uint16_t>(~bit), std::memory_order_release);
    swapPending_.fetch_and(static_cast<uint16_t>(~bit), std::memory_order_release);
}

void PadColorManager::tickKitSwap() {
    if (!swapActive_) return;
    // Triangle, so the pending pads breathe rather than blink.
    breathStep_ = static_cast<uint8_t>((breathStep_ + 1) % (kBreathHalfLen * 2));
    paintKitSwap();
}

void PadColorManager::repaintKitSwap() {
    if (!swapActive_) return;
    swapSettled_ = 0;
    paintKitSwap();
}

void PadColorManager::paintKitSwap() {
    const uint8_t up = breathStep_ < kBreathHalfLen
                           ? breathStep_
                           : static_cast<uint8_t>(kBreathHalfLen * 2 - breathStep_);
    const uint8_t pct = static_cast<uint8_t>(
        kBreathMinPct + (kBreathMaxPct - kBreathMinPct) * up / kBreathHalfLen);

    const uint16_t pending = swapPending_.load(std::memory_order_acquire);
    for (uint8_t i = 0; i < PAD_COUNT; ++i) {
        paintSwapPad(i, pending, pct);
    }
}

void PadColorManager::endKitSwap() {
    if (!swapActive_) return;
    swapActive_ = false;
    swapPending_.store(0, std::memory_order_release);
    swapSettled_ = 0;

    crosspad::PadManager& pm = crosspad::getPadManager();
    for (uint8_t i = 0; i < PAD_COUNT; ++i) {
        if (static_cast<int>(i) == selectedPad_) continue;
        pm.setPadColor(i, uint32ToRgb(shownColor(i, kitColor_[i])));
    }
}

void PadColorManager::resetAllColors() {
    crosspad::PadManager& pm = crosspad::getPadManager();
    const crosspad::RgbColor gray(50, 50, 50);
    for (int i = 0; i < PAD_COUNT; ++i) {
        pm.setPadColor(static_cast<uint8_t>(i), gray);
    }
}

} // namespace crosspad_sampler
