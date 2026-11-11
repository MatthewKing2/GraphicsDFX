#pragma once
#include "game_input.hpp"
#include "plant_compute.hpp"
#include "renderer.hpp"
#include <string>
#include <vector>

namespace controlroom {
// Room/UI tuning lives here. Reactor/transport tuning lives in plant::Config.
struct Settings {
  float walkSpeed = 2.3f, sprintMultiplier = 1.6f, mouseSensitivity = .0022f;
  float fieldOfView = 68, predictionInterval = 3;
  float fogDensity = .13f, grain = .09f, ambientLight = 1.0f;
  uint32_t forecastTrials = 64, forecastSeconds = 60;
};
Mesh cubeMesh();
class Room {
  Settings settings;
  glm::vec3 eye{0, 1.7f, 1.0f}, savedEye{};
  float yaw = 0, pitch = 0, savedYaw = 0, savedPitch = 0;
  bool console = false;
  int selected = -1, hoveredRod = -1, hoveredBank = -1;
  glm::vec2 panelCursor{-1};
  std::vector<SceneInstance> architecture;
  std::string notice = "SELECT A ROD THEN A POWER BANK";
  void buildRoom();

public:
  explicit Room(Settings s = {});
  bool atConsole() const { return console; }
  // Returns a routing intent only. UI never changes the backend's state itself.
  bool input(const GameControls &, float dt, float aspect,
             const plant::Config &, uint32_t &routeRod, uint32_t &routeBank);
  void routeResult(bool accepted);
  FrameTransforms camera(float aspect, float elapsed = 0) const;
  void draw(std::vector<SceneInstance> &, const plant::Config &,
            const plant::Panel &, const plant::Forecast &, bool predicting,
            float elapsed, bool paused, const char *computeName,
            float aspect) const;
  void inspectionView(bool panel); // deterministic debug screenshot viewpoints
  static bool selfTest();
};
} // namespace controlroom
