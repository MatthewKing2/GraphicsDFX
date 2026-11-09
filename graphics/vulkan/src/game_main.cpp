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
// A normal Windows sleep can round a 16.7 ms cap up to ~31 ms. This timer sleeps
// precisely without busy-spinning or changing the system-wide timer resolution.
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

void validateGpu(GpuPrediction &gpu, const prediction::Course &course) {
    unsigned cases = 0;
    for (uint32_t count : {1u, 63u, 64u, 65u, 512u, 2048u})
        for (uint32_t stage : {0u, 1u, 2u, 7u, 15u, 31u, 32u, 47u}) {
            prediction::Request r;
            r.rollouts = count;
            r.junction = stage;
            r.routeCount = prediction::choices(course, stage);
            r.worldTick = stage * 144 + (cases % 3) * 10;
            r.seed = 12345 + stage;
            r.xMm = cases % 2 ? 900 : -1200;
            r.velocity = cases % 2 ? -20 : 15;
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
void benchmark(GpuPrediction &gpu, const prediction::Course &course, const std::string &path) {
    CpuPrediction cpu(course);
    std::ofstream file(path);
    if (!file)
        throw std::runtime_error("cannot open benchmark output");
    std::string header =
        "backend,section,choices,rollouts_per_choice,latency_ms,kernel_ms,rollouts_per_s,steps_per_"
        "s,p0,p1,p2,p3,course_bytes,request_bytes,result_bytes,agreement\n";
    file << header;
    std::cout << header;
    for (uint32_t count : {256u, 512u, 1024u})
        for (uint32_t stage : {0u, 1u, 4u}) {
            prediction::Request r;
            r.rollouts = count;
            r.junction = stage;
            r.worldTick = stage * 144;
            r.routeCount = prediction::choices(course, stage);
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
                row << std::fixed << std::setprecision(6) << (backend ? "gpu" : "cpu") << ','
                    << stage << ',' << r.routeCount << ',' << count << ',' << latency << ','
                    << kernel << ',' << r.routeCount * count * 1000.0 / latency << ','
                    << steps * 1000.0 / latency;
                for (unsigned choice = 0; choice < 4; ++choice)
                    row << ',' << prediction::survival(last, choice);
                row << ',' << sizeof(course) << ',' << sizeof(r) << ','
                    << 4 * sizeof(prediction::Summary) << ",true\n";
                file << row.str();
                std::cout << row.str();
            }
        }
    std::cout << "Saved " << path
              << ". Timings are not measured energy; payload excludes protocol overhead.\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        bool gpuBackend = true, selfTest = false, validate = false, bench = false, smoke = false,
             autoplay = false;
        int frameLimit = 0, captureFrame = 6, fpsLimit = 60;
        uint32_t rollouts = 512, seed = 12345;
        bool seedSet = false;
        std::string output = "build/tunnel-benchmark.csv", capture;
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
            else if (flag == "--help") {
                std::cout
                    << "VOID / DIVE\nSpace/Enter starts or retries. A/D or mouse steers. No "
                       "brakes.\n"
                    << "R restart, Esc pause, F1 CPU/GPU, F2 route signs, F3 console/window "
                       "stats.\n"
                    << "--backend cpu|gpu --rollouts 512 --seed N --fps 60 --frames N\n"
                    << "--autoplay --smoke --self-test --validate --benchmark [--output path]\n"
                    << "--capture build/tunnel.bmp [--capture-frame 6] (one blocking debug "
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
        Renderer renderer(1280, 720, "VOID / DIVE", survival::cubeMesh(), 24000);
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
            benchmark(gpu, course, output);
            return 0;
        }
        GameInput input(renderer);
        survival::Game game(course);
        survival::Autopilot bot;
        CpuPrediction cpu(course);
        FramePacer pacer;
        survival::Guidance guidance;
        guidance.gpu = gpuBackend;
        guidance.rollouts = rollouts;
        std::vector<SceneInstance> scene;
        scene.reserve(24000);
        std::vector<double> frameTimes;
        frameTimes.reserve(180);
        double accumulator = 0, wallTime = 0, nextPrediction = 0, lastAccepted = -100;
        auto previous = Clock::now();
        uint32_t requestId = 0, validRun = 0, validSection = 0, accepted = 0;
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
                accumulator += smoke ? 8.0 / 120 : dt;
                jumpEdge |= controls.jump;
                restartEdge |= controls.restart;
                int catchup = 0;
                while (accumulator >= 1.0 / 120 && catchup++ < 12) {
                    auto step = autoplay || smoke ? bot.sample(game) : controls;
                    if (!autoplay && !smoke) {
                        step.jump = jumpEdge;
                        step.restart = restartEdge;
                    }
                    game.step(step, 1.0f / 120);
                    jumpEdge = restartEdge = false;
                    accumulator -= 1.0 / 120;
                    if (smoke && game.state().dead)
                        throw std::runtime_error("smoke died: " + game.state().deathReason);
                }
                if (catchup >= 12)
                    accumulator = 0;
            } else
                accumulator = 0;
            prediction::Result result;
            auto accept = [&](const prediction::Result &r, bool fromGpu) {
                const auto &state = game.state();
                if (!guidance.enabled || fromGpu != guidance.gpu || !game.approachingJunction() ||
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
                !game.approachingJunction() || wallTime - lastAccepted > .35)
                guidance.valid = false;
            if (!guidance.paused && guidance.enabled && game.approachingJunction() &&
                wallTime >= nextPrediction && !cpu.busy() && !gpu.busy()) {
                auto r = game.request(++requestId, rollouts);
                if (guidance.gpu)
                    gpu.submit(r);
                else
                    cpu.submit(r);
                nextPrediction = wallTime + .1;
            }
            guidance.pending = cpu.busy() || gpu.busy();
            game.scene(scene, guidance);
            if (!capture.empty() && frameNumber == captureFrame)
                renderer.captureNextFrame(capture);
            renderer.drawScene(game.transforms(renderer.aspectRatio()), scene);
            if (fpsLimit > 0)
                pacer.waitUntil(start + std::chrono::duration_cast<Clock::duration>(
                                            std::chrono::duration<double>(1.0 / fpsLimit)));
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
                    std::string title = "VOID / DIVE - " +
                                        std::to_string(int(1000 / guidance.frameMs)) + " FPS - " +
                                        (guidance.gpu ? "GPU" : "CPU");
                    input.title(title.c_str());
                } else
                    input.title("VOID / DIVE");
                frameTimes.clear();
            }
            ++frameNumber;
            if (smoke && game.state().stage >= 20) {
                if (accepted < 3)
                    throw std::runtime_error("live route guidance not connected");
                std::cout << "PASS rendered dive: 20 decisions, speed=" << game.state().speed
                          << " accepted_predictions=" << accepted << " frames=" << frameNumber
                          << '\n';
                break;
            }
            if (frameLimit && frameNumber >= frameLimit)
                break;
            if (smoke && frameNumber > 1200)
                throw std::runtime_error("dive smoke did not reach 20 forks");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Game error: " << error.what() << '\n';
        return 1;
    }
}
