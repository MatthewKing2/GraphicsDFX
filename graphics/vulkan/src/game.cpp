#include "game.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace survival {
namespace {
const glm::vec3 cyan(.08f, .75f, .85f), violet(.5f, .12f, .8f), red(.95f, .025f, .065f),
    white(.8f, .9f, 1);
constexpr float tunnelRadius = TUNNEL_RADIUS * .001f, shaftRadius = TUNNEL_SHAFT_RADIUS * .001f;
const float tunnelApothem = tunnelRadius * std::cos(glm::radians(22.5f)),
            shaftApothem = shaftRadius * std::cos(glm::radians(22.5f));
void object(std::vector<SceneInstance> &scene, glm::vec3 center, glm::vec3 size, glm::vec3 color,
            float primitive = 1) {
    SceneInstance item;
    item.transform = glm::scale(glm::translate(glm::mat4(1), center), size);
    item.tint = glm::vec4(color, primitive);
    scene.push_back(item);
}
void triangle(std::vector<SceneInstance> &scene, glm::vec3 a, glm::vec3 b, glm::vec3 tip,
              float depth, glm::vec3 color) {
    // The existing triangular-prism primitive has base (-.5,-.5)..(.5,-.5)
    // and tip (0,.5) in local X/Z. Map it onto any front-wall triangle.
    if ((b.x - a.x) * (tip.z - a.z) - (b.z - a.z) * (tip.x - a.x) < 0)
        std::swap(a, b);
    glm::vec3 base = (a + b) * .5f;
    SceneInstance item;
    item.transform[0] = glm::vec4(b - a, 0);
    item.transform[1] = glm::vec4(0, depth, 0, 0);
    item.transform[2] = glm::vec4(tip - base, 0);
    item.transform[3] = glm::vec4((base + tip) * .5f, 1);
    item.tint = glm::vec4(color, 2);
    scene.push_back(item);
}
void mouthCorners(std::vector<SceneInstance> &scene, float x, float y) {
    float diagonal = tunnelRadius * std::sqrt(.5f);
    for (int sx : {-1, 1})
        for (int sz : {-1, 1}) {
            glm::vec3 corner(x + sx * tunnelRadius, y, sz * tunnelRadius),
                right(x + sx * tunnelRadius, y, 0), top(x, y, sz * tunnelRadius),
                tip(x + sx * diagonal, y, sz * diagonal);
            triangle(scene, right, corner, tip, .45f, {.018f, .02f, .032f});
            triangle(scene, corner, top, tip, .45f, {.018f, .02f, .032f});
        }
}
void rectangle(std::vector<SceneInstance> &scene, float x, float y, float w, float h,
               glm::vec3 color) {
    SceneInstance item;
    item.transform = glm::scale(
        glm::translate(glm::mat4(1), glm::vec3((x + w / 2) / 640 - 1, (y + h / 2) / 360 - 1, 0)),
        glm::vec3(w / 640, h / 360, 0));
    item.tint = glm::vec4(color, -1);
    scene.push_back(item);
}
std::array<uint8_t, 7> glyph(char c) {
    // Tiny 5x7 font; no font library, textures, or runtime allocations per glyph.
    switch (c) {
    case 'A':
        return {14, 17, 17, 31, 17, 17, 17};
    case 'B':
        return {30, 17, 17, 30, 17, 17, 30};
    case 'C':
        return {14, 17, 16, 16, 16, 17, 14};
    case 'D':
        return {30, 17, 17, 17, 17, 17, 30};
    case 'E':
        return {31, 16, 16, 30, 16, 16, 31};
    case 'F':
        return {31, 16, 16, 30, 16, 16, 16};
    case 'G':
        return {14, 17, 16, 23, 17, 17, 15};
    case 'H':
        return {17, 17, 17, 31, 17, 17, 17};
    case 'I':
        return {31, 4, 4, 4, 4, 4, 31};
    case 'J':
        return {7, 2, 2, 2, 18, 18, 12};
    case 'K':
        return {17, 18, 20, 24, 20, 18, 17};
    case 'L':
        return {16, 16, 16, 16, 16, 16, 31};
    case 'M':
        return {17, 27, 21, 21, 17, 17, 17};
    case 'N':
        return {17, 25, 21, 19, 17, 17, 17};
    case 'O':
        return {14, 17, 17, 17, 17, 17, 14};
    case 'P':
        return {30, 17, 17, 30, 16, 16, 16};
    case 'Q':
        return {14, 17, 17, 17, 21, 18, 13};
    case 'R':
        return {30, 17, 17, 30, 20, 18, 17};
    case 'S':
        return {15, 16, 16, 14, 1, 1, 30};
    case 'T':
        return {31, 4, 4, 4, 4, 4, 4};
    case 'U':
        return {17, 17, 17, 17, 17, 17, 14};
    case 'V':
        return {17, 17, 17, 17, 17, 10, 4};
    case 'W':
        return {17, 17, 17, 21, 21, 21, 10};
    case 'X':
        return {17, 17, 10, 4, 10, 17, 17};
    case 'Y':
        return {17, 17, 10, 4, 4, 4, 4};
    case 'Z':
        return {31, 1, 2, 4, 8, 16, 31};
    case '0':
        return {14, 17, 19, 21, 25, 17, 14};
    case '1':
        return {4, 12, 4, 4, 4, 4, 14};
    case '2':
        return {14, 17, 1, 2, 4, 8, 31};
    case '3':
        return {30, 1, 1, 14, 1, 1, 30};
    case '4':
        return {2, 6, 10, 18, 31, 2, 2};
    case '5':
        return {31, 16, 16, 30, 1, 1, 30};
    case '6':
        return {14, 16, 16, 30, 17, 17, 14};
    case '7':
        return {31, 1, 2, 4, 8, 8, 8};
    case '8':
        return {14, 17, 17, 14, 17, 17, 14};
    case '9':
        return {14, 17, 17, 15, 1, 1, 14};
    case ':':
        return {0, 4, 4, 0, 4, 4, 0};
    case '.':
        return {0, 0, 0, 0, 0, 4, 4};
    case '-':
        return {0, 0, 0, 31, 0, 0, 0};
    case '/':
        return {1, 2, 2, 4, 8, 8, 16};
    case '%':
        return {25, 25, 2, 4, 8, 19, 19};
    case '+':
        return {0, 4, 4, 31, 4, 4, 0};
    default:
        return {};
    }
}
void screenText(std::vector<SceneInstance> &scene, float x, float y, const std::string &value,
                float scale, glm::vec3 color) {
    for (char c : value) {
        auto rows = glyph(c);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (rows[row] & (1 << (4 - col)))
                    rectangle(scene, x + col * scale, y + row * scale, scale, scale, color);
        x += 6 * scale;
    }
}
void worldText(std::vector<SceneInstance> &scene, glm::vec3 center, const std::string &value,
               float scale, glm::vec3 color) {
    float x = center.x - float(value.size() * 6 - 1) * scale * .5f;
    for (char c : value) {
        auto rows = glyph(c);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (rows[row] & (1 << (4 - col)))
                    object(scene, {x + col * scale, center.y, center.z + (3 - row) * scale},
                           {scale * .88f, .035f, scale * .88f}, color, 3);
        x += 6 * scale;
    }
}
void ring(std::vector<SceneInstance> &scene, float x, float y, float radius, float thickness,
          glm::vec3 color, float z = 0) {
    for (int side = 0; side < 8; ++side) {
        float angle = (side + .5f) * glm::radians(45.0f);
        glm::vec3 normal(std::cos(angle), 0, std::sin(angle)),
            tangent(std::sin(angle), 0, -std::cos(angle));
        SceneInstance item;
        item.transform[0] = glm::vec4(tangent * (2 * radius * std::tan(glm::radians(22.5f))), 0);
        item.transform[1] = glm::vec4(0, thickness, 0, 0);
        item.transform[2] = glm::vec4(normal * .055f, 0);
        item.transform[3] = glm::vec4(glm::vec3(x, y, z) + normal * radius, 1);
        item.tint = glm::vec4(color, 3);
        scene.push_back(item);
    }
}
void tube(std::vector<SceneInstance> &scene, float x0, float x1, float y0, float y1, float radius,
          glm::vec3 tint) {
    if (y1 <= y0)
        return;
    for (int side = 0; side < 8; ++side) {
        float angle = (side + .5f) * glm::radians(45.0f);
        glm::vec3 normal(std::cos(angle), 0, std::sin(angle)),
            tangent(std::sin(angle), 0, -std::cos(angle));
        SceneInstance item;
        item.transform[0] =
            glm::vec4(tangent * (2 * radius * std::tan(glm::radians(22.5f)) + .02f), 0);
        item.transform[1] = glm::vec4(x1 - x0, y1 - y0, 0, 0);
        item.transform[2] = glm::vec4(normal * .15f, 0);
        item.transform[3] =
            glm::vec4(glm::vec3((x0 + x1) * .5f, (y0 + y1) * .5f, 0) + normal * radius, 1);
        item.tint = glm::vec4(tint * (side % 2 ? .7f : 1.0f), 1);
        scene.push_back(item);
    }
}
std::string seconds(float t) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << t;
    return out.str();
}
} // namespace
Mesh cubeMesh() {
    Mesh mesh;
    const glm::vec3 corners[8] = {{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, .5f, -.5f},
                                  {-.5f, .5f, -.5f},  {-.5f, -.5f, .5f}, {.5f, -.5f, .5f},
                                  {.5f, .5f, .5f},    {-.5f, .5f, .5f}};
    const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                             {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    const float shade[6] = {.36f, 1.0f, .8f, .62f, .55f, .72f};
    for (int f = 0; f < 6; ++f) {
        uint32_t base = uint32_t(mesh.vertices.size());
        for (int v : faces[f])
            mesh.vertices.push_back({corners[v], glm::vec3(shade[f]), {}});
        for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
            mesh.indices.push_back(base + index);
    }
    return mesh;
}

float distanceAt(float seconds) {
    return INITIAL_SPEED * seconds + .5f * FORWARD_ACCELERATION * seconds * seconds;
}
float routeCenter(const prediction::Course &course, unsigned stage, unsigned choice) {
    return prediction::laneCenter(prediction::choices(course, stage), choice) * .001f;
}
Game::Game(const prediction::Course &course, unsigned replaySeed) : data(course) {
    initialReactorSeed = replaySeed ? replaySeed : (std::random_device{}() | 1u);
    reactorRng = initialReactorSeed;
}
void Game::look(float dx, float dy) {
    // Relative mouse movement steers the player, not a freely flying camera.
    float center =
        player.branch < 0 ? 0.0f : routeCenter(data, player.stage, unsigned(player.branch));
    player.mouseTarget =
        std::clamp(player.mouseTarget + dx * .042f, -47.0f - center, 47.0f - center);
    player.mouseVerticalTarget = std::clamp(player.mouseVerticalTarget - dy * .042f, -47.0f, 47.0f);
}
void Game::restart(bool) {
    unsigned generation = player.run + 1;
    float best = std::max(player.best, player.elapsed);
    player = State{};
    player.run = generation;
    player.best = best;
    player.title = false;
    reactorRng = initialReactorSeed;
}
void Game::die(const char *why) {
    if (player.dead)
        return;
    player.dead = true;
    player.deathReason = why;
    ++player.deaths;
    player.best = std::max(player.best, player.elapsed);
}
void Game::step(const GameControls &input, float dt) {
    if (input.restart || ((player.title || player.dead) && input.jump)) {
        restart();
        return;
    }
    if (player.title || player.dead)
        return;
    const float before = player.elapsed;
    const float sectionStart = prediction::sectionStartTick(player.stage) / 60.0f;
    const float duration = prediction::sectionTicks(player.stage) / 60.0f;
    const float previousPhase = (before - sectionStart) / duration;
    const float targetSpeed =
        std::abs(input.strafe) > .01f
            ? input.strafe * STEER_SPEED
            : std::clamp((player.mouseTarget - player.localX) * 8.0f, -STEER_SPEED, STEER_SPEED);
    const float targetVerticalSpeed =
        std::abs(input.vertical) > .01f
            ? input.vertical * STEER_SPEED
            : std::clamp((player.mouseVerticalTarget - player.feet.z) * 8.0f, -STEER_SPEED,
                         STEER_SPEED);
    player.lateralSpeed += std::clamp(targetSpeed - player.lateralSpeed, -STEER_ACCELERATION * dt,
                                      STEER_ACCELERATION * dt);
    player.verticalSpeed += std::clamp(targetVerticalSpeed - player.verticalSpeed,
                                       -STEER_ACCELERATION * dt, STEER_ACCELERATION * dt);
    player.localX += player.lateralSpeed * dt;
    player.feet.z += player.verticalSpeed * dt;
    if (std::abs(input.strafe) > .01f)
        player.mouseTarget = player.localX;
    if (std::abs(input.vertical) > .01f)
        player.mouseVerticalTarget = player.feet.z;
    player.elapsed += dt;
    player.worldTime = player.elapsed;
    player.speed = INITIAL_SPEED + FORWARD_ACCELERATION * player.elapsed;
    player.feet.y = distanceAt(player.elapsed);
    const unsigned worldTick = unsigned(player.worldTime * 60);
    while ((player.reactorTick + 1) * prediction::REACTOR_TICK <= worldTick) {
        ++player.reactorTick;
        player.reactor = prediction::reactorStep(data, player.reactor,
                                                 player.reactorTick * prediction::REACTOR_TICK,
                                                 player.routePower, reactorRng);
    }
    if (input.jump && player.pulseCharges && worldTick >= player.pulseUntil) {
        --player.pulseCharges;
        ++player.pulsesUsed;
        player.pulseUntil = worldTick + prediction::PULSE_TICKS;
    }
    const float phase = (player.elapsed - sectionStart) / duration;
    const float entry = prediction::entryTick(player.stage) / (duration * 60);
    const float merge = prediction::mergeTick(player.stage) / (duration * 60);
    auto inside = [&](float x, int limit) {
        return prediction::withinTube(int(std::round(x * 1000)),
                                      int(std::round(player.feet.z * 1000)), limit);
    };
    if (player.branch < 0 && phase >= entry) {
        auto count = prediction::choices(data, player.stage);
        int closest = 0;
        float distance = 100;
        for (unsigned choice = 0; choice < count; ++choice) {
            float center = prediction::laneCenter(count, choice) * .001f;
            if (std::abs(player.localX - center) < distance) {
                distance = std::abs(player.localX - center);
                closest = int(choice);
            }
        }
        float center = prediction::laneCenter(count, unsigned(closest)) * .001f;
        player.localX -= center;
        player.mouseTarget -= center;
        player.branch = closest;
        if (!inside(player.localX, prediction::TUNNEL_LIMIT)) {
            die("MISSED THE OPENING");
            return;
        }
    }
    if (player.branch >= 0) {
        if (phase < merge && !inside(player.localX, prediction::TUNNEL_LIMIT)) {
            die("TUNNEL WALL");
            return;
        }
        const auto &route = prediction::route(data, player.stage, unsigned(player.branch));
        const float controlPhase =
            (prediction::entryTick(player.stage) +
             (prediction::mergeTick(player.stage) - prediction::entryTick(player.stage)) / 3u) /
            (duration * 60);
        const float controlWindow = 3.0f / (duration * 60);
        if (!player.collectedControl && previousPhase <= controlPhase + controlWindow &&
            phase >= controlPhase - controlWindow &&
            (player.localX + 4) * (player.localX + 4) + (player.feet.z - 4) * (player.feet.z - 4) <
                4) {
            player.collectedControl = true;
            player.routePower ^= 1u << (unsigned(player.branch) & 3u);
            player.pulseCharges = std::min(prediction::MAX_PULSES, player.pulseCharges + 1);
            player.pulseUntil = std::max(player.pulseUntil, worldTick + 36);
            ++player.reroutes;
        }
        for (unsigned event = 0; event < prediction::obstacleCount(player.stage); ++event) {
            float eventPhase = prediction::hazardTick(player.stage, event) / (duration * 60);
            // Test the whole visible hazard depth, not just its central plane.
            const float halfWindow = prediction::contactTicks(player.stage) / (duration * 60);
            if (previousPhase <= eventPhase + halfWindow && phase >= eventPhase - halfWindow) {
                unsigned tick = unsigned(player.worldTime * 60);
                if (prediction::reactorBlocked(route.obstacles[event], player.reactor,
                                               int(std::round(player.localX * 1000)),
                                               int(std::round(player.feet.z * 1000)), tick,
                                               player.pulseUntil)) {
                    const char *names[] = {"BROKEN STRUT", "CARGO RAIL", "LIVE LASER GRID",
                                           "INTERLOCK FAILURE"};
                    die(names[route.obstacles[event].kind]);
                    return;
                }
            }
        }
        player.feet.x = routeCenter(data, player.stage, unsigned(player.branch)) + player.localX;
    } else {
        player.feet.x = player.localX;
    }
    if (!inside(player.feet.x, prediction::SHAFT_LIMIT)) {
        die("OUTER SHAFT WALL");
        return;
    }
    if (phase >= 1) {
        // Change coordinate systems only: preserve world position, target and
        // velocity.
        player.mouseTarget += routeCenter(data, player.stage, unsigned(player.branch));
        player.localX = player.feet.x;
        ++player.stage;
        player.branch = -1;
        player.collectedControl = false;
    }
}
uint32_t Game::decision() const { return player.stage + (player.branch >= 0 ? 1u : 0u); }
bool Game::approachingJunction() const { return !player.title && !player.dead; }
prediction::Request Game::request(uint32_t id, uint32_t rollouts) const {
    prediction::Request r;
    r.id = id;
    r.run = player.run;
    r.junction = decision();
    // A next-fork preview assumes we arrive there at our current world-space x.
    // It is conditional on surviving the current tunnel, not a forecast of our
    // input.
    r.xMm = int(std::round(player.feet.x * 1000));
    r.zMm = int(std::round(player.feet.z * 1000));
    r.velocity = std::clamp(int(std::round(player.lateralSpeed * 1000 / 60)),
                            -prediction::MAX_VELOCITY, prediction::MAX_VELOCITY);
    r.verticalVelocity = std::clamp(int(std::round(player.verticalSpeed * 1000 / 60)),
                                    -prediction::MAX_VELOCITY, prediction::MAX_VELOCITY);
    r.worldTick = unsigned(player.worldTime * 60);
    r.rollouts = rollouts;
    r.routeCount = prediction::choices(data, r.junction);
    r.seed = 12345u ^ (r.junction * 0x9e3779b9u);
    r.reactor = player.reactor;
    r.routePower = player.routePower;
    r.pulseUntil = player.pulseUntil;
    r.pulseCharges = player.pulseCharges;
    return r;
}
prediction::Telemetry Game::telemetry() const {
    prediction::Telemetry packet{};
    packet.run = player.run;
    packet.tick = unsigned(player.worldTime * 60);
    packet.section = player.stage;
    packet.routePower = player.routePower;
    packet.pulseUntil = player.pulseUntil;
    packet.pulseCharges = player.pulseCharges;
    packet.version = prediction::COURSE_VERSION;
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned e = 0; e < prediction::OBSTACLES; ++e) {
            packet.current[c] |=
                prediction::hazardLevel(player.reactor,
                                        prediction::route(data, player.stage, c).obstacles[e])
                << (e * 3);
            packet.upcoming[c] |=
                prediction::hazardLevel(player.reactor,
                                        prediction::route(data, player.stage + 1, c).obstacles[e])
                << (e * 3);
        }
    for (unsigned node = 0; node < prediction::RELAYS; ++node)
        if (prediction::relayLevel(player.reactor, node) > 0)
            ++packet.warningCount;
    return packet;
}
FrameTransforms Game::transforms(float aspect) const {
    // Camera looks down the shaft. Geometry is rebased around the player each
    // frame so long runs don't lose small-scale precision.
    float roll = std::clamp(-player.lateralSpeed * .00267f, -.10f, .10f);
    glm::vec3 eye(player.feet.x, 0, player.feet.z), up(std::sin(roll), 0, std::cos(roll));
    FrameTransforms frame;
    frame.view = glm::lookAt(eye, eye + glm::vec3(0, 1, 0), up);
    frame.projection =
        glm::perspective(glm::radians(std::min(103.0f, 84.0f + player.elapsed * .15f)), aspect,
                         .08f, std::max(220.0f, player.speed * 8));
    frame.projection[1][1] *= -1;
    return frame;
}
int Game::score() const { return int(player.elapsed * 100); }

