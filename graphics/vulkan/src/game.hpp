#pragma once
#include "game_input.hpp"
#include "prediction.hpp"
#include "renderer.hpp"
#include <string>
namespace survival {
constexpr float SECTION_SECONDS = 2.4f, INITIAL_SPEED = 18.0f, FORWARD_ACCELERATION = 1.2f;
struct State {
    glm::vec3 feet{0, 0, 0};
    float localX = 0, lateralSpeed = 0, mouseTarget = 0;
    float elapsed = 0, worldTime = 0, speed = INITIAL_SPEED, best = 0;
    bool title = true, dead = false;
    unsigned stage = 0, run = 1, deaths = 0;
    int branch = -1;
    std::string deathReason;
};
struct Guidance {
    bool gpu = true, enabled = true, stats = false, paused = false, pending = false, valid = false;
    int recommended = 0;
    float percentages[prediction::CHOICES] = {};
    double predictionMs = 0, kernelMs = 0, frameMs = 0, p95Ms = 0;
    uint32_t rollouts = 512, horizon = prediction::HORIZON;
};
float distanceAt(float seconds);
float routeCenter(const prediction::Course &, unsigned stage, unsigned choice, float phase);
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
    const prediction::Course &course() const {
        return data;
    }
    bool approachingJunction() const;
    uint32_t decision() const;
    prediction::Request request(uint32_t id, uint32_t rollouts) const;
    void scene(std::vector<SceneInstance> &, const Guidance &) const;
    int score() const;

  private:
    prediction::Course data;
    State player;
    void die(const char *);
};
class Autopilot {
  public:
    GameControls sample(const Game &);

  private:
    unsigned run = 0;
};
void selfTest();
} // namespace survival
