#include "game.hpp"
#include "gpu_prediction.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr bool PROFILE_REACTOR = false; // Set true, or use --profile; off in normal play.
// A normal Windows sleep can round a 16.7 ms cap up to ~31 ms. This timer
// sleeps precisely without busy-spinning or changing the system-wide timer
// resolution.
class FramePacer {
#ifdef _WIN32
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
#endif
  public:
    ~FramePacer() {
#ifdef _WIN32
        if (timer)
            CloseHandle(timer);
#endif
    }
    void waitUntil(Clock::time_point deadline) {
#ifdef _WIN32
        auto remaining =
            std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10000000>>>(
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
class CpuPrediction {
    prediction::Course course;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stop = false, active = false, queued = false, ready = false;
    prediction::Request request;
    prediction::Result result;
    std::exception_ptr error;
    Clock::time_point started;
    std::thread worker;

  public:
    explicit CpuPrediction(const prediction::Course &c)
        : course(c), worker([this] {
              std::unique_lock<std::mutex> lock(mutex);
              for (;;) {
                  wake.wait(lock, [&] { return stop || queued; });
                  if (stop)
                      return;
                  auto input = request;
                  queued = false;
                  lock.unlock();
                  prediction::Result output;
                  std::exception_ptr failure;
                  try {
                      output = prediction::evaluate(course, input);
                  } catch (...) {
                      failure = std::current_exception();
                  }
                  lock.lock();
                  result = output;
                  error = failure;
                  ready = true;
              }
          }) {}
    ~CpuPrediction() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_one();
        worker.join();
    }
    bool busy() const {
        std::lock_guard<std::mutex> lock(mutex);
        return active;
    }
    void submit(prediction::Request r) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (active)
                throw std::logic_error("CPU prediction already pending");
            started = Clock::now();
            request = r;
            active = queued = true;
            ready = false;
            error = nullptr;
        }
        wake.notify_one();
    }
    bool poll(prediction::Result &r) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!ready)
            return false;
        ready = active = false;
        if (error)
            std::rethrow_exception(error);
        r = result;
        r.latencyMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        return true;
    }
};

void warmSnapshot(const prediction::Course &course, prediction::Request &request) {
    // Construct a replay snapshot outside the timed region, not an
    // unrealistically healthy reactor in the middle of a run. Never include its
    // future RNG state.
    uint32_t rng = 12345u ^ 0xa51ca6u;
    for (uint32_t tick = prediction::REACTOR_TICK; tick <= request.worldTick;
         tick += prediction::REACTOR_TICK)
        request.reactor =
            prediction::reactorStep(course, request.reactor, tick, request.routePower & 15u, rng);
}

