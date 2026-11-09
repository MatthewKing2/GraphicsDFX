#include "prediction.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
namespace prediction {
using std::abs;
using std::clamp;
using std::max;
using std::min;
#define COURSE_PARAM const Course &course,
#include "../shaders/prediction_kernel.inc"
#undef COURSE_PARAM

Course makeCourse(uint seed) {
    Course course{};
    for (uint stage = 0; stage < STAGES; ++stage) {
        uint rng = mixSeed(seed ^ (stage * 0x9e3779b9u));
        uint count = stage == 0u ? 2u : 2u + rng % 3u;
        uint safe = (rng >> 8u) % count;
        uint sealed = (safe + 1u + (rng >> 16u) % (count - 1u)) % count;
        for (uint choice = 0; choice < CHOICES; ++choice) {
            auto &r = course.routes[stage * 4 + choice];
            rng = randomNext(rng);
            r.choiceCount = count;
            r.seed = rng;
            r.reactionTicks = 4u + (rng & 3u);
            r.lateralNoise = 70u + ((rng >> 8u) & 127u);
            // Gentle opening sections; size and motion ramp with forward speed.
            const int difficulty = int(std::min(stage, 12u));
            for (uint event = 0; event < OBSTACLES; ++event) {
                rng = randomNext(rng);
                auto &o = r.obstacles[event];
                o.kind = (stage + choice + event) % 4;
                o.phase = rng & 255u;
                o.period = stage < 4u ? 9u : stage < 10u ? 8u : 7u;
                o.flags = (rng >> 12u) & 3u;
                o.offset = int((rng >> 16u) % 1601u) - 800;
                o.extent = o.kind == 0   ? 1000 + difficulty * 80
                           : o.kind == 1 ? 200 + difficulty * 35
                                         : 1600 + difficulty * 100;
                o.amplitude = 600 + difficulty * 60;
                // The crusher has a wide safe gap initially, then closes further.
                if (o.kind == 3)
                    o.extent = 3100;
                o.opening = 2200 - difficulty * 100;
            }
            if (choice == safe) {
                r.reactionTicks = 4;
                r.lateralNoise = 80;
                for (uint event = 0; event < OBSTACLES; ++event)
                    r.obstacles[event] = {0,    0, 9, event & 1u, (event & 1u) ? 2400 : -2400,
                                          1400, 0, 0};
            } else if (choice == sealed && stage >= 3u && (stage % 3u) != 0u) {
                // Genuinely impassable: even the widest aperture is smaller than
                // the player. The probability is computed, not hard-coded to 0.
                r.obstacles[OBSTACLES - 1] = {3, 0, 7, 0, 0, 90, 0, 60};
            }
        }
    }
    return course;
}
uint choices(const Course &c, uint section) {
    return c.routes[(section & 31u) * 4].choiceCount;
}
const Route &route(const Course &c, uint section, uint choice) {
    return c.routes[(section & 31u) * 4 + choice];
}
void validate(const Course &c, const Request &r) {
    if (r.courseVersion != COURSE_VERSION || r.rollouts == 0 || r.rollouts > MAX_ROLLOUTS ||
        r.horizon != HORIZON || r.routeCount != choices(c, r.junction) || r.routeCount < 2 ||
        r.routeCount > 4 || r.junction > 60000 || r.worldTick > 50000000 || r.xMm < -SHAFT_LIMIT ||
        r.xMm > SHAFT_LIMIT || r.velocity < -MAX_VELOCITY || r.velocity > MAX_VELOCITY ||
        r.worldTick > r.junction * SECTION_TICKS + ENTRY_TICK)
        throw std::invalid_argument("invalid tunnel prediction request");
    for (const auto &route : c.routes) {
        if (route.choiceCount < 2 || route.choiceCount > 4 || route.reactionTicks > 20 ||
            route.lateralNoise > 500)
            throw std::invalid_argument("invalid tunnel route");
        for (const auto &o : route.obstacles)
            if (o.kind > 3 || o.period < 6 || o.period > 9 || std::abs(o.offset) > 3000 ||
                o.extent < 0 || o.extent > 4000 || o.amplitude < 0 || o.amplitude > 2000 ||
                o.opening < 0 || (o.kind == 3u && o.opening > o.extent))
                throw std::invalid_argument("invalid tunnel obstacle");
    }
}
Result evaluate(const Course &c, const Request &r) {
    validate(c, r);
    auto start = std::chrono::steady_clock::now();
    Result result;
    result.request = r;
    for (uint choice = 0; choice < r.routeCount; ++choice)
        for (uint n = 0; n < r.rollouts; ++n) {
            auto one = rollout(c, r, choice, n);
            auto &sum = result.routes[choice];
            sum.survived += one.survived;
            sum.progressTicks += one.progress;
            sum.steps += one.steps;
            ++sum.rollouts;
        }
    result.latencyMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    result.kernelMs = result.latencyMs;
    return result;
}
bool sameResults(const Result &a, const Result &b) {
    for (uint c = 0; c < CHOICES; ++c)
        if (a.routes[c].survived != b.routes[c].survived ||
            a.routes[c].progressTicks != b.routes[c].progressTicks ||
            a.routes[c].steps != b.routes[c].steps || a.routes[c].rollouts != b.routes[c].rollouts)
            return false;
    return true;
}
float survival(const Result &r, uint c) {
    return r.routes[c].rollouts ? float(r.routes[c].survived) / r.routes[c].rollouts : 0;
}
bool applies(const Request &r, uint run, uint junction, uint tick) {
    return r.run == run && r.junction == junction && r.courseVersion == COURSE_VERSION &&
           tick >= r.worldTick && tick - r.worldTick <= 30;
}
} // namespace prediction
