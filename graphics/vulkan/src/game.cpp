#include "game.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>

namespace survival {
namespace {
const glm::vec3 orange(1.0f, 0.37f, 0.08f), blue(0.12f, 0.62f, 1.0f), mint(0.15f, 1.0f, 0.65f);
void object(std::vector<SceneInstance> &scene, glm::vec3 center, glm::vec3 size, glm::vec3 color) {
    SceneInstance item;
    item.transform = glm::scale(glm::translate(glm::mat4(1), center), size);
    item.tint = glm::vec4(color, 1);
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
void text(std::vector<SceneInstance> &scene, float x, float y, const std::string &value,
          float scale, glm::vec3 color) {
    for (char c : value) {
        const auto rows = glyph(c);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col) {
                if (rows[row] & (1 << (4 - col)))
                    rectangle(scene, x + col * scale, y + row * scale, scale, scale, color);
            }
        x += 6 * scale;
    }
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
Game::Game(const prediction::Course &course) : data(course) {
    buildCourse();
}
void Game::buildCourse() {
    auto add = [&](glm::vec3 c, glm::vec3 size, glm::vec3 color, bool solid = false,
                   bool floor = false) { boxes.push_back({c, size * .5f, color, solid, floor}); };
    auto floor = [&](float x, float width, float begin, float end, glm::vec3 color) {
        if (end > begin)
            add({x, (begin + end) * .5f, -.25f}, {width, end - begin, .5f}, color, false, true);
    };
    for (unsigned stage = 0; stage < prediction::STAGES; ++stage) {
        const float base = stage * STAGE_LENGTH;
        floor(0, 8, base - (stage == 0 ? 5 : 0), base + 24, {.09f, .13f, .2f});
        floor(0, 24, base + 24, base + 40, {.14f, .19f, .26f});
        floor(0, 24, base + 96, base + 110, {.12f, .2f, .23f});
        floor(0, 8, base + 110, base + 114, {.09f, .2f, .18f});
        for (int side : {-1, 1}) {
            add({side * 4.5f, base + 10, 1.3f}, {1, 28, 2.6f}, {.16f, .2f, .28f}, true);
            add({side * 12.5f, base + 67, 1.3f}, {1, 86, 2.6f}, {.12f, .17f, .23f}, true);
        }
        add({0, base + 68, 1.4f}, {11, 56, 2.8f}, {.11f, .14f, .2f}, true);
        for (unsigned branch = 0; branch < 2; ++branch) {
            float x = branch ? 8.0f : -8.0f;
            auto color = branch ? blue : orange;
            const auto &route = data.routes[stage * 2 + branch];
            float gapStart = base + ROUTE_START + route.gapStart * .001f;
            float gapEnd = base + ROUTE_START + route.gapEnd * .001f;
            floor(x, 5, base + 40, gapStart, color * .3f);
            floor(x, 5, gapEnd, base + 96, color * .3f);
            for (float y = base + 42; y < base + 96; y += 7) {
                add({x - 2.35f, y, .025f}, {.12f, 1.8f, .05f}, color);
                add({x + 2.35f, y, .025f}, {.12f, 1.8f, .05f}, color);
            }
            for (float y : {gapStart - .15f, gapEnd + .15f})
                add({x, y, .04f}, {5, .22f, .08f}, orange);
            if (route.hurdleMm) {
                float y = base + ROUTE_START + route.hurdleMm * .001f;
                add({x - 2.5f, y, .5f}, {.18f, .3f, 1}, orange);
                add({x + 2.5f, y, .5f}, {.18f, .3f, 1}, orange);
            }
            float gateY = base + ROUTE_START + route.gateMm * .001f;
            for (int side : {-1, 1})
                add({x + side * 2.4f, gateY, 1.6f}, {.2f, .4f, 3.2f}, color * .6f);
            // Physical route sign: arrow stem and two diagonal arms.
            add({x, base + 35, .08f}, {.22f, 3, .15f}, color);
            for (int side : {-1, 1}) {
                add({x + side * .5f, base + 35.8f, .08f}, {1.2f, .25f, .15f}, color);
            }
        }
        add({0, base + 111, .035f}, {8, .35f, .07f}, mint);
        for (int side : {-1, 1})
            add({side * 4.3f, base + 111, 2}, {.3f, .4f, 4}, mint * .7f);
        add({0, base + 111, 4}, {9, .4f, .3f}, mint);
    }
    add({0, -4, 1.5f}, {9, .6f, 3}, {.15f, .2f, .3f}, true);
}
void Game::look(float dx, float dy) {
    player.yaw = std::remainder(player.yaw + dx * .10f, 360.0f);
    player.pitch = std::clamp(player.pitch - dy * .10f, -82.0f, 82.0f);
}
bool Game::overlaps(const Box &box, glm::vec3 feet) const {
    return std::abs(feet.x - box.center.x) < box.half.x + .28f &&
           std::abs(feet.y - box.center.y) < box.half.y + .28f &&
           feet.z < box.center.z + box.half.z && feet.z + 1.65f > box.center.z - box.half.z;
}
void Game::die(const char *why) {
    if (player.respawnTime > 0 || player.won)
        return;
    player.deathReason = why;
    player.respawnTime = .9f;
    ++player.deaths;
}
void Game::restart(bool fromBeginning) {
    const unsigned nextRun = player.run + 1;
    if (fromBeginning)
        player = State{};
    else {
        player.feet = {0, player.stage * STAGE_LENGTH + 3, 0};
        player.verticalSpeed = 0;
        player.yaw = 0;
        player.pitch = 0;
        player.deadline = 22;
        player.respawnTime = 0;
        player.grounded = true;
        player.branch = -1;
    }
    player.run = nextRun;
    coyote = .1f;
    jumpBuffer = 0;
}
void Game::step(const GameControls &input, float dt) {
    if (input.restart) {
        restart();
        return;
    }
    if (player.won)
        return;
    if (player.respawnTime > 0) {
        player.respawnTime -= dt;
        if (player.respawnTime <= 0)
            restart(false);
        return;
    }
    player.elapsed += dt;
    player.worldTime += dt;
    player.deadline -= dt;
    if (player.deadline <= 0) {
        die("PURSUIT CAUGHT YOU");
        return;
    }
    if (input.jump)
        jumpBuffer = .12f;
    else
        jumpBuffer = std::max(0.0f, jumpBuffer - dt);
    coyote = player.grounded ? .10f : std::max(0.0f, coyote - dt);
    if (jumpBuffer > 0 && coyote > 0) {
        player.verticalSpeed = 6;
        player.grounded = false;
        coyote = 0;
        jumpBuffer = 0;
    }
    const float yaw = glm::radians(player.yaw);
    glm::vec2 direction = glm::vec2(std::sin(yaw), std::cos(yaw)) * input.forward +
                          glm::vec2(std::cos(yaw), -std::sin(yaw)) * input.strafe;
    const float length = glm::length(direction);
    if (length > 1)
        direction /= length;
    float speed = 8;
    if (player.branch >= 0)
        speed = data.routes[player.stage * 2 + player.branch].speedMmPerTick * .03f;
    for (int axis = 0; axis < 2; ++axis) {
        const auto before = player.feet;
        player.feet[axis] += direction[axis] * speed * dt;
        for (const auto &box : boxes)
            if (box.solid && overlaps(box, player.feet)) {
                player.feet = before;
                break;
            }
    }
    float previousZ = player.feet.z;
    player.verticalSpeed -= 18 * dt;
    player.feet.z += player.verticalSpeed * dt;
    player.grounded = false;
    if (player.verticalSpeed <= 0)
        for (const auto &box : boxes) {
            if (!box.floor)
                continue;
            float top = box.center.z + box.half.z;
            if (previousZ >= top - .001f && player.feet.z <= top &&
                std::abs(player.feet.x - box.center.x) < box.half.x + .16f &&
                std::abs(player.feet.y - box.center.y) < box.half.y + .16f) {
                player.feet.z = top;
                player.verticalSpeed = 0;
                player.grounded = true;
                break;
            }
        }
    const float base = player.stage * STAGE_LENGTH;
    if (player.feet.y >= base + 40 && player.feet.y < base + 96 && std::abs(player.feet.x) > 5.5f)
        player.branch = player.feet.x > 0 ? 1 : 0;
    const uint32_t tick = uint32_t(player.worldTime * 30);
    for (unsigned branch = 0; branch < 2; ++branch) {
        const auto &route = data.routes[player.stage * 2 + branch];
        const float x = branch ? 8.0f : -8.0f;
        if (route.hurdleMm && overlaps({{x, base + ROUTE_START + route.hurdleMm * .001f, .225f},
                                        {2.5f, .2f, .225f},
                                        {},
                                        false,
                                        false},
                                       player.feet)) {
            die("JUMP OVER THE ORANGE BEAM");
            return;
        }
        if (prediction::gateActive(route, tick) &&
            overlaps({{x, base + ROUTE_START + route.gateMm * .001f, 1.5f},
                      {2.5f, .25f, 1.5f},
                      {},
                      false,
                      false},
                     player.feet)) {
            die("RED GATE - WAIT FOR GREEN");
            return;
        }
    }
    if (player.feet.z < -3) {
        die("MISSED THE GAP - SPACE TO JUMP");
        return;
    }
    if (player.feet.y >= base + STAGE_LENGTH && player.stage + 1 < prediction::STAGES) {
        ++player.stage;
        player.deadline = 22;
        player.branch = -1;
    }
    if (player.feet.y >= STAGE_LENGTH * prediction::STAGES - 2 && std::abs(player.feet.x) < 4.3f)
        player.won = true;
}
FrameTransforms Game::transforms(float aspect) const {
    const float yaw = glm::radians(player.yaw), pitch = glm::radians(player.pitch);
    glm::vec3 forward(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch),
                      std::sin(pitch));
    const auto eye = player.feet + glm::vec3(0, 0, 1.55f);
    FrameTransforms frame;
    frame.view = glm::lookAt(eye, eye + forward, glm::vec3(0, 0, 1));
    frame.projection = glm::perspective(glm::radians(76.0f), aspect, .08f, 160.0f);
    frame.projection[1][1] *= -1;
    return frame;
}
bool Game::approachingJunction() const {
    float y = player.feet.y - player.stage * STAGE_LENGTH;
    return y >= 6 && y < 40 && player.branch < 0 && !player.won && player.respawnTime <= 0;
}
prediction::Request Game::request(uint32_t id, uint32_t rollouts) const {
    prediction::Request r;
    r.id = id;
    r.run = player.run;
    r.junction = player.stage;
    r.progressMm = int32_t((player.feet.y - player.stage * STAGE_LENGTH - ROUTE_START) * 1000);
    r.budgetTicks = uint32_t(std::max(0.0f, player.deadline) * 30);
    r.worldTick = uint32_t(player.worldTime * 30);
    r.rollouts = rollouts;
    // Keep seed fixed across refreshes so small timing changes don't add display noise.
    r.seed = 12345u ^ (player.stage * 0x9e3779b9u);
    return r;
}
int Game::score() const {
    return int(player.feet.y * 10) + int(player.stage) * 1000 -
           (player.won ? int(player.elapsed * 5) : 0);
}

void Game::scene(std::vector<SceneInstance> &scene, const Hud &hud) const {
    scene.clear();
    for (const auto &box : boxes)
        object(scene, box.center, box.half * 2.0f, box.color);
    const uint32_t tick = uint32_t(player.worldTime * 30);
    for (unsigned stage = 0; stage < prediction::STAGES; ++stage)
        for (unsigned branch = 0; branch < 2; ++branch) {
            const auto &route = data.routes[stage * 2 + branch];
            float x = branch ? 8.0f : -8.0f, base = stage * STAGE_LENGTH + ROUTE_START;
            if (route.hurdleMm)
                object(scene, {x, base + route.hurdleMm * .001f, .225f}, {5, .35f, .45f}, orange);
            const bool active = prediction::gateActive(route, tick),
                       warning = prediction::gateWarning(route, tick);
            const auto color = active    ? glm::vec3(1, .12f, .17f)
                               : warning ? glm::vec3(1, .7f, .07f)
                                         : mint;
            object(scene, {x, base + route.gateMm * .001f, active ? 1.5f : 2.85f},
                   {5, .35f, active ? 3.0f : .18f}, color);
            // Floating route beacons above the fork, updated only for current guidance.
            auto beacon = branch ? blue : orange;
            if (stage == player.stage && hud.guidance && hud.valid)
                beacon = hud.recommended == int(branch) ? mint : glm::vec3(.65f, .16f, .2f);
            object(scene, {x, stage * STAGE_LENGTH + 38, 3.3f}, {3, .3f, .25f}, beacon);
        }
    float wallY = std::max(player.stage * STAGE_LENGTH - 4, player.feet.y - player.deadline * 1.2f);
    object(scene, {0, wallY, 2.5f}, {25, .3f, 6}, {.7f, .05f, .3f});

    const glm::vec3 white(.9f, .95f, 1), muted(.55f, .66f, .75f), panel(.025f, .045f, .075f);
    rectangle(scene, 16, 16, 1248, 74, panel);
    text(scene, 32, 28, "DEADLINE / RUNNER", 3, white);
    text(scene, 32, 62,
         "SECTOR " + std::to_string(player.stage + 1) + "/3  SCORE " + std::to_string(score()), 2,
         muted);
    text(scene, 780, 28,
         "PURSUIT " + std::to_string(std::max(0, int(std::ceil(player.deadline)))) + "S", 3,
         player.deadline < 6 ? orange : mint);
    rectangle(scene, 780, 61, 450, 10, {.13f, .17f, .22f});
    rectangle(scene, 780, 61, 450 * std::clamp(player.deadline / 22, 0.0f, 1.0f), 10,
              player.deadline < 6 ? orange : mint);
    rectangle(scene, 16, 638, 1248, 66, panel);
    text(scene, 32, 650, "WASD MOVE  MOUSE LOOK  SPACE JUMP  R RESTART  ESC PAUSE", 2, white);
    text(scene, 32, 677,
         std::string("F1 ") + (hud.gpu ? "GPU" : "CPU") + "  F2 GUIDANCE " +
             (hud.guidance ? "ON" : "OFF") + "  F3 STATS",
         2, mint);
    rectangle(scene, 637, 355, 6, 2, white);
    rectangle(scene, 639, 353, 2, 6, white);
    if (approachingJunction() && hud.guidance) {
        for (int branch = 0; branch < 2; ++branch) {
            float x = branch ? 846 : 24;
            auto color = branch ? blue : orange;
            rectangle(scene, x, 126, 410, 114, panel);
            if (hud.valid && hud.recommended == branch)
                rectangle(scene, x, 126, 410, 5, mint);
            text(scene, x + 14, 143, branch ? "RIGHT / BLUE" : "LEFT / ORANGE", 3, color);
            std::string label =
                hud.valid
                    ? std::to_string(int(std::round(hud.percentages[branch] * 100))) + "% SURVIVE"
                    : "SCANNING...";
            text(scene, x + 14, 179, label, 3, hud.valid ? white : muted);
            text(scene, x + 14, 214, branch ? "SLOWER / SMALLER GAP" : "FASTER / HARDER JUMPS", 2,
                 muted);
        }
        text(scene, 430, 262, "MODEL ESTIMATE / NEXT 8S", 2, muted);
    }
    if (hud.stats) {
        rectangle(scene, 20, 370, 530, 118, panel);
        text(scene, 32, 382,
             "FRAME " + std::to_string(int(hud.frameMs)) + "MS P95 " +
                 std::to_string(int(hud.p95Ms)) + "MS",
             2, white);
        text(scene, 32, 408,
             "PRED " + std::to_string(int(hud.predictionMs * 1000)) + "US GPU " +
                 std::to_string(int(hud.kernelMs * 1000)) + "US",
             2, mint);
        text(scene, 32, 434, "ROLLOUTS " + std::to_string(hud.rollouts) + " HORIZON 240 TICKS", 2,
             muted);
        text(scene, 32, 460, "WIRE: COURSE 288B REQUEST 48B RESULT 32B", 2, muted);
    }
    if (player.stage > 0 && player.feet.y - player.stage * STAGE_LENGTH < 12 &&
        player.respawnTime <= 0)
        text(scene, 460, 300, "CHECKPOINT SAVED", 3, mint);
    if (player.elapsed < 4 && player.respawnTime <= 0) {
        rectangle(scene, 300, 340, 680, 188, panel);
        text(scene, 325, 359, "KEEP MOVING. CHOOSE A ROUTE.", 3, white);
        text(scene, 325, 399, "JUMP GAPS AND ORANGE BEAMS", 3, orange);
        text(scene, 325, 439, "RED GATE KILLS. GREEN IS SAFE.", 3, mint);
        text(scene, 325, 486, "REACH EACH CHECKPOINT BEFORE PURSUIT", 2, muted);
    }
    if (player.respawnTime > 0 || player.won || hud.paused) {
        rectangle(scene, 200, 302, 880, 205, panel);
        const std::string title = player.won   ? "YOU SURVIVED"
                                  : hud.paused ? "PAUSED"
                                               : "RUN INTERRUPTED";
        text(scene, 250, 328, title, 5, player.won ? mint : white);
        text(scene, 250, 390,
             player.won   ? "SCORE " + std::to_string(score()) + "  TIME " +
                                std::to_string(int(player.elapsed)) + "S"
             : hud.paused ? "ESC RESUME / CLOSE WINDOW TO QUIT"
                          : player.deathReason,
             3, orange);
        text(scene, 250, 450,
             player.won   ? "R TO RUN AGAIN"
             : hud.paused ? "F1 BACKEND / F2 GUIDANCE / F3 STATS"
                          : "RESTARTING FROM CHECKPOINT...",
             2, muted);
    }
}

GameControls Autopilot::sample(const Game &game) {
    const auto &p = game.state();
    if (run != p.run || stage != p.stage) {
        waypoint = 0;
        run = p.run;
        stage = p.stage;
    }
    GameControls controls;
    if (p.respawnTime > 0 || p.won)
        return controls;
    const unsigned choice = (pattern >> p.stage) & 1u;
    const float lane = choice ? 8.0f : -8.0f;
    const glm::vec2 targets[] = {{0, 26}, {lane, 35}, {lane, 99}, {0, 109}, {0, 114.4f}};
    glm::vec2 target = targets[std::min(waypoint, 4u)] + glm::vec2(0, p.stage * STAGE_LENGTH);
    glm::vec2 delta = target - glm::vec2(p.feet);
    if (glm::length(delta) < .45f && waypoint < 4) {
        ++waypoint;
        target = targets[waypoint] + glm::vec2(0, p.stage * STAGE_LENGTH);
        delta = target - glm::vec2(p.feet);
    }
    if (glm::length(delta) > .01f) {
        delta = glm::normalize(delta);
        const float yaw = glm::radians(p.yaw);
        controls.strafe = glm::dot(delta, glm::vec2(std::cos(yaw), -std::sin(yaw)));
        controls.forward = glm::dot(delta, glm::vec2(std::sin(yaw), std::cos(yaw)));
    }
    const auto &route = game.course().routes[p.stage * 2 + choice];
    float progress = p.feet.y - p.stage * STAGE_LENGTH - ROUTE_START;
    float gapDistance = route.gapStart * .001f - progress;
    if (gapDistance > 0 && gapDistance < 1.6f && p.grounded)
        controls.jump = true;
    const float beamDistance = route.hurdleMm * .001f - progress;
    if (route.hurdleMm && beamDistance > 0 && beamDistance < 1.6f && p.grounded)
        controls.jump = true;
    float gateDistance = route.gateMm * .001f - progress;
    uint32_t tick = uint32_t(p.worldTime * 30);
    if (gateDistance > 0 && gateDistance < 2.2f &&
        (prediction::gateActive(route, tick) || prediction::gateWarning(route, tick)))
        controls.forward = controls.strafe = 0;
    return controls;
}

void selfTest() {
    auto course = prediction::makeCourse();
    Game game(course);
    Autopilot bot;
    for (int i = 0; i < 120 * 80 && !game.state().won; ++i)
        game.step(bot.sample(game), 1.0f / 120);
    if (!game.state().won || game.state().deaths)
        throw std::runtime_error("autopilot cannot finish course without dying");
    std::cout << "PASS playable course: 3 junctions, 2 checkpoints, " << game.state().elapsed
              << " simulated seconds\n";
    for (unsigned pattern = 0; pattern < 8; ++pattern) {
        Game test(course);
        Autopilot routes(pattern);
        for (int tick = 0; tick < 120 * 80 && !test.state().won; ++tick)
            test.step(routes.sample(test), 1.0f / 120);
        if (!test.state().won || test.state().deaths)
            throw std::runtime_error("untraversable route combination " + std::to_string(pattern));
    }
    std::cout << "PASS all eight route combinations without deaths\n";
    game.restart();
    for (int i = 0; i < 120 * 23; ++i)
        game.step({}, 1.0f / 120);
    if (game.state().deaths != 1)
        throw std::runtime_error("pursuit/restart failed");
    std::cout << "PASS pursuit, death and checkpoint restart\n";
    Game pitchA(course), pitchB(course);
    pitchB.look(0, -700);
    GameControls forward;
    forward.forward = 1;
    for (int i = 0; i < 120; ++i) {
        pitchA.step(forward, 1.0f / 120);
        pitchB.step(forward, 1.0f / 120);
    }
    if (glm::length(pitchA.state().feet - pitchB.state().feet) > .001f)
        throw std::runtime_error("camera pitch changes movement");
    std::cout << "PASS horizontal movement independent of mouse pitch\n";
    prediction::Request request;
    request.progressMm = -10000;
    const auto first = prediction::evaluate(course, request),
               second = prediction::evaluate(course, request);
    if (!prediction::sameResults(first, second))
        throw std::runtime_error("prediction nondeterministic");
    request.budgetTicks = 0;
    auto doomed = prediction::evaluate(course, request);
    if (doomed.routes[0].survived || doomed.routes[1].survived)
        throw std::runtime_error("expired pursuit survives");
    auto safe = course;
    for (auto &route : safe.routes) {
        route.gapStart = route.gapEnd = route.hurdleMm = route.gateMm = 0;
        route.jumpFailure = 0;
    }
    request.budgetTicks = 660;
    auto clear = prediction::evaluate(safe, request);
    if (clear.routes[0].survived != request.rollouts ||
        clear.routes[1].survived != request.rollouts)
        throw std::runtime_error("clear route not safe");
    std::cout << "PASS deterministic rollouts, expired deadline and clear routes\n";
    int leftWins = 0, rightWins = 0;
    for (uint32_t tick = 0; tick < 900; tick += 15) {
        prediction::Request decision;
        decision.progressMm = 7000;
        decision.worldTick = tick;
        auto result = prediction::evaluate(course, decision);
        if (prediction::survival(result, 0) > prediction::survival(result, 1) + .05f)
            ++leftWins;
        if (prediction::survival(result, 1) > prediction::survival(result, 0) + .05f)
            ++rightWins;
    }
    if (!leftWins || !rightWins)
        throw std::runtime_error("hazard timing never changes a meaningful route recommendation");
    std::cout << "PASS hazard-dependent recommendations: left favored " << leftWins
              << ", right favored " << rightWins << " sampled states\n";
    prediction::Request freshness;
    freshness.run = 3;
    freshness.junction = 1;
    freshness.worldTick = 100;
    if (!prediction::applies(freshness, 3, 1, 110) || prediction::applies(freshness, 4, 1, 110) ||
        prediction::applies(freshness, 3, 2, 110) || prediction::applies(freshness, 3, 1, 140) ||
        prediction::applies(freshness, 3, 1, 90))
        throw std::runtime_error("stale guidance accepted");
    freshness.courseVersion++;
    if (prediction::applies(freshness, 3, 1, 110))
        throw std::runtime_error("old course result accepted");
    std::cout << "PASS result freshness: run, junction, course, age and future-tick rejection\n";
}
} // namespace survival
