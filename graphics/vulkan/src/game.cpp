#include "game.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace survival {
namespace {
const glm::vec3 cyan(.08f, .75f, .85f), violet(.5f, .12f, .8f), red(.95f, .025f, .065f),
    white(.8f, .9f, 1);
constexpr float tunnelRadius = 3.5f, shaftRadius = 16.0f;
const float tunnelApothem = tunnelRadius * std::cos(glm::radians(22.5f)),
            shaftApothem = shaftRadius * std::cos(glm::radians(22.5f));
void object(std::vector<SceneInstance> &scene, glm::vec3 center, glm::vec3 size, glm::vec3 color,
            float primitive = 1) {
    SceneInstance item;
    item.transform = glm::scale(glm::translate(glm::mat4(1), center), size);
    item.tint = glm::vec4(color, primitive);
    scene.push_back(item);
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
          glm::vec3 color) {
    for (int side = 0; side < 8; ++side) {
        float angle = (side + .5f) * glm::radians(45.0f);
        glm::vec3 normal(std::cos(angle), 0, std::sin(angle)),
            tangent(std::sin(angle), 0, -std::cos(angle));
        SceneInstance item;
        item.transform[0] = glm::vec4(tangent * (2 * radius * std::tan(glm::radians(22.5f))), 0);
        item.transform[1] = glm::vec4(0, thickness, 0, 0);
        item.transform[2] = glm::vec4(normal * .055f, 0);
        item.transform[3] = glm::vec4(glm::vec3(x, y, 0) + normal * radius, 1);
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
Game::Game(const prediction::Course &course) : data(course) {}
void Game::look(float dx, float) {
    // Relative mouse movement steers the player, not a freely flying camera.
    float center =
        player.branch < 0 ? 0.0f : routeCenter(data, player.stage, unsigned(player.branch));
    player.mouseTarget =
        std::clamp(player.mouseTarget + dx * .014f, -15.7f - center, 15.7f - center);
}
void Game::restart(bool) {
    unsigned generation = player.run + 1;
    float best = std::max(player.best, player.elapsed);
    player = State{};
    player.run = generation;
    player.best = best;
    player.title = false;
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
    const float sectionStart = player.stage * SECTION_SECONDS;
    const float previousPhase = (before - sectionStart) / SECTION_SECONDS;
    const float targetSpeed =
        std::abs(input.strafe) > .01f
            ? input.strafe * 12.0f
            : std::clamp((player.mouseTarget - player.localX) * 8.0f, -12.0f, 12.0f);
    player.lateralSpeed += std::clamp(targetSpeed - player.lateralSpeed, -80 * dt, 80 * dt);
    player.localX += player.lateralSpeed * dt;
    if (std::abs(input.strafe) > .01f)
        player.mouseTarget = player.localX;
    player.elapsed += dt;
    player.worldTime = player.elapsed;
    player.speed = INITIAL_SPEED + FORWARD_ACCELERATION * player.elapsed;
    player.feet.y = distanceAt(player.elapsed);
    const float phase = (player.elapsed - sectionStart) / SECTION_SECONDS;
    const float entry = float(prediction::ENTRY_TICK) / prediction::SECTION_TICKS;
    const float merge = float(prediction::MERGE_TICK) / prediction::SECTION_TICKS;
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
        if (std::abs(player.localX) > prediction::TUNNEL_LIMIT * .001f) {
            die("MISSED THE OPENING");
            return;
        }
    }
    if (player.branch >= 0) {
        if (phase < merge && std::abs(player.localX) > prediction::TUNNEL_LIMIT * .001f) {
            die("TUNNEL WALL");
            return;
        }
        const auto &route = prediction::route(data, player.stage, unsigned(player.branch));
        for (unsigned event = 0; event < prediction::OBSTACLES; ++event) {
            float eventPhase = float(prediction::hazardTick(event)) / prediction::SECTION_TICKS;
            // Test the whole visible hazard depth, not just its central plane.
            const float halfWindow = 3.0f / prediction::SECTION_TICKS;
            if (previousPhase <= eventPhase + halfWindow && phase >= eventPhase - halfWindow) {
                unsigned tick = unsigned(player.worldTime * 60);
                if (prediction::blocked(route.obstacles[event],
                                        int(std::round(player.localX * 1000)), tick)) {
                    const char *names[] = {"RED SPIKE", "SLIDING BLOCK", "ROTATING BLADE",
                                           "CRUSHING PASSAGE"};
                    die(names[route.obstacles[event].kind]);
                    return;
                }
            }
        }
        player.feet.x = routeCenter(data, player.stage, unsigned(player.branch)) + player.localX;
    } else {
        player.feet.x = player.localX;
    }
    if (std::abs(player.feet.x) > prediction::SHAFT_LIMIT * .001f) {
        die("OUTER SHAFT WALL");
        return;
    }
    if (phase >= 1) {
        // Change coordinate systems only: preserve world position, target and velocity.
        player.mouseTarget += routeCenter(data, player.stage, unsigned(player.branch));
        player.localX = player.feet.x;
        ++player.stage;
        player.branch = -1;
    }
}
uint32_t Game::decision() const {
    return player.stage + (player.branch >= 0 ? 1u : 0u);
}
bool Game::approachingJunction() const {
    return !player.title && !player.dead;
}
prediction::Request Game::request(uint32_t id, uint32_t rollouts) const {
    prediction::Request r;
    r.id = id;
    r.run = player.run;
    r.junction = decision();
    // A next-fork preview assumes we arrive there at our current world-space x.
    // It is conditional on surviving the current tunnel, not a forecast of our input.
    r.xMm = int(std::round(player.feet.x * 1000));
    r.velocity = std::clamp(int(std::round(player.lateralSpeed * 1000 / 60)), -200, 200);
    r.worldTick = unsigned(player.worldTime * 60);
    r.rollouts = rollouts;
    r.routeCount = prediction::choices(data, r.junction);
    r.seed = 12345u ^ (r.junction * 0x9e3779b9u);
    return r;
}
FrameTransforms Game::transforms(float aspect) const {
    // Camera looks down the shaft. Geometry is rebased around the player each
    // frame so long runs don't lose small-scale precision.
    float roll = std::clamp(-player.lateralSpeed * .008f, -.10f, .10f);
    glm::vec3 eye(player.feet.x, 0, 0), up(std::sin(roll), 0, std::cos(roll));
    FrameTransforms frame;
    frame.view = glm::lookAt(eye, eye + glm::vec3(0, 1, 0), up);
    frame.projection =
        glm::perspective(glm::radians(std::min(103.0f, 84.0f + player.elapsed * .15f)), aspect,
                         .08f, std::max(220.0f, player.speed * 8));
    frame.projection[1][1] *= -1;
    return frame;
}
int Game::score() const {
    return int(player.elapsed * 100);
}

void Game::scene(std::vector<SceneInstance> &scene, const Guidance &guidance) const {
    scene.clear();
    const unsigned first = player.stage ? player.stage - 1 : 0;
    const unsigned target = decision();
    const float origin = player.feet.y;
    for (unsigned stage = first; stage <= player.stage + 2; ++stage) {
        float start = stage * SECTION_SECONDS;
        float entry = float(prediction::ENTRY_TICK) / prediction::TICKS_PER_SECOND;
        float y0 = distanceAt(start) - origin, yEntry = distanceAt(start + entry) - origin;
        float yMerge = distanceAt(start + float(prediction::MERGE_TICK) / 60) - origin,
              yEnd = distanceAt(start + SECTION_SECONDS) - origin;
        auto count = prediction::choices(data, stage);
        tube(scene, 0, 0, y0, yEntry, shaftApothem, {.027f, .03f, .055f});
        tube(scene, 0, 0, yMerge, yEnd, shaftApothem, {.025f, .028f, .047f});
        ring(scene, 0, y0, shaftApothem, .09f, {.04f, .09f, .18f});
        // A dark bulkhead with 2..4 octagonal mouths, not a HUD route selector.
        object(scene, {0, yEntry, 10.0f}, {32, .45f, 12.8f}, {.018f, .02f, .032f});
        object(scene, {0, yEntry, -10.0f}, {32, .45f, 12.8f}, {.018f, .02f, .032f});
        float outer = float(count - 1) * TUNNEL_HALF_PITCH * .001f + tunnelRadius;
        for (int side : {-1, 1})
            object(scene, {side * (outer + (shaftRadius - outer) * .5f), yEntry, 0},
                   {std::max(.01f, shaftRadius - outer), .45f, 7.2f}, {.018f, .02f, .032f});
        for (unsigned divider = 0; divider + 1 < count; ++divider) {
            float x = (prediction::laneCenter(count, divider) +
                       prediction::laneCenter(count, divider + 1)) *
                      .0005f;
            object(scene, {x, yEntry, 0}, {1.0f, .5f, 7.2f}, {.025f, .02f, .04f});
        }
        for (unsigned choice = 0; choice < count; ++choice) {
            float x = prediction::laneCenter(count, choice) * .001f;
            auto rim = glm::vec3(.08f, .18f, .35f);
            if (stage == target && guidance.enabled && guidance.valid) {
                float probability = guidance.percentages[choice];
                rim = probability < .1f   ? red
                      : probability < .6f ? glm::vec3(.9f, .32f, .06f)
                                          : cyan;
                if (guidance.recommended == int(choice))
                    rim = glm::vec3(.15f, .95f, .55f);
            }
            tube(scene, x, x, yEntry, yMerge, tunnelApothem, {.022f, .026f, .046f});
            ring(scene, x, yEntry - .25f, tunnelApothem, .12f, rim);
            for (int band = 1; band < 21; ++band) {
                float t = float(band) / 21;
                ring(scene, x, yEntry + (yMerge - yEntry) * t, tunnelApothem, .055f,
                     {.035f, .06f, .12f});
            }
            const auto &route = prediction::route(data, stage, choice);
            for (unsigned event = 0; event < prediction::OBSTACLES; ++event) {
                const auto &o = route.obstacles[event];
                const auto pose = prediction::pose(o, unsigned(player.worldTime * 60));
                float y = distanceAt(start + float(prediction::hazardTick(event)) / 60) - origin;
                float eventTime = start + float(prediction::hazardTick(event)) / 60;
                float depth = distanceAt(eventTime + .05f) - distanceAt(eventTime - .05f);
                float cx = x + pose.center * .001f;
                if (o.kind == 0) {
                    SceneInstance spike;
                    spike.transform =
                        glm::translate(glm::mat4(1), {cx, y, (o.flags & 1u) ? 1.1f : -1.1f});
                    if (o.flags & 1u)
                        spike.transform =
                            glm::rotate(spike.transform, glm::radians(180.0f), glm::vec3(0, 1, 0));
                    spike.transform = glm::scale(spike.transform, {o.extent * .002f, depth, 3});
                    spike.tint = glm::vec4(red, 2);
                    scene.push_back(spike);
                } else if (o.kind == 1)
                    object(scene, {cx, y, 0}, {o.extent * .002f, depth, 7.0f}, {.95f, .2f, .055f});
                else if (o.kind == 2) {
                    SceneInstance blade;
                    float c = pose.cosine / 1024.0f, s = pose.sine / 1024.0f;
                    blade.transform[0] =
                        glm::vec4(c * o.extent * .002f, 0, s * o.extent * .002f, 0);
                    blade.transform[1] = glm::vec4(0, depth, 0, 0);
                    blade.transform[2] = glm::vec4(-s * .42f, 0, c * .42f, 0);
                    blade.transform[3] = glm::vec4(cx, y, 0, 1);
                    blade.tint = glm::vec4(red, 1);
                    scene.push_back(blade);
                    object(scene, {cx, y - depth * .5f - .02f, 0}, {.16f, .05f, .16f},
                           {1, .5f, .1f}, 3);
                } else {
                    float gap = pose.aperture * .001f;
                    for (int side : {-1, 1}) {
                        // Extend each moving edge to its own wall, including center offset.
                        float width =
                            std::max(.01f, tunnelRadius - side * pose.center * .001f - gap);
                        object(scene, {cx + side * (gap + width * .5f), y, 0}, {width, depth, 7.0f},
                               {.55f, .035f, .2f});
                        object(scene, {cx + side * gap, y - depth * .5f - .02f, 0},
                               {.07f, .04f, 7.0f}, red, 3);
                    }
                }
            }
            if (stage == target && guidance.enabled && !player.title) {
                std::string label =
                    guidance.valid
                        ? std::to_string(int(std::round(guidance.percentages[choice] * 100))) + "%"
                        : "...";
                worldText(scene, {x, yEntry - .8f, 4.3f}, label, .17f,
                          guidance.valid ? rim : glm::vec3(.15f, .25f, .4f));
            }
        }
    }
    // Sparse streaks emphasize acceleration without particle buffers or effects.
    for (int index = 0; index < 20; ++index) {
        float angle = index * 2.399963f;
        float y = std::fmod(index * 13.7f - player.feet.y, 95.0f);
        if (y < 0)
            y += 95;
        object(scene, {std::cos(angle) * 14.7f, y, std::sin(angle) * 14.7f},
               {.025f, std::min(3.5f, player.speed * .07f), .025f}, {.045f, .14f, .2f}, 3);
    }
    // No gameplay HUD, score panel, crosshair or screen-space probabilities.
    if (player.title || player.dead || guidance.paused) {
        rectangle(scene, 245, 190, 790, 340, {.008f, .012f, .023f});
        rectangle(scene, 245, 190, 790, 3, player.dead ? red : cyan);
        screenText(scene, 300, 225, player.dead ? "SHATTERED" : "VOID / DIVE", 7,
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
            screenText(scene, 300, 315, "A/D OR MOUSE: STEER", 3, cyan);
            screenText(scene, 300, 355, "NO BRAKES. PICK A TUNNEL.", 3, white);
            screenText(scene, 300, 400, "NUMBERS ARE MODEL ESTIMATES", 2, violet);
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
    if (run != p.run)
        run = p.run;
    int target = 0;
    if (p.branch < 0) {
        auto count = prediction::choices(game.course(), p.stage);
        // Choose the best modeled option once per section without peeking at any
        // game RNG. Prefer the guaranteed-clear center line for the test driver.
        for (unsigned c = 0; c < count; ++c) {
            const auto &route = prediction::route(game.course(), p.stage, c);
            if (route.obstacles[0].kind == 0 && route.obstacles[0].offset == -2400 &&
                route.obstacles[1].kind == 0 && route.obstacles[1].offset == 2400) {
                target = prediction::laneCenter(count, c);
                break;
            }
        }
    }
    float desired = std::clamp((target * .001f - p.localX) * 8.0f, -12.0f, 12.0f);
    controls.strafe = desired / 12;
    // When centered, avoid the mouse controller retaining a prior extreme target.
    if (std::abs(controls.strafe) < .011f)
        controls.strafe = 0;
    return controls;
}
void selfTest() {
    auto course = prediction::makeCourse();
    bool counts[5] = {};
    bool kinds[4] = {};
    for (unsigned stage = 0; stage < 32; ++stage) {
        counts[prediction::choices(course, stage)] = true;
        for (unsigned c = 0; c < prediction::choices(course, stage); ++c)
            for (const auto &o : prediction::route(course, stage, c).obstacles)
                kinds[o.kind] = true;
    }
    for (unsigned count = 2; count <= 4; ++count)
        if (!counts[count])
            throw std::runtime_error("missing fork count");
    for (bool present : kinds)
        if (!present)
            throw std::runtime_error("missing obstacle kind");
    Game game(course);
    Autopilot bot;
    unsigned boundaries = 0;
    for (int tick = 0; tick < 120 * 260 && game.state().stage < 20; ++tick) {
        float previousX = game.state().feet.x;
        unsigned previousStage = game.state().stage;
        bool wasTitle = game.state().title;
        game.step(bot.sample(game), 1.0f / 120);
        if (game.state().dead)
            throw std::runtime_error("test driver died: " + game.state().deathReason);
        if (!wasTitle && !game.state().dead &&
            std::abs(game.state().feet.x - previousX - game.state().lateralSpeed / 120) > .0001f)
            throw std::runtime_error("automatic lateral movement/recentering detected");
        if (game.state().stage != previousStage)
            ++boundaries;
    }
    if (game.state().stage < 20 || game.state().dead || game.state().deaths)
        throw std::runtime_error("viable tunnel sequence cannot be steered");
    if (game.state().speed <= INITIAL_SPEED + 20)
        throw std::runtime_error("forward speed not increasing");
    if (boundaries != 20)
        throw std::runtime_error("missing section-boundary coverage");
    std::cout << "PASS 20 twelve-second forks, acceleration and no automatic lateral movement\n";
    std::cout << "PASS seeded 2/3/4-way forks and all four obstacle kinds\n";
    Game noBrake(course);
    noBrake.restart();
    GameControls backwards;
    backwards.forward = -1;
    for (int tick = 0; tick < 60; ++tick)
        noBrake.step(backwards, 1.0f / 120);
    if (noBrake.state().feet.y < 8)
        throw std::runtime_error("backward input brakes falling");
    for (int tick = 0; tick < 360; ++tick)
        noBrake.step(backwards, 1.0f / 120);
    if (!noBrake.state().dead)
        throw std::runtime_error("missing a fork is not lethal");
    prediction::Obstacle sealed{3, 0, 7, 0, 0, 90, 0, 60};
    for (int x = -prediction::TUNNEL_LIMIT; x <= prediction::TUNNEL_LIMIT; x += 100)
        for (unsigned tick = 0; tick < 128; tick += 8)
            if (!prediction::blocked(sealed, x, tick))
                throw std::runtime_error("sealed path not lethal");
    prediction::Request r;
    r.routeCount = prediction::choices(course, 0);
    auto a = prediction::evaluate(course, r), b = prediction::evaluate(course, r);
    if (!prediction::sameResults(a, b))
        throw std::runtime_error("rollout nondeterministic");
    int impossible = 0;
    for (unsigned stage = 0; stage < 16; ++stage) {
        r.junction = stage;
        r.worldTick = stage * prediction::SECTION_TICKS;
        r.routeCount = prediction::choices(course, stage);
        auto result = prediction::evaluate(course, r);
        bool anyViable = false;
        for (unsigned c = 0; c < r.routeCount; ++c) {
            if (prediction::survival(result, c) > .2f)
                anyViable = true;
            if (result.routes[c].survived == 0)
                ++impossible;
        }
        if (!anyViable)
            throw std::runtime_error("predictor finds no viable opening");
    }
    if (!impossible)
        throw std::runtime_error("no genuinely impossible choices");
    std::cout << "PASS geometric rollout survival, impossible choices and deterministic streams\n";
    r.run = 4;
    r.junction = 0;
    r.worldTick = 10;
    if (!prediction::applies(r, 4, 0, 20) || prediction::applies(r, 5, 0, 20) ||
        prediction::applies(r, 4, 1, 20) || prediction::applies(r, 4, 0, 45))
        throw std::runtime_error("stale result accepted");
    std::cout << "PASS stale run/junction/age rejection\n";
}
} // namespace survival
