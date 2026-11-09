#include "prediction.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace prediction {
using std::max;
using std::min;
#define COURSE_PARAM const Course &course,
#include "../shaders/prediction_kernel.inc"
#undef COURSE_PARAM

Course makeCourse() {
    Course course{};
    for (uint stage = 0; stage < STAGES; ++stage) {
        // Short orange route: two demanding jumps. Blue route: slower movement,
        // smaller gap, more forgiving jumps, and a long-open timed gate.
        course.routes[stage * 2] = {
            82000, 267, 28000, 30000, 44000, 58000, 150, 42, stage * 37, 21, 700 + stage * 100, 6};
        course.routes[stage * 2 + 1] = {
            82000, 227, 33000, 34400, 0, 50000, 180, 30, 65 + stage * 41, 24, 450 + stage * 100, 9};
    }
    return course;
}
void validate(const Course &course, const Request &r) {
    if (r.courseVersion != COURSE_VERSION || r.junction >= STAGES || r.routeCount != 6 ||
        r.rollouts == 0 || r.rollouts > MAX_ROLLOUTS || r.horizon == 0 || r.horizon > MAX_HORIZON ||
        r.progressMm < -32000 || r.progressMm > 82000 || r.budgetTicks > 10000 ||
        r.worldTick > 10000000) {
        throw std::invalid_argument("prediction request outside documented bounds");
    }
    for (const auto &route : course.routes) {
        if (route.lengthMm == 0 || route.lengthMm > 100000 || route.speedMmPerTick < 20 ||
            route.speedMmPerTick > 500 || route.period == 0 || route.period > 10000 ||
            route.activeTicks >= route.period ||
            route.warningTicks >= route.period - route.activeTicks || route.phase > 10000 ||
            route.jumpFailure > 10000 || route.reactionMax > 120 || route.gapStart > route.gapEnd ||
            route.gapEnd > route.lengthMm || route.hurdleMm > route.lengthMm ||
            route.gateMm > route.lengthMm) {
            throw std::invalid_argument("invalid corridor record");
        }
    }
}
Result evaluate(const Course &course, const Request &request) {
    validate(course, request);
    const auto start = std::chrono::steady_clock::now();
    Result result;
    result.request = request;
    for (uint choice = 0; choice < CHOICES; ++choice) {
        auto &sum = result.routes[choice];
        for (uint index = 0; index < request.rollouts; ++index) {
            const auto r = rollout(course, request, choice, index);
            sum.survived += r.survived;
            sum.progressMm += r.progress;
            sum.steps += r.steps;
            ++sum.rollouts;
        }
    }
    result.latencyMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    result.kernelMs = result.latencyMs;
    return result;
}
bool sameResults(const Result &a, const Result &b) {
    for (uint c = 0; c < CHOICES; ++c) {
        if (a.routes[c].survived != b.routes[c].survived ||
            a.routes[c].progressMm != b.routes[c].progressMm ||
            a.routes[c].steps != b.routes[c].steps || a.routes[c].rollouts != b.routes[c].rollouts)
            return false;
    }
    return true;
}
float survival(const Result &r, uint c) {
    return r.routes[c].rollouts ? float(r.routes[c].survived) / r.routes[c].rollouts : 0.0f;
}
bool gateActive(const Route &r, uint tick) {
    return (tick + r.phase) % r.period < r.activeTicks;
}
bool gateWarning(const Route &r, uint tick) {
    const uint phase = (tick + r.phase) % r.period;
    return phase >= r.period - r.warningTicks;
}
bool applies(const Request &request, uint run, uint junction, uint worldTick) {
    return request.run == run && request.junction == junction &&
           request.courseVersion == COURSE_VERSION && worldTick >= request.worldTick &&
           worldTick - request.worldTick <= 30;
}
} // namespace prediction
