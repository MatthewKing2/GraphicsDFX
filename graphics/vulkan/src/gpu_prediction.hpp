#pragma once
#include "compute_context.hpp"
#include "prediction.hpp"
#include <memory>

// One outstanding request. Submit/poll on the render thread (VkQueue host access
// is externally synchronized there); GPU execution is asynchronous to that thread.
class GpuPrediction {
  public:
    GpuPrediction(ComputeContext, const prediction::Course &);
    ~GpuPrediction();
    bool busy() const;
    bool submit(const prediction::Request &);
    bool poll(prediction::Result &);

  private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
