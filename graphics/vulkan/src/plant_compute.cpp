#include "plant_compute.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace plant {
namespace {
uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  return (x ^ (x >> 16)) | 1u;
}
} // namespace
uint32_t rodCount(const Config &c) { return c.reactors * c.rodsPerReactor; }
void validate(const Config &c) {
  if (!c.reactors || !c.rodsPerReactor || c.reactors > MaxRods ||
      c.rodsPerReactor > MaxRods || rodCount(c) > MaxRods || !c.banks ||
      c.banks > MaxBanks || !c.plugsPerBank || c.plugsPerBank > MaxRods ||
      c.banks * c.plugsPerBank < rodCount(c) || c.segments < 2 ||
      c.segments > MaxSegments || c.capacityGW == 0 || c.capacityGW > 1000 ||
      c.demandPeriod < 2 || c.demandMinPercent > c.demandMaxPercent ||
      c.demandMaxPercent > 200 || c.baseHeat >= c.nominalHeat ||
      c.nominalHeat >= c.runawayHeat || c.hotHeat > c.runawayHeat ||
      !c.nominalLoadPercent || c.nominalLoadPercent > 100 ||
      c.runawayHeat >= c.criticalHeat || c.criticalHeat > 255 ||
      c.diffusion256 > 128 || c.ambientHeating256 > 256 ||
      c.quietCooling256 > 256 || c.loadedCooling256 > 256 ||
      c.runawayHeating256 > 256 || !c.historiesPerRod ||
      c.historiesPerRod > 128 || !c.walkSteps || c.walkSteps > 256 ||
      !c.generations || c.generations > 4 || !c.rodPitch || c.rodPitch > 16 ||
      !c.fuelWidth || c.fuelWidth > c.rodPitch || c.fuelAbsorb256 > 256 ||
      c.moderatorAbsorb256 > 256 || !c.heatCapacity || c.heatCapacity > 1024 ||
      c.sourceStrength > 1024 || c.heatPerFission > 1024 ||
      c.hotFeedback256 > 256 || c.fissionBase256 > 256 ||
      c.fissionLoad256 > 256)
    throw std::invalid_argument("invalid plant config");
}
State initialState(const Config &c, uint32_t seed) {
  validate(c);
  State s;
  for (uint32_t r = 0; r < rodCount(c); ++r) {
    // General balanced placement, including non-divisible rod/bank counts.
    s.sockets[r] = uint16_t((r % c.banks) * c.plugsPerBank + r / c.banks);
    for (uint32_t j = 0; j < c.segments; ++j) {
      auto &cell = s.cells[r * c.segments + j];
      cell.rng = mix(seed ^ ((r * c.segments + j + 1) * 0x9e3779b9u));
      uint32_t base = std::min(c.runawayHeat - 1, c.baseHeat + 6 + r % 6);
      uint32_t peak = std::min(c.runawayHeat - 1, r == 0   ? 21u
                                                  : r == 1 ? 23u
                                                           : 15 + r % 5);
      peak = std::max(base, peak);
      int width = std::max(1u, c.segments * 14 / 100);
      int patch = std::max(
          0, width - std::abs(int(j) - int(c.segments * (r % 3 + 1) / 4)));
      cell.heatQ6 = uint16_t(
          (base + (peak - base) * uint32_t(patch) / uint32_t(width)) * 64);
    }
  }
  return s;
}
bool selfTest() {
  auto require = [](bool value, const char *why) {
    if (!value)
      throw std::runtime_error(why);
  };
  Config c;
  auto a = createBackend(), b = createBackend();
  auto original = initialState(c);
  a->reset(c, original);
  b->reset(c, original);
  a->advance(30);
  for (int i = 0; i < 30; ++i)
    b->advance(1);
  auto x = a->save(), y = b->save();
  require(x.tick == 30 && y.tick == 30, "fixed tick");
  for (uint32_t i = 0; i < rodCount(c) * c.segments; ++i) {
    require(x.cells[i].heatQ6 == y.cells[i].heatQ6 &&
                x.cells[i].rng == y.cells[i].rng,
            "batch determinism");
  }
  b->reset(c, original);
  b->advance(5);
  auto early = b->save();
  for (uint32_t i = 0; i < rodCount(c) * c.segments; ++i)
    require(early.cells[i].heatQ6 < c.criticalHeat * 64,
            "unexpected instant meltdown");
  Forecast f, g;
  auto before = a->save();
  a->forecast({8, 10, 123}, f);
  a->forecast({8, 10, 123}, g);
  require(a->save().tick == before.tick, "prediction advanced live time");
  auto after = a->save();
  for (uint32_t i = 0; i < rodCount(c) * c.segments; ++i)
    require(before.cells[i].heatQ6 == after.cells[i].heatQ6 &&
                before.cells[i].rng == after.cells[i].rng &&
                before.cells[i].runaway == after.cells[i].runaway,
            "forecast mutated live state");
  b->reset(c, before);
  b->advance(1);
  a->advance(1);
  auto restored = b->save(), continued = a->save();
  for (uint32_t i = 0; i < rodCount(c) * c.segments; ++i)
    require(restored.cells[i].heatQ6 == continued.cells[i].heatQ6 &&
                restored.cells[i].rng == continued.cells[i].rng,
            "checkpoint restore diverged");
  for (uint32_t r = 0; r < rodCount(c); ++r)
    require(f.hits[r] == g.hits[r] && f.hits[r] <= f.trials,
            "forecast reproducibility");
  require(a->reroute(0, 1), "free bank slot");
  Panel panel;
  a->readPanel(panel);
  require(panel.revision == 1 && panel.sockets[0] / c.plugsPerBank == 1,
          "routing revision");
  require(!a->reroute(999, 0) && !a->reroute(0, 999), "invalid routing");
  // Four rods per bank: no spare socket. Rejection must not steal a connection.
  Config full = c;
  full.plugsPerBank = 3;
  b->reset(full, initialState(full));
  require(!b->reroute(0, 1), "occupied bank rejection");
  // A pre-existing critical cell must count as one rod hit in EVERY trial.
  auto critical = original;
  critical.cells[0].heatQ6 = uint16_t(c.criticalHeat * 64);
  b->reset(c, critical);
  b->forecast({4, 1, 9}, f);
  require(f.hits[0] == 4, "critical union");
  bool rejected = false;
  try {
    auto invalid = c;
    invalid.segments = 0;
    validate(invalid);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "config validation");
  std::cout << "Plant tests passed: bounded transport, no instant runaway, "
               "deterministic ticks/forecasts, routing, critical union.\n";
  return true;
}
} // namespace plant
