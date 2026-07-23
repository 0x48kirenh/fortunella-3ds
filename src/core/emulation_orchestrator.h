// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "core/core.h"

namespace Core {

class CpuThreadPool;

class EmulationOrchestrator {
public:
    explicit EmulationOrchestrator(System& system);
    ~EmulationOrchestrator();

    EmulationOrchestrator(const EmulationOrchestrator&) = delete;
    EmulationOrchestrator& operator=(const EmulationOrchestrator&) = delete;

    [[nodiscard]] System::ResultStatus RunFrame();

    void Stop();

private:
    [[nodiscard]] System::ResultStatus ProcessSignals();

    System& system;
    std::unique_ptr<CpuThreadPool> cpu_pool;
};

} // namespace Core
