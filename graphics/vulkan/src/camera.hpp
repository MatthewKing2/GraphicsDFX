#pragma once

#include <glm/glm.hpp>

#include <memory>

class Renderer;

// Everything the application needs to build view and projection matrices.
// This contains no Vulkan or GLFW types.
struct CameraState {
    glm::vec3 position;
    glm::vec3 forward;
    glm::vec3 up;
    float verticalFieldOfViewDegrees;
};

class CameraController {
public:
    // Controls: W/S move forward/back, A/D strafe, mouse looks, scroll zooms,
    // and Escape closes the window.
    CameraController(
        Renderer& renderer,
        const glm::vec3& initialPosition,
        const glm::vec3& initialTarget,
        const glm::vec3& worldUp = glm::vec3(0.0f, 0.0f, 1.0f));
    ~CameraController();

    CameraController(const CameraController&) = delete;
    CameraController& operator=(const CameraController&) = delete;

    // Call once per frame, after Renderer::pollEvents().
    void update(float deltaSeconds);

    const CameraState& state() const;

    // Units are world-units/second, degrees/pixel, and degrees/scroll-step.
    void setMoveSpeed(float unitsPerSecond);
    void setLookSensitivity(float degreesPerPixel);
    void setZoomSensitivity(float degreesPerScrollStep);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
