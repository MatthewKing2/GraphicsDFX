#include "prediction.hpp"
#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
namespace prediction {
using std::abs;
using std::clamp;
using std::max;
using std::min;
#define COURSE_PARAM const Course &course,
#define COURSE_ARG course,
#define INOUT_UINT uint &
#include "../shaders/prediction_kernel.inc"
#undef COURSE_PARAM
#undef COURSE_ARG
#undef INOUT_UINT

Course makeCourse(uint seed) {
    Course course{};
    uint circuitRng = mixSeed(seed ^ 0x72656163u);
    for (uint node = 0; node < RELAYS; ++node) {
        circuitRng = randomNext(circuitRng);
        uint a = (node + 127u) & 127u, b = (node + 17u) & 127u, c = (circuitRng >> 9u) & 127u;
        course.reactorRules[node] =
            a | (b << 7u) | (c << 14u) | ((node >> 5u) << 21u) | ((4u + (circuitRng & 7u)) << 23u);
    }
    for (uint stage = 0; stage < STAGES; ++stage) {
        uint rng = mixSeed(seed ^ (stage * 0x9e3779b9u));
        uint count = stage == 0u ? 2u : 2u + rng % 3u;
        for (uint choice = 0; choice < CHOICES; ++choice) {
            auto &r = course.routes[stage * 4 + choice];
            rng = randomNext(rng);
            r.choiceCount = count;
            r.seed = rng;
            r.reactionTicks = 4u + (rng & 3u);
            r.lateralNoise = 3u * (70u + ((rng >> 8u) & 127u));
            // Gentle opening sections; size and motion ramp with forward speed.
            const int difficulty = int(std::min(stage, 12u));
            for (uint event = 0; event < OBSTACLES; ++event) {
                rng = randomNext(rng);
                auto &o = r.obstacles[event];
                o.kind = (stage + choice + event) % 4;
                o.phase = rng & 255u;
                o.period = stage < 4u ? 9u : stage < 10u ? 8u : 7u;
                o.flags = ((rng >> 12u) & 3u) | ((choice * 32u + ((rng >> 20u) & 31u)) << 8u);
                o.offset = 3 * (int((rng >> 16u) % 1601u) - 800);
                o.extent = 3 * (o.kind == 0   ? 1000 + difficulty * 80
                                : o.kind == 1 ? 200 + difficulty * 35
                                              : 1600 + difficulty * 100);
                o.amplitude = 3 * (600 + difficulty * 60);
                // The crusher has a wide safe gap initially, then closes further.
                if (o.kind == 3)
                    o.extent = 9300;
                o.opening = 3 * (2200 - difficulty * 100);
            }
            // Every route has a real powered interlock, not a guaranteed-safe marker.
            auto &gate = r.obstacles[obstacleCount(stage) - 1];
            gate.kind = 3;
            gate.extent = 9300;
            gate.opening = 9300;
            // Familiar, gentle broken struts in the introductory passage.
            if (stage == 0 && choice == 0) {
                for (uint event = 0; event + 1 < obstacleCount(stage); ++event) {
                    auto &o = r.obstacles[event];
                    o.kind = 0;
                    o.offset = (event & 1u) ? 7200 : -7200;
                    o.extent = 3000;
                }
            }
        }
    }
    return course;
}
uint choices(const Course &c, uint section) { return c.routes[(section & 31u) * 4].choiceCount; }
const Route &route(const Course &c, uint section, uint choice) {
    return c.routes[(section & 31u) * 4 + choice];
}
void validate(const Course &c, const Request &r) {
    if (r.courseVersion != COURSE_VERSION || r.rollouts == 0 || r.rollouts > MAX_ROLLOUTS ||
        r.horizon != HORIZON || r.routeCount != choices(c, r.junction) || r.routeCount < 2 ||
        r.routeCount > 4 || r.junction > 60000 || r.worldTick > 100000000 || r.xMm < -SHAFT_LIMIT ||
        r.xMm > SHAFT_LIMIT || r.velocity < -MAX_VELOCITY || r.velocity > MAX_VELOCITY ||
        r.zMm < -SHAFT_LIMIT || r.zMm > SHAFT_LIMIT || r.verticalVelocity < -MAX_VELOCITY ||
        r.verticalVelocity > MAX_VELOCITY ||
        r.worldTick > sectionStartTick(r.junction) + entryTick(r.junction) ||
        (r.routePower & ~271u) != 0 || r.pulseCharges > MAX_PULSES || r.pulseUntil > 100000100)
        throw std::invalid_argument("invalid tunnel prediction request");
    for (const auto &route : c.routes) {
        if (route.choiceCount < 2 || route.choiceCount > 4 || route.reactionTicks > 20 ||
            route.lateralNoise > 1500)
            throw std::invalid_argument("invalid tunnel route");
        for (const auto &o : route.obstacles)
            if (o.kind > 3 || o.period < 6 || o.period > 9 || std::abs(o.offset) > 9000 ||
                o.extent < 0 || o.extent > 12000 || o.amplitude < 0 || o.amplitude > 6000 ||
                o.opening < 0 || (o.kind == 3u && o.opening > o.extent))
                throw std::invalid_argument("invalid tunnel obstacle");
    }
}
Result evaluate(const Course &c, const Request &r) {
    validate(c, r);
    auto start = std::chrono::steady_clock::now();
    Result result;
    result.request = r;
    auto work = [&](uint choice) {
        Summary sum;
        for (uint n = 0; n < r.rollouts; ++n) {
            auto one = rollout(c, r, choice, n);
            sum.survived += one.survived;
            sum.progressTicks += one.progress;
            sum.steps += one.steps;
            ++sum.rollouts;
        }
        return sum;
    };
    // Routes are independent. Local aggregates avoid cache-line sharing in the
    // loop. Keep tiny correctness checks serial; use up to four workers for real
    // requests.
    if (r.rollouts >= 128) {
        std::array<std::future<Summary>, CHOICES> jobs;
        for (uint choice = 0; choice < r.routeCount; ++choice)
            jobs[choice] = std::async(std::launch::async, work, choice);
        for (uint choice = 0; choice < r.routeCount; ++choice)
            result.routes[choice] = jobs[choice].get();
    } else {
        for (uint choice = 0; choice < r.routeCount; ++choice)
            result.routes[choice] = work(choice);
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