void validateGpu(GpuPrediction &gpu, const prediction::Course &course) {
    unsigned cases = 0;
    for (uint32_t count : {1u, 63u, 64u, 65u, 512u, 2048u})
        for (uint32_t stage : {0u, 1u, 2u, 7u, 15u, 31u, 32u, 47u}) {
            prediction::Request r;
            r.rollouts = count;
            r.junction = stage;
            r.routeCount = prediction::choices(course, stage);
            r.worldTick = prediction::sectionStartTick(stage) +
                          std::min((cases % 3) * 10, prediction::entryTick(stage) - 1);
            r.seed = 12345 + stage;
            r.xMm = cases % 2 ? 35000 : -1200;
            r.zMm = cases % 2 ? -5000 : 6000;
            r.verticalVelocity = cases % 2 ? 90 : -120;
            r.velocity = cases % 2 ? -20 : 15;
            r.routePower = cases & 15u;
            r.pulseCharges = cases % 3u;
            r.pulseUntil = r.worldTick + (cases % 2u ? 0u : 36u);
            warmSnapshot(course, r);
            if (cases % 3u == 0u)
                for (auto &word : r.reactor.words)
                    word = 0xffffffffu; // All fault/age bits: test active and pulsed
                                        // hazards.
            auto cpu = prediction::evaluate(course, r);
            if (!gpu.submit(r) || gpu.submit(r))
                throw std::runtime_error("prediction busy contract failed");
            prediction::Result device;
            while (!gpu.poll(device))
                std::this_thread::yield();
            if (!prediction::sameResults(cpu, device))
                throw std::runtime_error("CPU/GPU tunnel prediction mismatch at case " +
                                         std::to_string(cases));
            ++cases;
        }
    std::cout << "PASS exact CPU/GPU tunnel aggregates: " << cases
              << " cases, 2/3/4 choices, partial groups and wrapped course\n";
}
void benchmark(GpuPrediction &gpu, const prediction::Course &course, const std::string &path,
               bool frozen = false) {
    CpuPrediction cpu(course);
    std::ofstream file(path);
    if (!file)
        throw std::runtime_error("cannot open benchmark output");
    std::string header = "mode,backend,section,choices,rollouts_per_choice,latency_ms,kernel_ms,"
                         "rollouts_per_s,steps_per_"
                         "s,p0,p1,p2,p3,course_bytes,request_bytes,result_bytes,agreement\n";
    file << header;
    std::cout << header;
    for (uint32_t count : {256u, 512u, 1024u})
        for (uint32_t stage : {0u, 1u, 4u}) {
            prediction::Request r;
            r.routePower = frozen ? 256u : 0u;
            r.rollouts = count;
            r.junction = stage;
            r.worldTick = prediction::sectionStartTick(stage);
            r.routeCount = prediction::choices(course, stage);
            warmSnapshot(course, r);
            auto reference = prediction::evaluate(course, r);
            for (unsigned backend = 0; backend < 2; ++backend) {
                double latency = 0, kernel = 0;
                prediction::Result last;
                for (int sample = 0; sample < 8; ++sample) {
                    if (backend)
                        gpu.submit(r);
                    else
                        cpu.submit(r);
                    while (backend ? !gpu.poll(last) : !cpu.poll(last))
                        std::this_thread::yield();
                    if (!prediction::sameResults(last, reference))
                        throw std::runtime_error("benchmark agreement failure");
                    if (sample >= 2) {
                        latency += last.latencyMs;
                        kernel += last.kernelMs;
                    }
                }
                latency /= 6;
                kernel /= 6;
                uint64_t steps = 0;
                for (const auto &route : last.routes)
                    steps += route.steps;
                std::ostringstream row;
                row << std::fixed << std::setprecision(6)
                    << (frozen ? "frozen_diagnostic" : "reactor") << ','
                    << (backend ? "gpu" : "cpu") << ',' << stage << ',' << r.routeCount << ','
                    << count << ',' << latency << ',' << kernel << ','
                    << r.routeCount * count * 1000.0 / latency << ',' << steps * 1000.0 / latency;
                for (unsigned choice = 0; choice < 4; ++choice)
                    row << ',' << prediction::survival(last, choice);
                row << ',' << sizeof(course) << ',' << sizeof(r) << ','
                    << 4 * sizeof(prediction::Summary) << ",true\n";
                file << row.str();
                std::cout << row.str();
            }
        }
    std::cout << "Saved " << path
              << ". Timings are not measured energy; payload excludes protocol "
                 "overhead.\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        bool gpuBackend = true, selfTest = false, validate = false, bench = false, smoke = false,
             autoplay = false, profile = PROFILE_REACTOR, noPrediction = false, frozen = false;
        int frameLimit = 0, captureFrame = 6, fpsLimit = 60;
        uint32_t rollouts = 1024, seed = 12345;
        bool seedSet = false;
        std::string output = "build/reactor-benchmark.csv", capture,
                    profileOutput = "build/reactor-profile.csv";
        for (int i = 1; i < argc; ++i) {
            std::string flag = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc)
                    throw std::invalid_argument("missing value for " + flag);
                return argv[i];
            };
            if (flag == "--backend") {
                auto name = value();
                if (name != "cpu" && name != "gpu")
                    throw std::invalid_argument("backend must be cpu or gpu");
                gpuBackend = name == "gpu";
            } else if (flag == "--rollouts")
                rollouts = uint32_t(std::stoul(value()));
            else if (flag == "--seed") {
                seed = uint32_t(std::stoul(value()));
                seedSet = true;
            } else if (flag == "--frames")
                frameLimit = std::stoi(value());
            else if (flag == "--capture-frame")
                captureFrame = std::stoi(value());
            else if (flag == "--fps")
                fpsLimit = std::stoi(value());
            else if (flag == "--output")
                output = value();
            else if (flag == "--capture")
                capture = value();
            else if (flag == "--self-test")
                selfTest = true;
            else if (flag == "--validate")
                validate = true;
            else if (flag == "--benchmark")
                bench = true;
            else if (flag == "--smoke")
                smoke = true;
            else if (flag == "--autoplay")
                autoplay = true;
            else if (flag == "--profile")
                profile = true;
            else if (flag == "--profile-output")
                profileOutput = value();
            else if (flag == "--no-prediction")
                noPrediction = true;
            else if (flag == "--freeze-reactor")
                frozen = true;
            else if (flag == "--help") {
                std::cout << "REACTOR / RUN\nSpace/Enter starts; Space pulses during "
                             "play. WASD/mouse "
                             "steers. No "
                             "brakes.\n"
                          << "R restart, Esc pause, F1 CPU/GPU, F2 route signs, F3 "
                             "console/window "
                             "stats.\n"
                          << "--backend cpu|gpu --rollouts 1024 --seed N --fps 60 "
                             "--frames N\n"
                          << "--profile [--profile-output path] --no-prediction "
                             "--freeze-reactor "
                             "(diagnostic only)\n"
                          << "--autoplay --smoke --self-test --validate --benchmark "
                             "[--output path]\n"
                          << "--capture build/tunnel.bmp [--capture-frame 6] (one "
                             "blocking debug "
                             "frame)\n";
                return 0;
            } else
                throw std::invalid_argument("unknown option: " + flag);
        }
        if (frameLimit < 0 || captureFrame < 0 || fpsLimit < 0 || fpsLimit > 1000)
            throw std::invalid_argument("invalid frame options");
        if (!seedSet && !selfTest && !validate && !bench && !smoke)
            seed = std::random_device{}();
        auto course = prediction::makeCourse(seed);
        prediction::Request checked;
        checked.rollouts = rollouts;
        checked.routeCount = prediction::choices(course, 0);
        prediction::validate(course, checked);
        if (selfTest || validate)
            survival::selfTest();
        if (selfTest && !validate && !bench)
            return 0;
        Renderer renderer(1280, 720, "REACTOR / RUN", survival::cubeMesh(), 24000, profile);
        VkPhysicalDeviceProperties info{};
        vkGetPhysicalDeviceProperties(renderer.computeContext().physical, &info);
        std::cout << "[device] " << info.deviceName << " course_seed=" << seed << '\n';
        GpuPrediction gpu(renderer.computeContext(), course);
        if (validate) {
            validateGpu(gpu, course);
            if (!bench)
                return 0;
        }
        if (bench) {
            benchmark(gpu, course, output, frozen);
            return 0;
        }
        GameInput input(renderer);
        survival::Game game(course, (autoplay || smoke || profile) ? (seed ^ 0xa51ca6u) : 0);
        survival::Autopilot bot;
        CpuPrediction cpu(course);
        FramePacer pacer;
        survival::Guidance guidance;
        guidance.gpu = gpuBackend;
        guidance.rollouts = rollouts;
        guidance.enabled = !noPrediction;
        double physicsMs = 0, sceneMs = 0, drawMs = 0, sleepMs = 0, gpuRenderMs = 0, fenceMs = 0,
               uploadMs = 0, forecastGpuMs = 0, forecastCpuMs = 0, forecastLatencyMs = 0;
        unsigned gpuFrames = 0, forecastCount = 0, submitted = 0, maxStage = 0;
        std::vector<double> profileFrames;
        auto profileStart = Clock::now();
        std::vector<SceneInstance> scene;
        scene.reserve(24000);
        std::vector<double> frameTimes;
        frameTimes.reserve(180);
        double accumulator = 0, wallTime = 0, nextPrediction = 0, lastAccepted = -100;
        auto previous = Clock::now();
        uint32_t requestId = 0, validRun = 0, validSection = 0, accepted = 0;
        uint32_t validPower = 0, validPulse = 0, validCharges = 0;
        bool jumpEdge = false, restartEdge = false;
        int frameNumber = 0;
        while (!renderer.shouldClose()) {
            auto start = Clock::now();
            double dt =
                std::clamp(std::chrono::duration<double>(start - previous).count(), 0.0, .1);
            previous = start;
            wallTime += dt;
            renderer.pollEvents();
            auto controls = input.sample();
            if (controls.pause)
                guidance.paused = !guidance.paused;
            if (!controls.focused && !smoke && !autoplay && !game.state().title)
                guidance.paused = true;
            input.capture(!guidance.paused && !game.state().title && !game.state().dead && !smoke &&
                          !autoplay);
            if (controls.backend) {
                guidance.gpu = !guidance.gpu;
                guidance.valid = false;
                nextPrediction = 0;
            }
            if (controls.guidance) {
                guidance.enabled = !guidance.enabled;
                guidance.valid = false;
                nextPrediction = 0;
            }
            if (controls.stats)
                guidance.stats = !guidance.stats;
            if (!guidance.paused) {
                if (!autoplay && !smoke)
                    game.look(controls.mouseX, controls.mouseY);
                auto physicsStart = Clock::now();
                accumulator += smoke ? 4.0 / 120 : dt;
                jumpEdge |= controls.jump;
                restartEdge |= controls.restart;
                int catchup = 0;
                const int maxCatchup = smoke ? 24 : 12;
                while (accumulator >= 1.0 / 120 && catchup++ < maxCatchup) {
                    auto step = autoplay || smoke ? bot.sample(game) : controls;
                    if (!autoplay && !smoke) {
                        step.jump = jumpEdge;
                        step.restart = restartEdge;
                    }
                    game.step(step, 1.0f / 120);
                    jumpEdge = restartEdge = false;
                    accumulator -= 1.0 / 120;
                    maxStage = std::max(maxStage, game.state().stage);
                }
                if (catchup >= maxCatchup)
                    accumulator = 0;
                if (profile)
                    physicsMs +=
                        std::chrono::duration<double, std::milli>(Clock::now() - physicsStart)
                            .count();
            } else
                accumulator = 0;
            prediction::Result result;
            auto accept = [&](const prediction::Result &r, bool fromGpu) {
                if (profile) {
                    ++forecastCount;
                    forecastLatencyMs += r.latencyMs;
                    if (fromGpu)
                        forecastGpuMs += r.kernelMs;
                    else
                        forecastCpuMs += r.kernelMs;
                }
                const auto &state = game.state();
                if (!guidance.enabled || fromGpu != guidance.gpu || !game.approachingJunction() ||
                    (r.request.routePower & 15u) != state.routePower ||
                    r.request.pulseUntil != state.pulseUntil ||
                    r.request.pulseCharges != state.pulseCharges ||
                    !prediction::applies(r.request, state.run, game.decision(),
                                         unsigned(state.worldTime * 60)))
                    return;
                int best = 0;
                for (unsigned choice = 0; choice < r.request.routeCount; ++choice) {
                    guidance.percentages[choice] = prediction::survival(r, choice);
                    if (guidance.percentages[choice] > guidance.percentages[best])
                        best = int(choice);
                }
                if (!guidance.valid ||
                    guidance.percentages[best] > guidance.percentages[guidance.recommended] + .03f)
                    guidance.recommended = best;
                guidance.valid = true;
                validRun = state.run;
                validSection = game.decision();
                validPower = state.routePower;
                validPulse = state.pulseUntil;
                validCharges = state.pulseCharges;
                lastAccepted = wallTime;
                guidance.predictionMs = r.latencyMs;
                guidance.kernelMs = fromGpu ? r.kernelMs : 0;
                ++accepted;
            };
            if (cpu.poll(result))
                accept(result, false);
            if (gpu.poll(result))
                accept(result, true);
            if (validRun != game.state().run || validSection != game.decision() ||
                validPower != game.state().routePower || validPulse != game.state().pulseUntil ||
                validCharges != game.state().pulseCharges || !game.approachingJunction() ||
                wallTime - lastAccepted > .35)
                guidance.valid = false;
            if (!guidance.paused && guidance.enabled && game.approachingJunction() &&
                wallTime >= nextPrediction && !cpu.busy() && !gpu.busy()) {
                auto r = game.request(++requestId, rollouts);
                if (frozen)
                    r.routePower |= 256u;
                if (guidance.gpu)
                    gpu.submit(r);
                else
                    cpu.submit(r);
                ++submitted;
                nextPrediction = wallTime + .2;
            }
            guidance.pending = cpu.busy() || gpu.busy();
            auto sceneStart = Clock::now();
            game.scene(scene, guidance);
            if (profile)
                sceneMs +=
                    std::chrono::duration<double, std::milli>(Clock::now() - sceneStart).count();
            if (!capture.empty() && frameNumber == captureFrame)
                renderer.captureNextFrame(capture);
            auto drawStart = Clock::now();
            renderer.drawScene(game.transforms(renderer.aspectRatio()), scene);
            if (profile) {
                drawMs +=
                    std::chrono::duration<double, std::milli>(Clock::now() - drawStart).count();
                FrameTimings timing;
                if (renderer.readCompletedSceneTiming(timing)) {
                    ++gpuFrames;
                    gpuRenderMs += timing.gpuRenderMs;
                    fenceMs += timing.fenceWaitMs;
                    uploadMs += timing.uploadMs;
                }
            }
            auto sleepStart = Clock::now();
            if (fpsLimit > 0)
                pacer.waitUntil(start + std::chrono::duration_cast<Clock::duration>(
                                            std::chrono::duration<double>(1.0 / fpsLimit)));
            if (profile) {
                sleepMs +=
                    std::chrono::duration<double, std::milli>(Clock::now() - sleepStart).count();
                profileFrames.push_back(
                    std::chrono::duration<double, std::milli>(Clock::now() - start).count());
            }
            frameTimes.push_back(
                std::chrono::duration<double, std::milli>(Clock::now() - start).count());
            if (frameTimes.size() == 180) {
                double total = 0;
                for (double value : frameTimes)
                    total += value;
                std::sort(frameTimes.begin(), frameTimes.end());
                guidance.frameMs = total / frameTimes.size();
                guidance.p95Ms = frameTimes[170];
                std::cout << "[dive] frame_ms=" << guidance.frameMs << " p95_ms=" << guidance.p95Ms
                          << " backend=" << (guidance.gpu ? "gpu" : "cpu")
                          << " prediction_ms=" << guidance.predictionMs
                          << " kernel_ms=" << guidance.kernelMs << " section=" << game.state().stage
                          << " speed=" << game.state().speed << " objects=" << scene.size() << '\n';
                if (guidance.stats) {
                    std::string title = "REACTOR / RUN - " +
                                        std::to_string(int(1000 / guidance.frameMs)) + " FPS - " +
                                        (guidance.gpu ? "GPU" : "CPU");
                    input.title(title.c_str());
                } else
                    input.title("REACTOR / RUN");
                frameTimes.clear();
            }
            ++frameNumber;
            if (smoke && frameNumber >= 480) {
                if (accepted < 3 || maxStage < 1)
                    throw std::runtime_error("live route guidance not connected");
                std::cout << "PASS rendered reactor run: stages=" << maxStage
                          << " speed=" << game.state().speed << " accepted_predictions=" << accepted
                          << " frames=" << frameNumber << '\n';
                break;
            }
            if (frameLimit && frameNumber >= frameLimit)
                break;
        }
        if (profile && frameNumber > 0) {
            double wallMs =
                std::chrono::duration<double, std::milli>(Clock::now() - profileStart).count();
            std::sort(profileFrames.begin(), profileFrames.end());
            std::ofstream file(profileOutput);
            if (!file)
                throw std::runtime_error("cannot open profile output");
            std::ostringstream summary;
            summary << "backend,mode,frames,wall_ms,fps,p95_frame_ms,world_cpu_ms_"
                       "per_frame,scene_"
                       "cpu_ms_per_frame,"
                       "draw_cpu_ms_per_frame,pacer_ms_per_frame,render_gpu_ms_per_"
                       "frame,fence_ms_"
                       "per_frame,"
                       "upload_ms_per_frame,forecast_gpu_total_ms,forecast_cpu_total_"
                       "ms,forecast_"
                       "latency_mean_ms,"
                       "requests,completed,accepted,request_bytes,result_bytes,"
                       "telemetry_bytes\n";
            summary << std::fixed << std::setprecision(6) << (gpuBackend ? "gpu" : "cpu") << ','
                    << (noPrediction ? "off"
                        : frozen     ? "frozen_diagnostic"
                                     : "reactor")
                    << ',' << frameNumber << ',' << wallMs << ',' << frameNumber * 1000 / wallMs
                    << ','
                    << profileFrames[std::min(profileFrames.size() - 1,
                                              size_t(profileFrames.size() * .95))]
                    << ',' << physicsMs / frameNumber << ',' << sceneMs / frameNumber << ','
                    << drawMs / frameNumber << ',' << sleepMs / frameNumber << ','
                    << gpuRenderMs / std::max(1u, gpuFrames) << ','
                    << fenceMs / std::max(1u, gpuFrames) << ','
                    << uploadMs / std::max(1u, gpuFrames) << ',' << forecastGpuMs << ','
                    << forecastCpuMs << ',' << forecastLatencyMs / std::max(1u, forecastCount)
                    << ',' << submitted << ',' << forecastCount << ',' << accepted << ','
                    << sizeof(prediction::Request) << ',' << 4 * sizeof(prediction::Summary) << ','
                    << sizeof(prediction::Telemetry) << '\n';
            file << summary.str();
            std::cout << "[reactor profile]\n" << summary.str();
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Game error: " << error.what() << '\n';
        return 1;
    }
}
