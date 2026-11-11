#include "plant_compute.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace plant {
namespace {
uint32_t next(uint32_t x) {
  x ^= x << 13;
  x ^= x >> 17;
  return x ^ (x << 5);
}
uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  return (x ^ (x >> 16)) | 1u;
}
struct Loads {
  uint32_t demand[MaxBanks]{}, delivered[MaxBanks]{}, output[MaxRods]{},
      operating[MaxRods]{};
};
Loads loads(const Config &c, const State &s) {
  Loads p;
  uint32_t count[MaxBanks]{};
  for (uint32_t r = 0; r < rodCount(c); ++r)
    ++count[s.sockets[r] / c.plugsPerBank];
  const uint32_t average = rodCount(c) * c.capacityGW / c.banks;
  for (uint32_t b = 0; b < c.banks; ++b) {
    uint32_t phase = (s.tick + b * c.demandPhaseStride + 45) % c.demandPeriod;
    uint32_t ramp = std::min(phase, c.demandPeriod - phase);
    p.demand[b] =
        average * c.demandMinPercent / 100 +
        uint32_t(uint64_t(average) * (c.demandMaxPercent - c.demandMinPercent) *
                 ramp * 2 / (100 * c.demandPeriod));
    p.delivered[b] = std::min(p.demand[b], count[b] * c.capacityGW);
  }
  for (uint32_t r = 0; r < rodCount(c); ++r) {
    const uint32_t b = s.sockets[r] / c.plugsPerBank;
    p.output[r] =
        std::min(c.capacityGW, (p.demand[b] + count[b] / 2) / count[b]);
    p.operating[r] =
        std::min(c.criticalHeat,
                 c.baseHeat + p.output[r] * (c.nominalHeat - c.baseHeat) * 100 /
                                  (c.capacityGW * c.nominalLoadPercent));
  }
  return p;
}
// Source histories and at most two children per fission. This is a fictional
// fuel/moderator map, identical in spirit to neutron.py, NOT reactor safety
// code. Walks couple neighbouring rods in the same chamber. No cross-reactor
// leakage.
void transport(const Config &c, const State &s, const Loads &p,
               std::array<uint32_t, MaxCells> &counts) {
  struct Walker {
    int x, y;
    uint32_t random, generation;
  };
  std::array<Walker, 32> stack{}; // bounded by validation (<= 4 generations)
  for (uint32_t r = 0; r < rodCount(c); ++r) {
    uint32_t hot = 0;
    for (uint32_t j = 1; j < c.segments; ++j)
      if (s.cells[r * c.segments + j].heatQ6 >
          s.cells[r * c.segments + hot].heatQ6)
        hot = j;
    for (uint32_t source = 0; source < c.historiesPerRod; ++source) {
      const uint32_t id = r * c.historiesPerRod + source;
      uint32_t rng = next(s.cells[r * c.segments + id % c.segments].rng ^
                          (id + 1) * 0x9e3779b9u ^ c.seed) |
                     1u;
      int radius = std::max(1u, c.segments * 8 / 100);
      int y = int((rng >> 8) % c.segments);
      if ((rng >> 24) < 64)
        y = std::clamp(int(hot) + int((rng >> 16) % (2 * radius + 1)) - radius,
                       0, int(c.segments) - 1);
      uint32_t pending = 1;
      stack[0] = {int(r % c.rodsPerReactor * c.rodPitch + rng % c.fuelWidth), y,
                  rng, 0};
      while (pending) {
        Walker w = stack[--pending];
        for (uint32_t step = 0; step < c.walkSteps; ++step) {
          w.random = next(w.random);
          switch (w.random & 3) {
          case 0:
            ++w.x;
            break;
          case 1:
            --w.x;
            break;
          case 2:
            ++w.y;
            break;
          default:
            --w.y;
            break;
          }
          if (w.x < 0 || w.x >= int(c.rodsPerReactor * c.rodPitch) || w.y < 0 ||
              w.y >= int(c.segments))
            break; // leaked
          uint32_t target = r / c.rodsPerReactor * c.rodsPerReactor +
                            uint32_t(w.x) / c.rodPitch;
          uint32_t node = target * c.segments + uint32_t(w.y);
          bool fuel = uint32_t(w.x) % c.rodPitch < c.fuelWidth;
          uint32_t absorption = fuel ? c.fuelAbsorb256 : c.moderatorAbsorb256;
          uint32_t rate =
              c.fissionBase256 + c.fissionLoad256 *
                                     (p.operating[target] - c.baseHeat) /
                                     (c.nominalHeat - c.baseHeat);
          rate += c.hotFeedback256 *
                  uint32_t(std::max(0, int(s.cells[node].heatQ6 / 64) -
                                           int(c.hotHeat)));
          rate = std::min(256 - absorption, rate);
          uint32_t draw = (w.random >> 8) & 255;
          if (draw < absorption)
            break;
          if (fuel && draw < absorption + rate) {
            ++counts[node];
            if (w.generation + 1 < c.generations) {
              stack[pending++] = {w.x, w.y, next(w.random ^ 0x85ebca6bu) | 1u,
                                  w.generation + 1};
              stack[pending++] = {w.x, w.y, next(w.random ^ 0xc2b2ae35u) | 1u,
                                  w.generation + 1};
            }
            break;
          }
        }
      }
    }
  }
}
void step(const Config &c, State &s) {
  const uint32_t n = rodCount(c) * c.segments;
  for (uint32_t i = 0; i < n; ++i)
    s.cells[i].rng = next(s.cells[i].rng);
  // Conservative exchanges on alternating non-overlapping axial pairs.
  for (uint32_t pass = 0; pass < 2; ++pass)
    for (uint32_t r = 0; r < rodCount(c); ++r)
      for (uint32_t j = (s.tick + pass) & 1; j + 1 < c.segments; j += 2) {
        auto &a = s.cells[r * c.segments + j];
        auto &b = s.cells[r * c.segments + j + 1];
        int difference = int(a.heatQ6 / 64) - int(b.heatQ6 / 64);
        int magnitude = (std::abs(difference) * int(c.diffusion256) +
                         int((a.rng >> (pass * 8)) & 255)) /
                        256;
        int flux = (difference < 0 ? -magnitude : magnitude) * 64;
        a.heatQ6 = uint16_t(int(a.heatQ6) - flux);
        b.heatQ6 = uint16_t(int(b.heatQ6) + flux);
      }
  const Loads p = loads(c, s);
  std::array<uint32_t, MaxCells> counts{};
  transport(c, s, p, counts);
  for (uint32_t i = 0; i < n; ++i) {
    auto &cell = s.cells[i];
    uint32_t h = cell.heatQ6 / 64, operating = p.operating[i / c.segments];
    bool runaway = cell.runaway || h >= c.runawayHeat;
    bool rise = h < operating && ((cell.rng >> 16) & 255) < c.ambientHeating256;
    bool fuse = runaway && ((cell.rng >> 8) & 255) < c.runawayHeating256;
    bool fall =
        h > operating && !runaway &&
        ((cell.rng >> 24) & 255) <
            (operating >= c.hotHeat ? c.loadedCooling256 : c.quietCooling256);
    // Normalize sample weights and preserve fractional heat. More samples
    // improve transport estimation, rather than creating extra physical heat.
    uint32_t dose =
        uint32_t(uint64_t(counts[i]) * c.heatPerFission * c.sourceStrength *
                 64 / (c.historiesPerRod * c.heatCapacity));
    int heat =
        int(cell.heatQ6) + int(dose) + (int(rise) + int(fuse) - int(fall)) * 64;
    cell.heatQ6 = uint16_t(std::clamp(heat, 0, int(c.criticalHeat * 64)));
    cell.runaway = runaway || cell.heatQ6 >= c.runawayHeat * 64;
    cell.activity = counts[i] != 0;
  }
  ++s.tick;
}
class CpuBackend final : public Backend {
  Config config;
  State state;

public:
  const char *name() const override { return "CPU"; }
  void reset(const Config &c, const State &s) override {
    validate(c);
    if (s.version != Version)
      throw std::invalid_argument("plant snapshot version");
    bool used[MaxBanks * MaxRods]{};
    for (uint32_t r = 0; r < rodCount(c); ++r) {
      uint32_t plug = s.sockets[r];
      if (plug >= c.banks * c.plugsPerBank || used[plug])
        throw std::invalid_argument("invalid sockets");
      used[plug] = true;
    }
    for (uint32_t i = 0; i < rodCount(c) * c.segments; ++i)
      if (s.cells[i].heatQ6 > c.criticalHeat * 64 || !s.cells[i].rng)
        throw std::invalid_argument("invalid thermal snapshot");
    config = c;
    state = s;
  }
  void advance(uint32_t ticks) override {
    while (ticks--)
      step(config, state);
  }
  bool reroute(uint32_t rod, uint32_t bank) override {
    if (rod >= rodCount(config) || bank >= config.banks)
      return false;
    if (state.sockets[rod] / config.plugsPerBank == bank)
      return true;
    for (uint32_t plug = bank * config.plugsPerBank;
         plug < (bank + 1) * config.plugsPerBank; ++plug) {
      bool used = false;
      for (uint32_t r = 0; r < rodCount(config); ++r)
        used |= state.sockets[r] == plug;
      if (!used) {
        state.sockets[rod] = uint16_t(plug);
        ++state.revision;
        return true;
      }
    }
    return false;
  }
  void readPanel(Panel &panel) const override {
    panel = {};
    panel.tick = state.tick;
    panel.revision = state.revision;
    panel.rods = rodCount(config);
    panel.segments = config.segments;
    const auto p = loads(config, state);
    for (uint32_t r = 0; r < panel.rods; ++r) {
      panel.sockets[r] = state.sockets[r];
      panel.rodGW[r] = uint16_t(p.output[r]);
      for (uint32_t j = 0; j < config.segments; ++j) {
        uint32_t i = r * config.segments + j;
        panel.heatQ6[i] = state.cells[i].heatQ6;
        panel.runaway[r] |= state.cells[i].runaway;
      }
    }
    for (uint32_t b = 0; b < config.banks; ++b) {
      panel.bankDemandGW[b] = uint16_t(p.demand[b]);
      panel.bankDeliveredGW[b] = uint16_t(p.delivered[b]);
    }
  }
  void forecast(const ForecastRequest &request, Forecast &out) const override {
    if (!request.trials || request.trials > 4096 || request.horizonTicks > 600)
      throw std::invalid_argument("forecast bounds");
    out = {};
    out.tick = state.tick;
    out.revision = state.revision;
    out.trials = request.trials;
    out.horizonTicks = request.horizonTicks;
    for (uint32_t trial = 0; trial < request.trials; ++trial) {
      State future = state;
      bool failed[MaxRods]{};
      for (uint32_t i = 0; i < rodCount(config) * config.segments; ++i)
        future.cells[i].rng =
            mix(request.seed ^ ((trial * MaxCells + i + 1) * 0x9e3779b9u));
      for (uint32_t t = 0; t <= request.horizonTicks; ++t) {
        for (uint32_t i = 0; i < rodCount(config) * config.segments; ++i)
          failed[i / config.segments] |=
              future.cells[i].heatQ6 >= config.criticalHeat * 64;
        if (t < request.horizonTicks)
          step(config, future);
      }
      for (uint32_t r = 0; r < rodCount(config); ++r)
        out.hits[r] += failed[r];
    }
  }
  State save() const override { return state; }
};
} // namespace
std::unique_ptr<Backend> createBackend() {
  return std::make_unique<CpuBackend>();
}
} // namespace plant
