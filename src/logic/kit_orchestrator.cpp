// SPDX-License-Identifier: MIT

#include <crosspad-sampler/logic/kit_orchestrator.hpp>
#include <crosspad-sampler/logic/pad_color_manager.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/event/EventTypes.hpp>
#include <crosspad/platform/PlatformServices.hpp>

namespace crosspad_sampler {

void KitOrchestrator::init(PadColorManager& colorManager)
{
    colorManager_ = &colorManager;
}

crosspad::KitLoadResult KitOrchestrator::changeKit(int kitId)
{
    crosspad::KitLoadResult result;

    if (loading_) {
        result.error = "Already loading";
        return result;
    }

    crosspad::IKitManager* kitManager = crosspad::getKitManager();
    if (!kitManager) {
        result.error = "Kit manager unavailable";
        return result;
    }

    crosspad::KitInfo* kit = kitManager->getKit(kitId);
    if (!kit) {
        result.error = "Kit not found";
        return result;
    }

    loading_ = true;

    // Nothing is stopped here. What is sounding keeps sounding: the load task
    // reloads each pad once its voice has ended (kit_load_task.cpp), and an
    // engine that has to go wholesale — the pitched bank — stops its own
    // voices on the platform side.
    crosspad::IEventBus& bus = crosspad::getEventBus();

    result = kitManager->loadKit(*kit);
    if (result.success) {
        kitManager->setCurrentKitId(kitId);
    }

    // Colors are NOT applied here. A live swap reloads one pad at a time, so
    // the loader stages them through PadColorManager::beginKitSwap() and hands
    // each pad over as its sample lands (kit_load_task.cpp).

    if (progressFn_) {
        progressFn_(result.padsLoaded, PAD_COUNT, progressData_);
    }

    loading_ = false;

    bus.postAppChanged("Sampler", crosspad::EventSource::App);

    return result;
}

void KitOrchestrator::setProgressCallback(KitLoadProgressFn fn, void* userData)
{
    progressFn_   = fn;
    progressData_ = userData;
}

bool KitOrchestrator::isLoading() const
{
    return loading_;
}

} // namespace crosspad_sampler
