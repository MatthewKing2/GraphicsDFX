#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace prediction {
using uint = std::uint32_t;
constexpr uint STAGES = 3, CHOICES = 2, COURSE_VERSION = 1;
constexpr uint TICKS_PER_SECOND = 30, HORIZON = 240, CHECKPOINT_BUDGET = 660;
constexpr uint CONNECTOR_MM = 32000, MAX_ROLLOUTS = 16384, MAX_HORIZON = 480;

// Explicit std430-compatible records: 12 words per route, six routes, no pointers.
// Positions are millimetres; time is 1/30 second; probabilities are /10000.
struct alignas(16) Route {
    uint lengthMm, speedMmPerTick, gapStart, gapEnd;
    uint hurdleMm, gateMm, period, activeTicks;
    uint phase, warningTicks, jumpFailure, reactionMax;
};
struct Course { std::array<Route, STAGES * CHOICES> routes; };
struct alignas(16) Request {
    uint id = 0, courseVersion = COURSE_VERSION, run = 0, junction = 0;
    std::int32_t progressMm = 0;
    uint budgetTicks = CHECKPOINT_BUDGET, worldTick = 0, seed = 12345;
    uint rollouts = 512, horizon = HORIZON, routeCount = STAGES * CHOICES, reserved = 0;
};
struct alignas(16) Summary { uint survived = 0, progressMm = 0, steps = 0, rollouts = 0; };
struct Result {
    Request request;
    std::array<Summary, CHOICES> routes{};
    double latencyMs = 0, kernelMs = 0;
};
static_assert(sizeof(Route) == 48 && sizeof(Course) == 288, "course wire layout");
static_assert(sizeof(Request) == 48 && offsetof(Request, progressMm) == 16, "request wire layout");
static_assert(sizeof(Summary) == 16, "summary wire layout");
Course makeCourse();
void validate(const Course&, const Request&);
Result evaluate(const Course&, const Request&);
bool sameResults(const Result&, const Result&);
float survival(const Result&, uint choice);
bool gateActive(const Route&, uint worldTick);
bool gateWarning(const Route&, uint worldTick);
} // namespace prediction