void Game::scene(std::vector<SceneInstance> &scene, const Guidance &guidance) const {
    scene.clear();
    const unsigned first = player.stage ? player.stage - 1 : 0;
    const unsigned target = decision();
    const float origin = player.feet.y;
    for (unsigned stage = first; stage <= player.stage + 2; ++stage) {
        float start = prediction::sectionStartTick(stage) / 60.0f;
        float entry = prediction::entryTick(stage) / 60.0f;
        float y0 = distanceAt(start) - origin, yEntry = distanceAt(start + entry) - origin;
        float yMerge = distanceAt(start + prediction::mergeTick(stage) / 60.0f) - origin,
              yEnd = distanceAt(start + prediction::sectionTicks(stage) / 60.0f) - origin;
        auto count = prediction::choices(data, stage);
        // Open shaft: cool green steel. Passages: rusty metal, with amber exit rims.
        tube(scene, 0, 0, y0, yEntry, shaftApothem, {.026f, .047f, .042f});
        tube(scene, 0, 0, yMerge, yEnd, shaftApothem, {.026f, .047f, .042f});
        ring(scene, 0, y0, shaftApothem, .09f, {.04f, .13f, .22f});
        // A dark bulkhead with 2..4 octagonal mouths, not a HUD route selector.
        object(scene, {0, yEntry, 30.0f}, {96, .45f, 39.0f}, {.018f, .02f, .032f});
        object(scene, {0, yEntry, -30.0f}, {96, .45f, 39.0f}, {.018f, .02f, .032f});
        float outer = float(count - 1) * TUNNEL_HALF_PITCH * .001f + tunnelRadius;
        for (int side : {-1, 1})
            object(scene, {side * (outer + (shaftRadius - outer) * .5f), yEntry, 0},
                   {std::max(.01f, shaftRadius - outer), .45f, 21.6f}, {.018f, .02f, .032f});
        for (unsigned divider = 0; divider + 1 < count; ++divider) {
            float x = (prediction::laneCenter(count, divider) +
                       prediction::laneCenter(count, divider + 1)) *
                      .0005f;
            object(scene, {x, yEntry, 0}, {3.0f, .5f, 21.6f}, {.025f, .02f, .04f});
        }
        for (unsigned choice = 0; choice < count; ++choice) {
            float x = prediction::laneCenter(count, choice) * .001f;
            mouthCorners(scene, x, yEntry);
            worldText(scene, {x, yEntry - .5f, 18.5f}, std::string("BUS ") + char('A' + choice),
                      .27f, {.75f, .48f, .12f});
            for (unsigned lamp = 0; lamp < 8; ++lamp) {
                auto age = prediction::relayLevel(player.reactor, choice * 32 + lamp);
                glm::vec3 light = age >= 4 ? red
                                  : age    ? glm::vec3(1, .45f, .04f)
                                           : glm::vec3(.08f, .32f, .24f);
                object(scene, {x + (float(lamp) - 3.5f) * .95f, yEntry - .3f, 11.8f},
                       {.55f, .1f, .35f}, light, 3);
            }
            auto rim = glm::vec3(.08f, .18f, .35f);
            if (stage == target && guidance.enabled && guidance.valid) {
                float probability = guidance.percentages[choice];
                rim = probability < .1f   ? red
                      : probability < .6f ? glm::vec3(.9f, .32f, .06f)
                                          : cyan;
                if (guidance.recommended == int(choice))
                    rim = glm::vec3(.15f, .95f, .55f);
            }
            tube(scene, x, x, yEntry, yMerge, tunnelApothem, {.073f, .049f, .027f});
            ring(scene, x, yEntry - .25f, tunnelApothem, .12f, rim);
            // Inset the rim so it is visible in front of, not buried inside, the
            // wall.
            ring(scene, x, yMerge - .25f, tunnelApothem - .12f, .16f, {.65f, .23f, .09f});
            for (int band = 1; band < 21; ++band) {
                float t = float(band) / 21;
                ring(scene, x, yEntry + (yMerge - yEntry) * t, tunnelApothem, .055f,
                     {.13f, .09f, .045f});
            }
            for (int side : {-1, 1}) {
                object(scene, {x + side * 9.3f, (yEntry + yMerge) * .5f, 1.5f},
                       {.20f, yMerge - yEntry, .20f}, {.12f, .08f, .025f});
                object(scene, {x + side * 9.3f, (yEntry + yMerge) * .5f, 1.7f},
                       {.08f, yMerge - yEntry, .08f}, {.35f, .19f, .035f}, 3);
                for (int stripe = 0; stripe < 5; ++stripe)
                    object(scene, {x + side * (6.2f + stripe * .6f), yEntry - .3f, -9.0f},
                           {.32f, .12f, .65f},
                           stripe % 2 ? glm::vec3(.02f) : glm::vec3(.8f, .45f, .04f), 3);
            }
            float controlTime =
                start + (prediction::entryTick(stage) +
                         (prediction::mergeTick(stage) - prediction::entryTick(stage)) / 3u) /
                            60.0f;
            if (stage != player.stage || !player.collectedControl) {
                float cy = distanceAt(controlTime) - origin;
                ring(scene, x - 4, cy, 2, .1f, {.12f, .9f, .42f}, 4);
                worldText(scene, {x - 4, cy - .1f, 6.8f}, "REROUTE", .13f, {.12f, .8f, .35f});
            }
            const auto &route = prediction::route(data, stage, choice);
            for (unsigned event = 0; event < prediction::obstacleCount(stage); ++event) {
                const auto &base = route.obstacles[event];
                unsigned tick = unsigned(player.worldTime * 60);
                unsigned age = prediction::hazardLevel(player.reactor, base);
                const auto o =
                    prediction::reactorObstacle(base, player.reactor, tick, player.pulseUntil);
                const auto pose = prediction::pose(o, unsigned(player.worldTime * 60));
                float y = distanceAt(start + prediction::hazardTick(stage, event) / 60.0f) - origin;
                float eventTime = start + prediction::hazardTick(stage, event) / 60.0f;
                float halfWindow = prediction::contactTicks(stage) / 60.0f;
                float depth =
                    distanceAt(eventTime + halfWindow) - distanceAt(eventTime - halfWindow);
                float cx = x + pose.center * .001f;
                if (o.kind == 0) {
                    SceneInstance spike;
                    spike.transform = glm::translate(
                        glm::mat4(1), {cx, y,
                                       (o.flags & 1u) ? TUNNEL_SPIKE_CENTER * .001f
                                                      : -TUNNEL_SPIKE_CENTER * .001f});
                    if (o.flags & 1u)
                        spike.transform =
                            glm::rotate(spike.transform, glm::radians(180.0f), glm::vec3(0, 1, 0));
                    spike.transform = glm::scale(
                        spike.transform, {o.extent * .002f, depth, TUNNEL_SPIKE_HEIGHT * .001f});
                    spike.tint = glm::vec4(red, 2);
                    scene.push_back(spike);
                } else if (o.kind == 1)
                    object(scene, {cx, y, 0},
                           {o.extent * .002f, depth, TUNNEL_SLIDER_HALF_HEIGHT * .002f},
                           {.95f, .2f, .055f});
                else if (o.kind == 2) {
                    SceneInstance blade;
                    float c = pose.cosine / 1024.0f, s = pose.sine / 1024.0f;
                    blade.transform[0] =
                        glm::vec4(c * o.extent * .002f, 0, s * o.extent * .002f, 0);
                    blade.transform[1] = glm::vec4(0, depth, 0, 0);
                    blade.transform[2] = glm::vec4(-s * TUNNEL_BLADE_HALF_THICKNESS * .002f, 0,
                                                   c * TUNNEL_BLADE_HALF_THICKNESS * .002f, 0);
                    blade.transform[3] = glm::vec4(cx, y, 0, 1);
                    bool active = tick >= player.pulseUntil && age >= 4;
                    blade.tint = glm::vec4(active ? red : glm::vec3(.8f, .45f, .02f), 3);
                    if (!active)
                        blade.transform[2] *= .045f;
                    if (active || age > 0)
                        scene.push_back(blade);
                    object(scene, {cx, y - depth * .5f - .02f, 0}, {.16f, .05f, .16f},
                           active ? red : glm::vec3(.1f, .4f, .3f), 3);
                    object(scene, {cx + c * base.extent * .001f, y, s * base.extent * .001f},
                           {.65f, depth, .65f}, {.12f, .14f, .13f});
                    object(scene, {cx - c * base.extent * .001f, y, -s * base.extent * .001f},
                           {.65f, depth, .65f}, {.12f, .14f, .13f});
                } else {
                    float gap = pose.aperture * .001f;
                    glm::vec3 warning = tick < player.pulseUntil ? glm::vec3(.1f, .7f, 1)
                                        : age >= 4               ? red
                                        : age                    ? glm::vec3(1, .45f, .025f)
                                                                 : glm::vec3(.1f, .6f, .35f);
                    for (int side : {-1, 1}) {
                        // Extend each moving edge to its own wall, including center offset.
                        float width =
                            std::max(.01f, tunnelRadius - side * pose.center * .001f - gap);
                        object(scene, {cx + side * (gap + width * .5f), y, 0},
                               {width, depth, tunnelRadius * 2}, {.14f, .12f, .065f});
                        object(scene, {cx + side * gap, y - depth * .5f - .02f, 0},
                               {.14f, .04f, gap * 2}, warning, 3);
                        float height = std::max(.01f, tunnelRadius - gap);
                        object(scene, {cx, y, side * (gap + height * .5f)},
                               {gap * 2, depth, height}, {.14f, .12f, .065f});
                        object(scene, {cx, y - depth * .5f - .02f, side * gap},
                               {gap * 2, .04f, .14f}, warning, 3);
                    }
                    worldText(scene, {cx, y - depth * .5f - .03f, 10.0f},
                              tick < player.pulseUntil ? "PULSE"
                              : age >= 4               ? "CLOSING"
                              : age                    ? "WARNING"
                                                       : "INTERLOCK",
                              .15f, warning);
                }
            }
            if (stage == target && guidance.enabled && !player.title) {
                std::string label =
                    guidance.valid
                        ? std::to_string(int(std::round(guidance.percentages[choice] * 100))) + "%"
                        : "...";
                worldText(scene, {x, yEntry - .8f, 14.3f}, label, .51f,
                          guidance.valid ? rim : glm::vec3(.15f, .25f, .4f));
            }
        }
    }
    // Physical pulse cells on a small cockpit brace, not a screen-space HUD.
    for (unsigned cell = 0; cell < 2; ++cell)
        object(scene, {player.feet.x + .9f + cell * .25f, 2.8f, player.feet.z - 1.5f},
               {.16f, .40f, .12f},
               cell < player.pulseCharges ? glm::vec3(.1f, .8f, 1) : glm::vec3(.035f), 3);
    if (unsigned(player.worldTime * 60) < player.pulseUntil)
        ring(scene, player.feet.x, 2.5f, 1.5f, .04f, {.12f, .6f, 1}, player.feet.z);
    // Sparse streaks emphasize acceleration without particle buffers or effects.
    for (int index = 0; index < 20; ++index) {
        float angle = index * 2.399963f;
        float y = std::fmod(index * 13.7f - player.feet.y, 95.0f);
        if (y < 0)
            y += 95;
        object(scene, {std::cos(angle) * 44.1f, y, std::sin(angle) * 44.1f},
               {.025f, std::min(3.5f, player.speed * .07f), .025f}, {.045f, .14f, .2f}, 3);
    }
    // No gameplay HUD, score panel, crosshair or screen-space probabilities.
    if (player.title || player.dead || guidance.paused) {
        rectangle(scene, 245, 190, 790, 340, {.008f, .012f, .023f});
        rectangle(scene, 245, 190, 790, 3, player.dead ? red : cyan);
        screenText(scene, 300, 225, player.dead ? "SYSTEM LOST" : "REACTOR / RUN", 7,
                   player.dead ? red : white);
        if (player.dead) {
            screenText(scene, 300, 315, player.deathReason, 3, violet);
            screenText(scene, 300, 360,
                       seconds(player.elapsed) + " SECONDS / " + std::to_string(player.stage) +
                           " FORKS",
                       3, white);
            screenText(scene, 300, 405, "BEST " + seconds(player.best) + " SECONDS", 3, cyan);
            screenText(scene, 300, 470, "SPACE TO DIVE AGAIN", 3, white);
        } else if (guidance.paused) {
            screenText(scene, 300, 330, "PAUSED", 4, violet);
            screenText(scene, 300, 420, "ESC TO KEEP FALLING", 3, white);
        } else {
            screenText(scene, 300, 315, "WASD OR MOUSE: STEER", 3, cyan);
            screenText(scene, 300, 355, "SPACE: EMERGENCY PULSE", 3, white);
            screenText(scene, 300, 400, "GREEN RINGS: REROUTE POWER", 2, violet);
            screenText(scene, 300, 470, "SPACE TO DIVE", 4, white);
        }
    }
}
GameControls Autopilot::sample(const Game &game) {
    const auto &p = game.state();
    GameControls controls;
    if (p.title || p.dead) {
        controls.jump = true;
        return controls;
    }
    if (run != p.run) {
        run = p.run;
        section = ~0u;
    }
    auto tick = unsigned(p.worldTime * 60);
    prediction::Target target{0, 0};
    if (p.branch < 0) {
        if (section != p.stage) {
            section = p.stage;
            targetChoice = 0;
            int best = 2147483647;
            for (unsigned c = 0; c < prediction::choices(game.course(), p.stage); ++c) {
                const auto &route = prediction::route(game.course(), p.stage, c);
                int cost = int(
                    std::abs(prediction::laneCenter(route.choiceCount, c) * .001f - p.localX) * 20);
                for (unsigned e = 0; e < prediction::obstacleCount(p.stage); ++e)
                    cost += int(prediction::hazardLevel(p.reactor, route.obstacles[e])) * 100;
                if (cost < best) {
                    best = cost;
                    targetChoice = c;
                }
            }
        }
        target.x =
            prediction::laneCenter(prediction::choices(game.course(), p.stage), targetChoice);
    } else {
        const auto &route = prediction::route(game.course(), p.stage, unsigned(p.branch));
        auto start = prediction::sectionStartTick(p.stage);
        for (unsigned e = 0; e < prediction::obstacleCount(p.stage); ++e) {
            auto arrival = start + prediction::hazardTick(p.stage, e);
            if (tick <= arrival + prediction::contactTicks(p.stage)) {
                target = prediction::reactorTarget(route, p.reactor, int(p.localX * 1000),
                                                   int(p.feet.z * 1000), arrival, e, p.pulseUntil);
                if (arrival <= tick + 18 && route.obstacles[e].kind == 3 &&
                    prediction::hazardLevel(p.reactor, route.obstacles[e]) >= 5 && p.pulseCharges &&
                    tick >= p.pulseUntil)
                    controls.jump = true;
                break;
            }
        }
    }
    controls.strafe = std::clamp((target.x * .001f - p.localX) * 8 / STEER_SPEED, -1.0f, 1.0f);
    controls.vertical = std::clamp((target.z * .001f - p.feet.z) * 8 / STEER_SPEED, -1.0f, 1.0f);
    if (std::abs(controls.strafe) < .011f)
        controls.strafe = 0;
    if (std::abs(controls.vertical) < .011f)
        controls.vertical = 0;
    return controls;
}
void selfTest() {
    auto require = [](bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    auto course = prediction::makeCourse();
    prediction::Request r;
    r.rollouts = 32;
    r.routeCount = prediction::choices(course, 0);
    prediction::validate(course, r);
    bool counts[5] = {}, kinds[4] = {};
    for (unsigned stage = 0; stage < 32; ++stage) {
        counts[prediction::choices(course, stage)] = true;
        require(prediction::sectionStartTick(stage + 1) ==
                    prediction::sectionStartTick(stage) + prediction::sectionTicks(stage),
                "course timing gap");
        for (unsigned c = 0; c < prediction::choices(course, stage); ++c)
            for (const auto &o : prediction::route(course, stage, c).obstacles)
                kinds[o.kind] = true;
    }
    require(counts[2] && counts[3] && counts[4] && kinds[0] && kinds[1] && kinds[2] && kinds[3],
            "missing route/hazard variation");
    prediction::ReactorState healthy{}, failed{};
    for (auto &word : failed.words)
        word = ~0u;
    prediction::Obstacle gate{3, 0, 8, 0, 0, 9300, 0, 9300}, laser{2, 0, 8, 0, 0, 8400, 0, 0};
    require(!prediction::reactorBlocked(gate, healthy, 0, 0, 10, 0),
            "healthy interlock blocks player");
    require(prediction::reactorBlocked(gate, failed, 0, 0, 10, 0), "failed interlock not closed");
    require(!prediction::reactorBlocked(gate, failed, 0, 0, 10, 82), "pulse does not open gate");
    require(!prediction::reactorBlocked(laser, healthy, 0, 0, 10, 0) &&
                prediction::reactorBlocked(laser, failed, 0, 0, 10, 0) &&
                !prediction::reactorBlocked(laser, failed, 0, 0, 10, 82),
            "laser/pulse boundary");
    require(!prediction::reactorBlocked(laser, failed, 0, 4000, 0, 0),
            "vertical laser escape broken");
    unsigned a = 12345, b = a, c = a;
    prediction::ReactorState x{}, y{}, boost{};
    for (unsigned tick = 6; tick <= 600; tick += 6) {
        x = prediction::reactorStep(course, x, tick, 0, a);
        y = prediction::reactorStep(course, y, tick, 0, b);
        boost = prediction::reactorStep(course, boost, tick, 1, c);
    }
    bool identical = true, different = false, nonzero = false;
    for (unsigned w = 0; w < 16; ++w) {
        identical &= x.words[w] == y.words[w];
        different |= x.words[w] != boost.words[w];
        nonzero |= x.words[w] != 0;
    }
    require(identical && different && nonzero, "circuit determinism or rerouting broken");
    for (unsigned node = 0; node < 128; ++node)
        require(prediction::relayLevel(x, node) <= 7, "age overflow");
    std::cout << "PASS coupled 128-relay state, deterministic streams, warning "
                 "ages, rerouting and "
                 "pulse overrides\n";
    auto result = prediction::evaluate(course, r), repeat = prediction::evaluate(course, r);
    require(prediction::sameResults(result, repeat), "forecast not deterministic");
    r.reactor = failed;
    r.pulseCharges = 0;
    auto damaged = prediction::evaluate(course, r);
    require(!prediction::sameResults(result, damaged), "predictor ignores reactor snapshot");
    std::cout << "PASS forecast depends on supplied circuit state (not a "
                 "decorative simulation)\n";
    Game game(course, 12345);
    game.restart();
    GameControls pulse;
    pulse.jump = true;
    game.step(pulse, 1.0f / 120);
    require(game.state().pulseCharges == 1 && game.state().pulsesUsed == 1,
            "pulse input not consumed");
    for (unsigned tick = 0; tick < 120 * 12 && game.state().reroutes == 0 && !game.state().dead;
         ++tick) {
        GameControls input;
        float targetX = game.state().branch < 0 ? -12.0f : -4.0f;
        float targetZ = game.state().branch < 0 ? 0.0f : 4.0f;
        input.strafe = std::clamp((targetX - game.state().localX) * 8 / STEER_SPEED, -1.0f, 1.0f);
        input.vertical = std::clamp((targetZ - game.state().feet.z) * 8 / STEER_SPEED, -1.0f, 1.0f);
        float beforeX = game.state().feet.x, beforeZ = game.state().feet.z;
        game.step(input, 1.0f / 120);
        if (!game.state().dead)
            require(std::abs(game.state().feet.x - beforeX - game.state().lateralSpeed / 120) <
                            .0001f &&
                        std::abs(game.state().feet.z - beforeZ - game.state().verticalSpeed / 120) <
                            .0001f,
                    "automatic steering/recentering");
    }
    require(game.state().reroutes == 1 && game.state().routePower == 1 &&
                game.state().pulseCharges == 2,
            "control-ring pickup did not reroute/recharge");
    auto packet = game.telemetry();
    require(sizeof(packet) == 64 && packet.routePower == 1, "compressed telemetry invalid");
    for (unsigned choice = 0; choice < 4; ++choice)
        for (unsigned e = 0; e < 10; ++e)
            require(((packet.current[choice] >> (3 * e)) & 7) ==
                        prediction::hazardLevel(
                            game.state().reactor,
                            prediction::route(course, game.state().stage, choice).obstacles[e]),
                    "telemetry compression lost hazard state");
    auto snapshot = game.request(2, 32);
    require(snapshot.routePower == 1 && snapshot.reactor.words[0] == game.state().reactor.words[0],
            "request missing live reactor state");
    std::cout << "PASS actual pulse, ring pickup, recharge, live snapshot and "
                 "64-byte telemetry\n";
    std::vector<SceneInstance> pieces;
    mouthCorners(pieces, 0, 0);
    require(pieces.size() == 8, "missing octagonal mouth corner geometry");
    require(prediction::withinTube(0, prediction::TUNNEL_LIMIT, prediction::TUNNEL_LIMIT) &&
                !prediction::withinTube(prediction::TUNNEL_LIMIT, prediction::TUNNEL_LIMIT,
                                        prediction::TUNNEL_LIMIT),
            "octagonal collision broken");
    r.run = 4;
    r.junction = 0;
    r.worldTick = 10;
    require(prediction::applies(r, 4, 0, 20) && !prediction::applies(r, 5, 0, 20) &&
                !prediction::applies(r, 4, 0, 45),
            "stale forecast accepted");
    std::cout << "PASS octagonal geometry, two-axis motion and stale-result "
                 "rejection\n";
}
} // namespace survival
