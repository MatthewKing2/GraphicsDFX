#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace profiling {

// The only switch: true enables CPU timers, GPU queries, and periodic summaries.
// Leave false for normal use; rebuild after changing it.
// Benchmark with -O3 -DNDEBUG to exclude validation overhead; normal build
// flags are unchanged. GPU render measures a pipeline span (including stalls),
// not a utilization counter; CPU matrix memcpy does not time GPU memory fetches.
inline constexpr bool ENABLED = false;
inline constexpr std::uint32_t RANDOM_SEED = 12345;
inline constexpr std::uint64_t WARMUP_FRAMES = 60;
inline constexpr std::size_t REPORT_FRAMES = 240;

using Clock = std::chrono::steady_clock;
enum class Stage : std::size_t {
    EventsCamera, GravityVelocity, PositionModels, CameraMatrices,
    FenceWait, Acquire, MatrixCopy, RecordCommands, Submit, Present, QueryRead,
    Count
};
inline constexpr std::size_t STAGE_COUNT = static_cast<std::size_t>(Stage::Count);
inline constexpr std::array<const char*, STAGE_COUNT> STAGE_NAMES = {
    "events + camera", "gravity + velocity", "positions + models", "view + projection",
    "frame fence wait", "image acquisition", "matrix memcpy", "command recording",
    "queue submission", "presentation call", "GPU query readback"
};

inline double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

class Profiler {
public:
    void beginFrame() {
        ++frameId;
        frameTimes.fill(0.0);
        frameStart = Clock::now();
    }

    void add(Stage stage, double ms) {
        frameTimes[static_cast<std::size_t>(stage)] += ms;
    }

    std::uint64_t currentFrameId() const { return frameId; }

    void expectGpu(std::uint64_t id) {
        if (auto* window = windowFor(id)) ++window->gpuExpected;
    }

    // GPU results belong to the frame that submitted them, not today's CPU frame.
    void gpuResult(std::uint64_t id, bool valid, double ms) {
        if (auto* window = windowFor(id)) {
            ++window->gpuResolved;
            if (valid) {
                window->gpuTotalMs += ms;
                ++window->gpuSamples;
            }
        }
    }

    void endFrame(std::size_t instances, std::size_t copiedBytes) {
        const double totalMs = milliseconds(Clock::now() - frameStart);
        if (auto* window = windowFor(frameId)) {
            window->frameMs[window->cpuSamples++] = totalMs;
            window->totalMs += totalMs;
            window->instances = instances;
            window->copiedBytes = copiedBytes;
            for (std::size_t i = 0; i < STAGE_COUNT; ++i) {
                window->stageMs[i] += frameTimes[i];
            }
        }
        reportReady(false);
    }

    void finish() { reportReady(true); }

private:
    struct Window {
        std::uint64_t id = std::numeric_limits<std::uint64_t>::max();
        std::size_t cpuSamples = 0;
        std::size_t gpuExpected = 0;
        std::size_t gpuResolved = 0;
        std::size_t gpuSamples = 0;
        std::size_t instances = 0;
        std::size_t copiedBytes = 0;
        double totalMs = 0.0;
        double gpuTotalMs = 0.0;
        std::array<double, STAGE_COUNT> stageMs{};
        std::array<double, REPORT_FRAMES> frameMs{};
        bool reported = false;
    };
    // Frame fences bound GPU lag to the in-flight slots. Three windows leave
    // room for a completed CPU window awaiting its final GPU timestamps.
    std::array<Window, 3> windows{};
    std::array<double, STAGE_COUNT> frameTimes{};
    std::uint64_t frameId = 0;
    Clock::time_point frameStart{};

    Window* windowFor(std::uint64_t id) {
        if (id <= WARMUP_FRAMES) return nullptr;
        const auto windowId = (id - WARMUP_FRAMES - 1) / REPORT_FRAMES;
        auto& window = windows[windowId % windows.size()];
        if (window.id != windowId) {
            window = Window{};
            window.id = windowId;
        }
        return &window;
    }

    void reportReady(bool includePartial) {
        for (auto& window : windows) {
            if (window.reported || window.cpuSamples == 0 ||
                window.gpuResolved != window.gpuExpected ||
                (!includePartial && window.cpuSamples < REPORT_FRAMES)) continue;

            const double count = static_cast<double>(window.cpuSamples);
            auto sorted = window.frameMs;
            std::sort(sorted.begin(), sorted.begin() + window.cpuSamples);
            const auto p95 = (window.cpuSamples * 95 + 99) / 100 - 1;
            double accountedMs = 0.0;
            std::ostringstream output;
            output << std::fixed << std::setprecision(3)
                   << "\n[profile] window=" << window.id
                   << " frames=" << window.cpuSamples
                   << " instances=" << window.instances
                   << " copied_bytes/frame=" << window.copiedBytes << '\n'
                   << "  frame avg=" << window.totalMs / count
                   << " ms, p95=" << sorted[p95]
                   << " ms, loop FPS=" << 1000.0 * count / window.totalMs << '\n';
            for (std::size_t i = 0; i < STAGE_COUNT; ++i) {
                accountedMs += window.stageMs[i];
                output << "  " << STAGE_NAMES[i] << ": "
                       << window.stageMs[i] / count << " ms ("
                       << 100.0 * window.stageMs[i] / window.totalMs << "%)\n";
            }
            output << "  other CPU overhead: "
                   << std::max(0.0, window.totalMs - accountedMs) / count << " ms\n";
            if (window.gpuSamples != 0) {
                output << "  GPU render: " << window.gpuTotalMs / window.gpuSamples
                       << " ms, samples=" << window.gpuSamples << '/' << window.gpuExpected
                       << " (overlaps CPU; do not add to frame total)\n";
            } else {
                output << "  GPU render: unavailable\n";
            }
            std::cout << output.str() << std::flush;
            window.reported = true;
        }
    }
};

inline Profiler& profiler() {
    static Profiler instance;
    return instance;
}

// With ENABLED=false, these branches are compiled out, including Clock::now().
class ScopedTimer {
public:
    explicit ScopedTimer(Stage measuredStage) : stage(measuredStage) {
        if constexpr (ENABLED) start = Clock::now();
    }
    ~ScopedTimer() {
        if constexpr (ENABLED) profiler().add(stage, milliseconds(Clock::now() - start));
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
private:
    Stage stage;
    Clock::time_point start{};
};

} // namespace profiling
