#pragma once
#include "../shaders/tunnel_config.inc"
#include <array>
#include <cstddef>
#include <cstdint>

namespace prediction {
using uint = std::uint32_t;
constexpr uint STAGES = 32, CHOICES = 4, COURSE_VERSION = 5;
constexpr uint TICKS_PER_SECOND = 60, ENTRY_TICK = TUNNEL_ENTRY_TICK, OBSTACLES = TUNNEL_OBSTACLES;
constexpr uint HORIZON = TUNNEL_SECTION_BASE, MAX_HORIZON = HORIZON, MAX_ROLLOUTS = 16384;
constexpr int PLAYER_RADIUS = TUNNEL_PLAYER_RADIUS, TUNNEL_LIMIT = TUNNEL_LOCAL_LIMIT,
              SHAFT_LIMIT = TUNNEL_SHAFT_LIMIT, MAX_VELOCITY = TUNNEL_MAX_VELOCITY,
              ACCELERATION = TUNNEL_ACCELERATION;

// Each obstacle is an extruded cross-section at a known arrival tick.
// kind: 0 triangular spike, 1 slider, 2 rotating bar, 3 contracting aperture.
// period is log2(period in ticks); motion uses masks/shifts, not divisions.
struct alignas(16) Obstacle {
    uint kind, phase, period, flags;
    int offset, extent, amplitude, opening;
};
struct alignas(16) Route {
    uint choiceCount, seed, reactionTicks, lateralNoise;
    std::array<Obstacle, OBSTACLES> obstacles;
};
struct Course {
    std::array<Route, STAGES * CHOICES> routes;
};
struct alignas(16) Request {
    uint id = 0, courseVersion = COURSE_VERSION, run = 0, junction = 0;
    int xMm = 0, velocity = 0;
    uint worldTick = 0, seed = 12345;
    uint rollouts = 512, horizon = HORIZON, routeCount = 2, reserved = 0;
    int zMm = 0, verticalVelocity = 0;
    uint reserved2 = 0, reserved3 = 0;
};
struct alignas(16) Summary {
    uint survived = 0, progressTicks = 0, steps = 0, rollouts = 0;
};
struct Result {
    Request request;
    std::array<Summary, CHOICES> routes{};
    double latencyMs = 0, kernelMs = 0;
};
struct Pose {
    int center, cosine, sine, aperture;
};
struct Target {
    int x, z;
};
static_assert(sizeof(Obstacle) == 32 && sizeof(Route) == 336 && sizeof(Course) == 43008,
              "course layout");
static_assert(sizeof(Request) == 64 && offsetof(Request, xMm) == 16 && offsetof(Request, zMm) == 48,
              "request layout");
static_assert(sizeof(Summary) == 16, "summary layout");
Course makeCourse(uint seed = 12345);
uint choices(const Course &, uint section);
const Route &route(const Course &, uint section, uint choice);
int laneCenter(uint count, uint choice);
Pose pose(Obstacle, uint tick);
uint sectionTicks(uint section);
uint sectionStartTick(uint section);
uint entryTick(uint section);
uint mergeTick(uint section);
uint contactTicks(uint section);
uint obstacleCount(uint section);
uint hazardTick(uint section, uint event);
bool withinTube(int x, int z, int limit);
bool blocked(Obstacle, int localX, int z, uint tick);
Target steeringTarget(Route, int localX, int z, uint eventTick, uint event, uint halfWindow);
void validate(const Course &, const Request &);
Result evaluate(const Course &, const Request &);
bool sameResults(const Result &, const Result &);
float survival(const Result &, uint choice);
bool applies(const Request &, uint run, uint junction, uint worldTick);
} // namespace prediction
