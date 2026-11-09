// Shared CPU/GLSL scalar algorithm. No floating point, division, mesh accesses,
// recursion, unbounded loops, or external memory traffic per simulated step.
// COURSE_PARAM is a C++ const reference; GLSL reads its resident course SSBO.
struct RolloutResult { uint survived; uint progress; uint steps; };

uint randomNext(uint state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
uint mixSeed(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value | 1u;
}

RolloutResult rollout(COURSE_PARAM Request request, uint choice, uint number) {
    RolloutResult result;
    result.survived = 0u; result.progress = 0u; result.steps = 0u;
    uint rng = mixSeed(request.seed ^ (number * 0x9e3779b9u) ^ (choice * 0x85ebca6bu));
    uint stage = request.junction;
    uint branch = choice;
    int position = request.progressMm;
    uint budget = request.budgetTicks;
    uint tick = request.worldTick;
    Route route = course.routes[stage * 2u + branch];
    rng = randomNext(rng);
    uint delay = rng % (route.reactionMax + 1u);
    rng = randomNext(rng);
    uint speedVariation = rng % 17u; // 0..16 mm/tick slower than nominal (~6%).
    rng = randomNext(rng);
    int timingError = int(rng % 7u) - 3; // modeled arrival/timing uncertainty, not random real hazards
    bool gateReacted = false;
    for (uint step = 0u; step < request.horizon; ++step) {
        result.steps += 1u;
        if (budget == 0u) return result;
        budget -= 1u;
        tick += 1u;
        if (delay > 0u) { delay -= 1u; continue; }
        int previous = position;
        int next = position + int(route.speedMmPerTick - speedVariation);
        if (route.gapStart > 0u && previous < int(route.gapStart) && next >= int(route.gapStart)) {
            rng = randomNext(rng);
            if (rng % 10000u < route.jumpFailure) return result;
        }
        if (route.hurdleMm > 0u && previous < int(route.hurdleMm) && next >= int(route.hurdleMm)) {
            rng = randomNext(rng);
            if (rng % 10000u < route.jumpFailure / 2u) return result;
        }
        if (route.gateMm > 0u && previous < int(route.gateMm + 600u) && next >= int(route.gateMm) - 900) {
            uint phase = uint(int(tick + route.phase + route.period) + timingError) % route.period;
            if (phase < route.activeTicks) {
                if (!gateReacted) {
                    gateReacted = true;
                    rng = randomNext(rng);
                    if (rng % 10000u < 700u) return result; // late braking once, not once per wait tick
                }
                next = min(next, int(route.gateMm) - 901);
            }
        }
        position = max(previous, next);
        result.progress += uint(position - previous);
        if (position >= int(route.lengthMm)) {
            stage += 1u;
            if (stage >= 3u) { result.survived = 1u; return result; }
            position -= int(route.lengthMm + 32000u);
            budget = 660u; // next checkpoint: identical to real pursuit reset
            Route left = course.routes[stage * 2u];
            Route right = course.routes[stage * 2u + 1u];
            // Documented continuation: shortest nominal travel time, left on ties.
            branch = left.lengthMm * right.speedMmPerTick <= right.lengthMm * left.speedMmPerTick ? 0u : 1u;
            route = course.routes[stage * 2u + branch];
            gateReacted = false;
        }
    }
    result.survived = 1u;
    return result;
}
