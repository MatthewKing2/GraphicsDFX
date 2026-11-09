#pragma once
#include "game_input.hpp"
#include "prediction.hpp"
#include "renderer.hpp"
#include <string>

namespace survival {
constexpr float STAGE_LENGTH = 114.0f, ROUTE_START = 32.0f;
struct Box {
    glm::vec3 center, half, color;
    bool solid = false, floor = false;
};
struct State {
    glm::vec3 feet{0, 3, 0};
    float verticalSpeed = 0, yaw = 0, pitch = 0;
    float elapsed = 0, worldTime = 0, deadline = 22, respawnTime = 0;
    bool grounded = true, won = false;
    unsigned stage = 0, deaths = 0, run = 1;
    int branch = -1;
    std::string deathReason;
};
struct Hud {
    bool gpu = true, guidance = true, stats = false, paused = false, pending = false, valid = false;
    int recommended = 0;
    float percentages[2] = {0, 0};
    double predictionMs = 0, kernelMs = 0, frameMs = 0, p95Ms = 0;
    uint32_t rollouts = 512, horizon = 240;
};
Mesh cubeMesh();
class Game {
  public:
    explicit Game(const prediction::Course &);
    void look(float dx, float dy);
    void step(const GameControls &, float dt);
    void restart(bool fromBeginning = true);
    FrameTransforms transforms(float aspect) const;
    const State &state() const {
        return player;
    }
    const std::vector<Box> &geometry() const {
        return boxes;
    }
    const prediction::Course &course() const {
        return data;
    }
    bool approachingJunction() const;
    prediction::Request request(uint32_t id, uint32_t rollouts) const;
    void scene(std::vector<SceneInstance> &, const Hud &) const;
    int score() const;

  private:
    prediction::Course data;
    State player;
    std::vector<Box> boxes;
    float coyote = 0.1f, jumpBuffer = 0;
    void buildCourse();
    void die(const char *);
    bool overlaps(const Box &, glm::vec3 feet) const;
};
// Deterministic test/demo driver; ordinary interactive play never invokes it.
class Autopilot {
  public:
    explicit Autopilot(unsigned routePattern = 7) : pattern(routePattern) {}
    GameControls sample(const Game &);

  private:
    unsigned waypoint = 0, run = 0, stage = prediction::STAGES, pattern;
};
void selfTest();
} // namespace survival
