#include "control_room.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
struct Prediction {
  plant::Forecast result;
  uint32_t generation = 0;
  double milliseconds = 0;
};
double ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
// Avoid the normal Windows sleep rounding our 60 FPS cap to ~30-40 FPS.
class Pacer {
#ifdef _WIN32
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
#endif
public:
  ~Pacer() {
#ifdef _WIN32
    if (timer)
      CloseHandle(timer);
#endif
  }
  void wait(Clock::time_point deadline) {
#ifdef _WIN32
    auto remaining =
        std::chrono::duration_cast<
            std::chrono::duration<long long, std::ratio<1, 10000000>>>(
            deadline - Clock::now())
            .count();
    if (remaining <= 0)
      return;
    LARGE_INTEGER due;
    due.QuadPart = -remaining;
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
      return;
#endif
    std::this_thread::sleep_until(deadline);
  }
};
} // namespace
int main(int argc, char **argv) {
  try {
    bool test = false, profile = false, panelView = false, roomView = false;
    int frames = 0;
    float artTime = -1;
    std::string capture;
    plant::Config config;
    controlroom::Settings settings;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      auto value = [&]() {
        if (i + 1 >= argc)
          throw std::invalid_argument("missing value for " + arg);
        return std::string(argv[++i]);
      };
      if (arg == "--self-test")
        test = true;
      else if (arg == "--profile")
        profile = true;
      else if (arg == "--frames")
        frames = std::stoi(value());
      else if (arg == "--capture")
        capture = value();
      else if (arg == "--panel-view")
        panelView = true;
      else if (arg == "--room-view")
        roomView = true;
      else if (arg == "--fog")
        settings.fogDensity = std::stof(value());
      else if (arg == "--ambient")
        settings.ambientLight = std::stof(value());
      else if (arg == "--art-time")
        artTime = std::stof(value());
      else if (arg == "--reactors")
        config.reactors = uint32_t(std::stoul(value()));
      else if (arg == "--rods")
        config.rodsPerReactor = uint32_t(std::stoul(value()));
      else if (arg == "--banks")
        config.banks = uint32_t(std::stoul(value()));
      else if (arg == "--plugs")
        config.plugsPerBank = uint32_t(std::stoul(value()));
      else if (arg == "--segments")
        config.segments = uint32_t(std::stoul(value()));
      else if (arg == "--trials")
        settings.forecastTrials = uint32_t(std::stoul(value()));
      else if (arg == "--help") {
        std::cout << "Control room: WASD walk, mouse look, Shift hurry. Walk "
                     "to console, E to use/leave.\n"
                  << "At console: click rod, click power bank to reroute; "
                     "right click cancels.\n"
                  << "R resets plant; Escape pauses/releases mouse (or leaves "
                     "console).\n"
                  << "Options: --self-test --profile --frames N --capture "
                     "file.bmp --panel-view --room-view\n"
                  << "         --reactors N --rods N --banks N --plugs N "
                     "--segments N --trials N\n"
                  << "         --fog 0.13 --ambient 1.0 --art-time SECONDS "
                     "(freeze visual pulse for captures)\n";
        return 0;
      } else
        throw std::invalid_argument("unknown option: " + arg);
    }
    plant::validate(config);
    if (config.reactors > 4 || config.rodsPerReactor > 12)
      throw std::invalid_argument(
          "this physical panel fits up to 4 reactors / 12 rods per row");
    if (!settings.forecastTrials || settings.forecastTrials > 4096 ||
        frames < 0 || !std::isfinite(settings.fogDensity) ||
        settings.fogDensity < 0 || settings.fogDensity > 1 ||
        !std::isfinite(settings.ambientLight) || settings.ambientLight < 0 ||
        settings.ambientLight > 4 || !std::isfinite(artTime))
      throw std::invalid_argument("invalid trial/frame count");
    if (test) {
      plant::selfTest();
      controlroom::Room::selfTest();
      std::cout << "Control room tests passed.\n";
      return 0;
    }

    // CPU compute + GPU rendering baseline; backend does not see the room.
    Renderer renderer(1280, 720, "VESSEL 09 - Reactor control room",
                      controlroom::cubeMesh(), 30000, profile,
                      {"build/room-vert.spv", "build/room-frag.spv"});
    GameInput input(renderer);
    auto backend = plant::createBackend();
    backend->reset(config, plant::initialState(config));
    controlroom::Room room(settings);
    if (panelView || roomView)
      room.inspectionView(panelView);
    plant::Panel panel;
    backend->readPanel(panel);
    plant::Forecast forecast;
    // A bounded background job on an explicit snapshot, never a growing
    // queue. Future GPU/FPGA submit/poll can replace this CPU-only adapter.
    std::future<Prediction> prediction;
    uint32_t generation = 0;
    bool pending = false, paused = false, wasFocused = true;
    double thermalTime = 0, nextForecast = 0, elapsed = 0;
    double liveMs = 0, predictionMs = 0, sceneMs = 0, renderMs = 0, gpuMs = 0;
    uint32_t ticks = 0, predictions = 0, gpuFrames = 0;
    std::vector<SceneInstance> scene;
    scene.reserve(30000);
    auto previous = Clock::now(), beginning = previous;
    int frame = 0;
    Pacer pacer;
    std::cout << "Backend: " << backend->name() << "; render: Vulkan.\n"
              << "WASD/mouse/Shift. Walk to the panel and press E. Click rod, "
                 "then bank.\n"
              << "Thermal time runs at console only; forecast assumes NO later "
                 "intervention.\n";
    while (!renderer.shouldClose() && (!frames || frame < frames)) {
      auto start = Clock::now();
      float dt = float(std::clamp(
          std::chrono::duration<double>(start - previous).count(), 0.0, .1));
      previous = start;
      elapsed += dt;
      renderer.pollEvents();
      auto controls = input.sample();
      const bool automated = frames != 0;
      if (!automated && !controls.focused && wasFocused && !room.atConsole())
        paused = true;
      wasFocused = controls.focused;
      if (controls.pause && !room.atConsole())
        paused = !paused;
      if (controls.restart) {
        backend->reset(config, plant::initialState(config));
        backend->readPanel(panel);
        forecast = {};
        ++generation;
        thermalTime = 0;
        nextForecast = 0;
      }
      uint32_t routeRod = 0, routeBank = 0;
      if (!paused && (automated || controls.focused)) {
        if (room.input(controls, dt, renderer.aspectRatio(), config, routeRod,
                       routeBank)) {
          bool accepted = backend->reroute(routeRod, routeBank);
          room.routeResult(accepted);
          if (accepted) {
            backend->readPanel(panel);
            forecast = {};
            nextForecast = 0;
          }
        }
        if (room.atConsole()) {
          thermalTime += dt;
          if (thermalTime >= 1) {
            auto t = Clock::now();
            backend->advance(1);
            backend->readPanel(panel);
            liveMs += ms(t);
            ++ticks;
            thermalTime -= 1;
          }
        } else
          thermalTime = 0;
      }
      input.capture(!room.atConsole() && !paused && !automated);
      if (pending && prediction.wait_for(std::chrono::seconds(0)) ==
                         std::future_status::ready) {
        auto p = prediction.get();
        pending = false;
        predictionMs += p.milliseconds;
        ++predictions;
        if (p.generation == generation && p.result.revision == panel.revision)
          forecast = p.result;
      }
      if (!pending && !paused && elapsed >= nextForecast &&
          (room.atConsole() || !forecast.trials)) {
        auto snapshot = backend->save();
        uint32_t jobGeneration = generation;
        plant::ForecastRequest request{settings.forecastTrials,
                                       settings.forecastSeconds, 8128};
        prediction = std::async(std::launch::async,
                                [config, snapshot, request, jobGeneration]() {
                                  auto start = Clock::now();
                                  auto isolated = plant::createBackend();
                                  isolated->reset(config, snapshot);
                                  Prediction out;
                                  out.generation = jobGeneration;
                                  isolated->forecast(request, out.result);
                                  out.milliseconds = ms(start);
                                  return out;
                                });
        pending = true;
        nextForecast = elapsed + settings.predictionInterval;
      }
      auto build = Clock::now();
      room.draw(scene, config, panel, forecast, pending, float(elapsed), paused,
                backend->name(), renderer.aspectRatio());
      sceneMs += ms(build);
      if (!capture.empty() && frame == (frames ? frames - 1 : 8))
        renderer.captureNextFrame(capture);
      auto draw = Clock::now();
      renderer.drawScene(room.camera(renderer.aspectRatio(),
                                     artTime < 0 ? float(elapsed) : artTime),
                         scene);
      renderMs += ms(draw);
      FrameTimings timing;
      if (profile && renderer.readCompletedSceneTiming(timing)) {
        gpuMs += timing.gpuRenderMs;
        ++gpuFrames;
      }
      ++frame;
      pacer.wait(start + std::chrono::microseconds(16667));
    }
    if (pending) {
      auto p = prediction.get();
      predictionMs += p.milliseconds;
      ++predictions;
    }
    if (profile) {
      std::cout << "PROFILE frames=" << frame
                << " elapsed_s=" << ms(beginning) / 1000
                << " scene_ms/frame=" << sceneMs / std::max(1, frame)
                << " render_host_ms/frame=" << renderMs / std::max(1, frame)
                << " gpu_render_ms/frame=" << gpuMs / std::max(1u, gpuFrames)
                << " plant_ms/tick=" << liveMs / std::max(1u, ticks)
                << " forecast_ms/job="
                << predictionMs / std::max(1u, predictions)
                << " thermal_ticks=" << ticks
                << " forecast_jobs=" << predictions << "\n";
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Control room error: " << e.what() << "\n";
    return 1;
  }
}
