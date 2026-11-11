#pragma once
#include <cstdint>
#include <memory>
#include <type_traits>

// Game/accelerator boundary. No Vulkan, GLFW, GLM, pointers or STL containers
// inside these records. Integer heat and bounded random walks are intentional.
// These are logical records, NOT a promised raw PCIe wire format: encode active
// prefixes, explicit little-endian fields and a version when XDMA is added.
namespace plant {
constexpr uint32_t Version = 1, MaxRods = 32, MaxBanks = 8, MaxSegments = 256;
constexpr uint32_t MaxCells = MaxRods * MaxSegments;
struct Config {
  uint32_t reactors = 2, rodsPerReactor = 6, segments = 128;
  uint32_t banks = 4, plugsPerBank = 4, capacityGW = 100;
  uint32_t demandPeriod = 420, demandPhaseStride = 105;
  uint32_t demandMinPercent = 25, demandMaxPercent = 95;
  uint32_t baseHeat = 4, nominalHeat = 16, nominalLoadPercent = 60;
  uint32_t hotHeat = 18, runawayHeat = 25, criticalHeat = 31;
  uint32_t diffusion256 = 96, ambientHeating256 = 96;
  uint32_t quietCooling256 = 64, loadedCooling256 = 36, runawayHeating256 = 48;
  uint32_t historiesPerRod = 8, walkSteps = 24, generations = 2;
  uint32_t rodPitch = 4, fuelWidth = 2, fuelAbsorb256 = 12,
           moderatorAbsorb256 = 3;
  uint32_t fissionBase256 = 6, fissionLoad256 = 18, hotFeedback256 = 1;
  uint32_t sourceStrength = 8, heatCapacity = 8, heatPerFission = 2;
  uint32_t seed = 1741;
};
struct Cell {
  uint16_t heatQ6 = 0; // temperature in 1/64 game units, not engineering units
  uint8_t runaway = 0, activity = 0;
  uint32_t rng = 1;
};
struct State {
  uint32_t version = Version, tick = 0, revision = 0;
  uint16_t sockets[MaxRods]{}; // bank * plugsPerBank + plug
  Cell cells[MaxCells]{};      // [global rod][axial segment], no gaps
};
struct Panel {
  uint32_t tick = 0, revision = 0, rods = 0, segments = 0;
  uint16_t sockets[MaxRods]{}, heatQ6[MaxCells]{};
  uint16_t rodGW[MaxRods]{}, bankDemandGW[MaxBanks]{},
      bankDeliveredGW[MaxBanks]{};
  uint8_t runaway[MaxRods]{};
};
struct ForecastRequest {
  uint32_t trials = 64, horizonTicks = 60, seed = 8128;
};
struct Forecast {
  uint32_t tick = 0, revision = 0, trials = 0, horizonTicks = 0;
  uint32_t hits[MaxRods]{}; // count trials with ANY critical cell, not summed
                            // cell risk
};
static_assert(sizeof(Cell) == 8 && std::is_trivially_copyable<State>::value,
              "state layout changed");

void validate(const Config &);
uint32_t rodCount(const Config &);
State initialState(const Config &, uint32_t seed = 2026);

// A backend owns its evolving state. advance/reroute are small COMMANDS;
// readPanel is a small-rate display readback, NOT a per-render-frame full-state
// transfer. Later a GPU/FPGA backend can retain its state on-device behind this
// exact API.
class Backend {
public:
  virtual ~Backend() = default;
  virtual const char *name() const = 0;
  virtual void reset(const Config &, const State &) = 0;
  virtual void advance(uint32_t ticks) = 0;
  virtual bool reroute(uint32_t rod, uint32_t bank) = 0;
  virtual void readPanel(Panel &) const = 0;
  virtual void forecast(const ForecastRequest &, Forecast &) const = 0;
  virtual State save() const = 0; // explicit mode-transition/checkpoint only
};
// Defined by ONE source file selected in the Makefile. No fake FPGA fallback.
std::unique_ptr<Backend> createBackend();
bool selfTest();
} // namespace plant
