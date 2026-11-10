// Standalone circuit experiment. No renderer, window, game physics or game edits.
#include "../../src/prediction.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>
using Clock = std::chrono::steady_clock;
using State = prediction::ReactorState;
using uint = uint32_t;
static uint mix(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x | 1u;
}
static void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + ": " + std::to_string(r));
}
struct Parameters {
    uint samples = 4096, steps = 100, mask = 0, tick = 1800, seed = 12345;
};
static_assert(sizeof(Parameters) == 20);

// Persistent workers, equal samples per worker; no per-trial thread creation.
class Pool {
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable work, done;
    std::function<void(uint)> task;
    uint generation = 0, remaining = 0;
    bool stop = false;

  public:
    explicit Pool(uint count) {
        for (uint n = 0; n < count; ++n)
            workers.emplace_back([this, n] {
                uint seen = 0;
                std::unique_lock<std::mutex> lock(mutex);
                for (;;) {
                    work.wait(lock, [&] { return stop || generation != seen; });
                    if (stop)
                        return;
                    seen = generation;
                    auto run = task;
                    lock.unlock();
                    run(n);
                    lock.lock();
                    if (--remaining == 0)
                        done.notify_one();
                }
            });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        work.notify_all();
        for (auto &t : workers)
            t.join();
    }
    void run(const std::function<void(uint)> &fn) {
        std::unique_lock<std::mutex> lock(mutex);
        task = fn;
        remaining = uint(workers.size());
        ++generation;
        work.notify_all();
        done.wait(lock, [&] { return remaining == 0; });
    }
};

class Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    struct Buffer {
        VkBuffer handle{};
        VkDeviceMemory memory{};
        void *mapped{};
    };
    Buffer rules, initial, output, readback;
    void allocate(Buffer &b, VkDeviceSize bytes, VkBufferUsageFlags usage, bool host) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &info, nullptr, &b.handle), "buffer");
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(device, b.handle, &needs);
        VkPhysicalDeviceMemoryProperties types;
        vkGetPhysicalDeviceMemoryProperties(physical, &types);
        VkMemoryPropertyFlags flags =
            host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        uint chosen = types.memoryTypeCount;
        for (uint i = 0; i < types.memoryTypeCount; ++i)
            if ((needs.memoryTypeBits & (1u << i)) &&
                (types.memoryTypes[i].propertyFlags & flags) == flags) {
                chosen = i;
                break;
            }
        if (chosen == types.memoryTypeCount)
            throw std::runtime_error("memory type unavailable");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = chosen;
        check(vkAllocateMemory(device, &alloc, nullptr, &b.memory), "allocate");
        check(vkBindBufferMemory(device, b.handle, b.memory, 0), "bind");
        if (host)
            check(vkMapMemory(device, b.memory, 0, bytes, 0, &b.mapped), "map");
    }
    void begin() {
        check(vkResetFences(device, 1, &fence), "reset fence");
        check(vkResetCommandBuffer(cmd, 0), "reset commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &begin), "begin");
    }
    void finish() {
        check(vkEndCommandBuffer(cmd), "end");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &cmd;
        check(vkQueueSubmit(queue, 1, &info, fence), "submit");
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
    }

  public:
    std::string name;
    Gpu(const prediction::Course &course, uint capacity, const std::filesystem::path &shaderPath) {
        try {
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName = "Relay compute lab";
            app.apiVersion = VK_API_VERSION_1_0;
            VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            create.pApplicationInfo = &app;
            check(vkCreateInstance(&create, nullptr, &instance), "instance");
            uint count = 0;
            check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "devices");
            std::vector<VkPhysicalDevice> devices(count);
            check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "devices");
            uint family = 0;
            int best = -1;
            for (auto candidate : devices) {
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(candidate, &props);
                uint n = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
                std::vector<VkQueueFamilyProperties> families(n);
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, families.data());
                for (uint i = 0; i < n; ++i)
                    if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                        int score =
                            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                        if (score > best) {
                            best = score;
                            physical = candidate;
                            family = i;
                            name = props.deviceName;
                        }
                        break;
                    }
            }
            if (!physical)
                throw std::runtime_error("no compute device");
            float priority = 1;
            VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            qi.queueFamilyIndex = family;
            qi.queueCount = 1;
            qi.pQueuePriorities = &priority;
            VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            di.queueCreateInfoCount = 1;
            di.pQueueCreateInfos = &qi;
            check(vkCreateDevice(physical, &di, nullptr, &device), "device");
            vkGetDeviceQueue(device, family, 0, &queue);
            VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.queueFamilyIndex = family;
            pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            check(vkCreateCommandPool(device, &pi, nullptr, &pool), "pool");
            VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ci.commandPool = pool;
            ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ci.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(device, &ci, &cmd), "commands");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            check(vkCreateFence(device, &fi, nullptr, &fence), "fence");
            allocate(rules, sizeof(course.reactorRules), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            allocate(initial, sizeof(State), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            allocate(output, capacity * sizeof(State),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
            allocate(readback, capacity * sizeof(State), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            std::memcpy(rules.mapped, course.reactorRules.data(), sizeof(course.reactorRules));
            VkDescriptorSetLayoutBinding bindings[3]{};
            for (uint i = 0; i < 3; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorCount = 1;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = 3;
            li.pBindings = bindings;
            check(vkCreateDescriptorSetLayout(device, &li, nullptr, &setLayout), "set layout");
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.maxSets = 1;
            dpi.poolSizeCount = 1;
            dpi.pPoolSizes = &size;
            check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptorPool),
                  "descriptor pool");
            VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            si.descriptorPool = descriptorPool;
            si.descriptorSetCount = 1;
            si.pSetLayouts = &setLayout;
            check(vkAllocateDescriptorSets(device, &si, &set), "descriptor set");
            VkDescriptorBufferInfo infos[] = {{rules.handle, 0, sizeof(course.reactorRules)},
                                              {initial.handle, 0, sizeof(State)},
                                              {output.handle, 0, capacity * sizeof(State)}};
            VkWriteDescriptorSet writes[3]{};
            for (uint i = 0; i < 3; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &infos[i];
            }
            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
            VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &setLayout;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &push;
            check(vkCreatePipelineLayout(device, &pli, nullptr, &layout), "pipeline layout");
            std::ifstream file(shaderPath, std::ios::binary | std::ios::ate);
            if (!file)
                throw std::runtime_error("missing relay.spv; run make in tools/relay_lab");
            size_t bytes = size_t(file.tellg());
            if (!bytes || bytes % 4)
                throw std::runtime_error("bad shader");
            std::vector<uint> code(bytes / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char *>(code.data()), bytes);
            VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            sm.codeSize = bytes;
            sm.pCode = code.data();
            VkShaderModule module;
            check(vkCreateShaderModule(device, &sm, nullptr, &module), "shader");
            VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cp.layout = layout;
            cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cp.stage.module = module;
            cp.stage.pName = "main";
            auto result =
                vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline);
            vkDestroyShaderModule(device, module, nullptr);
            check(result, "pipeline");
        } catch (...) {
            destroy();
            throw;
        }
    }
    ~Gpu() { destroy(); }
    void destroy() {
        if (device)
            vkDeviceWaitIdle(device);
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
        if (descriptorPool)
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        for (auto *b : {&rules, &initial, &output, &readback}) {
            if (b->mapped)
                vkUnmapMemory(device, b->memory);
            if (b->handle)
                vkDestroyBuffer(device, b->handle, nullptr);
            if (b->memory)
                vkFreeMemory(device, b->memory, nullptr);
        }
        if (fence)
            vkDestroyFence(device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(device, pool, nullptr);
        if (device)
            vkDestroyDevice(device, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
    void run(const State &state, const Parameters &p) {
        std::memcpy(initial.mapped, &state, sizeof(state));
        begin();
        // Host coherent writes are available at submission. Prior dispatch has
        // finished at its fence; make output writes ordered before reuse.
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0,
                             nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, (p.samples + 63) / 64, 1, 1);
        finish();
    }
    std::vector<State> read(uint samples) {
        begin();
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        VkBufferCopy copy{0, 0, samples * sizeof(State)};
        vkCmdCopyBuffer(cmd, output.handle, readback.handle, 1, &copy);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &b, 0, nullptr, 0, nullptr);
        finish();
        std::vector<State> result(samples);
        std::memcpy(result.data(), readback.mapped, samples * sizeof(State));
        return result;
    }
};
static State warm(const prediction::Course &c) {
    State s;
    uint rng = 12345;
    for (uint tick = 6; tick <= 1800; tick += 6)
        s = prediction::reactorStep(c, s, tick, 0, rng);
    return s;
}
static void cpu(const prediction::Course &c, const State &initial, const Parameters &p,
                std::vector<State> &out, Pool &pool, uint workers) {
    pool.run([&](uint worker) {
        for (uint id = p.samples * worker / workers; id < p.samples * (worker + 1) / workers;
             ++id) {
            auto state = initial;
            uint rng = mix(p.seed ^ (id * 0x9e3779b9u));
            for (uint step = 0; step < p.steps; ++step)
                state = prediction::reactorStep(c, state, p.tick + (step + 1) * 6, p.mask, rng);
            out[id] = state;
        }
    });
}
static uint checksum(const std::vector<State> &states) {
    uint hash = 2166136261u;
    for (const auto &s : states)
        for (uint w : s.words)
            hash = (hash ^ w) * 16777619u;
    return hash;
}
static void words(const State &s) {
    std::cout << '[';
    for (uint i = 0; i < 16; ++i)
        std::cout << (i ? "," : "") << s.words[i];
    std::cout << ']';
}
static void fixture(const prediction::Course &c) {
    std::cout << "{\"rules\":[";
    for (uint i = 0; i < 128; ++i)
        std::cout << (i ? "," : "") << c.reactorRules[i];
    std::cout << "],\"cases\":[";
    for (uint n = 0; n < 3; ++n) {
        State state = n ? warm(c) : State{};
        if (n == 2)
            for (auto &word : state.words)
                word = ~0u;
        uint tick = n ? 1800 : 0, rng = 12345 + n;
        std::cout << (n ? "," : "") << "{\"initial\":";
        words(state);
        std::cout << ",\"tick\":" << tick << ",\"rng\":" << rng << ",\"steps\":[";
        for (uint step = 0; step < 24; ++step) {
            uint mask = (step / 4) & 15u;
            tick += 6;
            state = prediction::reactorStep(c, state, tick, mask, rng);
            std::cout << (step ? "," : "") << "{\"mask\":" << mask << ",\"rng\":" << rng
                      << ",\"state\":";
            words(state);
            std::cout << '}';
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
int main(int argc, char **argv) {
    try {
        std::string backend = "gpu";
        double seconds = 18, warmup = 3, periodMs = 1000;
        uint workers = 4;
        bool validate = false, emitFixture = false;
        Parameters p;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            auto value = [&] {
                if (++i >= argc)
                    throw std::runtime_error("missing argument");
                return std::string(argv[i]);
            };
            if (key == "--backend")
                backend = value();
            else if (key == "--seconds")
                seconds = std::stod(value());
            else if (key == "--warmup")
                warmup = std::stod(value());
            else if (key == "--period-ms")
                periodMs = std::stod(value());
            else if (key == "--samples")
                p.samples = uint(std::stoul(value()));
            else if (key == "--steps")
                p.steps = uint(std::stoul(value()));
            else if (key == "--workers")
                workers = uint(std::stoul(value()));
            else if (key == "--validate")
                validate = true;
            else if (key == "--fixture")
                emitFixture = true;
            else
                throw std::runtime_error("unknown option: " + key);
        }
        if (seconds <= 0 || seconds > 120 || warmup < 0 || warmup > 30 || periodMs < 0 ||
            periodMs > 10000 || workers < 1 || workers > 64 || p.samples < 1 || p.samples > 32768 ||
            p.steps > 1000 ||
            (backend != "cpu" && backend != "gpu" && backend != "idle" && backend != "live"))
            throw std::runtime_error("invalid arguments");
        auto course = prediction::makeCourse();
        if (emitFixture) {
            fixture(course);
            return 0;
        }
        Gpu gpu(course, std::max(p.samples, 1024u),
                std::filesystem::absolute(argv[0]).parent_path() / "relay.spv");
        Pool pool(workers);
        auto initial = warm(course);
        std::vector<State> output(p.samples);
        if (validate) {
            uint cases = 0;
            for (uint count : {1u, 63u, 64u, 65u, 257u, 1024u})
                for (uint steps : {0u, 1u, 7u, 100u})
                    for (uint mask : {0u, 5u}) {
                        Parameters q{count, steps, mask, 1800, 12345 + cases};
                        std::vector<State> reference(count);
                        cpu(course, initial, q, reference, pool, workers);
                        gpu.run(initial, q);
                        auto actual = gpu.read(count);
                        if (std::memcmp(reference.data(), actual.data(), count * sizeof(State)))
                            throw std::runtime_error("CPU/GPU mismatch at " +
                                                     std::to_string(cases));
                        ++cases;
                    }
            std::cout << "PASS " << cases
                      << " bit-exact pure-reactor CPU/GPU cases (all 16 words/trial)\n";
            return 0;
        }
        auto live = initial;
        uint liveRng = 12345, liveTick = 1800;
        auto job = [&] {
            if (backend == "gpu")
                gpu.run(initial, p);
            else if (backend == "cpu")
                cpu(course, initial, p, output, pool, workers);
            else if (backend == "live") {
                liveTick += 6;
                live = prediction::reactorStep(course, live, liveTick, 0, liveRng);
            }
        };
        auto phase = [&](double duration, bool measured) {
            auto start = Clock::now(), end = start + std::chrono::duration_cast<Clock::duration>(
                                                         std::chrono::duration<double>(duration));
            uint jobs = 0, late = 0;
            double computeMs = 0, maxJobMs = 0;
            double interval = backend == "live" ? 100 : (backend == "idle" ? 100 : periodMs);
            while (Clock::now() < end) {
                auto before = Clock::now();
                if (backend != "idle")
                    job();
                double ms =
                    std::chrono::duration<double, std::milli>(Clock::now() - before).count();
                if (backend != "idle") {
                    ++jobs;
                    computeMs += ms;
                    maxJobMs = std::max(maxJobMs, ms);
                    if (interval > 0 && ms > interval)
                        ++late;
                }
                if (interval > 0)
                    std::this_thread::sleep_until(std::min(
                        end, before + std::chrono::duration_cast<Clock::duration>(
                                          std::chrono::duration<double, std::milli>(interval))));
            }
            double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            if (measured) {
                std::cout << std::setprecision(10) << "{\"event\":\"measure_end\",\"backend\":\""
                          << backend << "\",\"device\":\"" << gpu.name
                          << "\",\"seconds\":" << elapsed << ",\"jobs\":" << jobs
                          << ",\"samples\":" << (backend == "live" ? 1 : p.samples)
                          << ",\"steps\":" << (backend == "live" ? 1 : p.steps)
                          << ",\"workers\":" << workers << ",\"period_ms\":" << interval
                          << ",\"mean_job_ms\":" << (jobs ? computeMs / jobs : 0)
                          << ",\"max_job_ms\":" << maxJobMs << ",\"late_jobs\":" << late << "}\n"
                          << std::flush;
            }
        };
        phase(warmup, false);
        std::cout << "{\"event\":\"measure_start\"}\n" << std::flush;
        phase(seconds, true);
        // Explicitly outside the measurement interval: no bulk readback per job.
        if (backend == "gpu")
            output = gpu.read(p.samples);
        else if (backend == "live")
            output = {live};
        std::cout << "{\"event\":\"checksum\",\"value\":" << checksum(output) << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
