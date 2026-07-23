// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "common/microprofile.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_thread_pool.h"
#include "core/emulation_orchestrator.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace Core {

EmulationOrchestrator::EmulationOrchestrator(System& system)
    : system(system),
      cpu_pool(std::make_unique<CpuThreadPool>(system)) {}

EmulationOrchestrator::~EmulationOrchestrator() {
    Stop();
}

void EmulationOrchestrator::Stop() {
    if (cpu_pool) {
        cpu_pool->Stop();
    }
}

System::ResultStatus EmulationOrchestrator::ProcessSignals() {
    System::Signal signal{System::Signal::None};
    u32 param{};
    {
        std::scoped_lock lock{system.signal_mutex};
        if (system.current_signal != System::Signal::None) {
            signal = system.current_signal;
            param = system.signal_param;
            system.current_signal = System::Signal::None;
        }
    }

    switch (signal) {
    case System::Signal::Reset:
        if (system.GetAppLoader().DoingInitialSetup()) {
            return System::ResultStatus::ShutdownRequested;
        }
        system.Reset();
        return System::ResultStatus::Success;

    case System::Signal::Shutdown:
        return System::ResultStatus::ShutdownRequested;

    case System::Signal::Load: {
        system.save_state_slot = param;
        system.save_state_request_time = std::chrono::steady_clock::now();
        system.save_state_request_status = System::SaveStateStatus::LOADING;
        break;
    }
    case System::Signal::Save: {
        system.save_state_slot = param;
        system.save_state_request_time = std::chrono::steady_clock::now();
        system.save_state_request_status = System::SaveStateStatus::SAVING;
        break;
    }
    default:
        break;
    }

    if (system.save_state_request_status == System::SaveStateStatus::LOADING &&
        system.KernelRunning() && !system.Kernel().AreAsyncOperationsPending()) {
        const u32 slot = system.save_state_slot;
        system.save_state_request_status = System::SaveStateStatus::NONE;
        LOG_INFO(Core, "Begin load of slot {}", slot);
        try {
            system.LoadState(slot);
            LOG_INFO(Core, "Load completed");
        } catch (const std::exception& e) {
            LOG_ERROR(Core, "Error loading: {}", e.what());
            system.status_details = e.what();
            return System::ResultStatus::ErrorSavestate;
        }
        system.frame_limiter.WaitOnce();
        return System::ResultStatus::Success;
    } else if (system.save_state_request_status == System::SaveStateStatus::SAVING &&
               system.KernelRunning() && !system.Kernel().AreAsyncOperationsPending()) {
        system.save_state_request_status = System::SaveStateStatus::NONE;
        const u32 slot = system.save_state_slot;
        LOG_INFO(Core, "Begin save to slot {}", slot);
        try {
            system.SaveState(slot);
            LOG_INFO(Core, "Save completed");
        } catch (const std::exception& e) {
            LOG_ERROR(Core, "Error saving: {}", e.what());
            system.status_details = e.what();
            return System::ResultStatus::ErrorSavestate;
        }
        system.frame_limiter.WaitOnce();
        return System::ResultStatus::Success;
    } else if (system.save_state_request_status != System::SaveStateStatus::NONE &&
               (std::chrono::steady_clock::now() - system.save_state_request_time) >
                   std::chrono::seconds(5)) {
        system.save_state_request_status = System::SaveStateStatus::NONE;
        LOG_ERROR(Core, "Cannot perform save state operation due to pending async operations");
        system.status_details = "Cannot perform save state operation due to pending async operations";
        return System::ResultStatus::ErrorSavestate;
    }

    return System::ResultStatus::Success;
}

System::ResultStatus EmulationOrchestrator::RunFrame() {
    system.status = System::ResultStatus::Success;

    if (!system.IsPoweredOn()) {
        return System::ResultStatus::ErrorNotInitialized;
    }

    System::ResultStatus signal_result = ProcessSignals();
    if (signal_result != System::ResultStatus::Success) {
        return signal_result;
    }

    if (!cpu_pool->IsRunning()) {
        cpu_pool->Start();
    }

    // Run slices until VBlank fires, then present the frame.
    // SwapBuffers MUST happen after WaitAll — never in parallel with workers,
    // because SwapBuffers can evict GPU surfaces that workers' BlitTextures still references.
    while (!system.GPU().IsFrameReady()) {
        cpu_pool->KickAll();
        cpu_pool->WaitAll();
    }

    system.GPU().ClearFrameReady();
    system.GPU().Renderer().SwapBuffers();

    return system.status;
}

} // namespace Core
