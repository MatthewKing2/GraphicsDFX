#include "camera.hpp"

#include "renderer.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

class CameraController::Impl {
public:
    Impl(
        Renderer& renderer,
        const glm::vec3& initialPosition,
        const glm::vec3& initialTarget,
        const glm::vec3& requestedWorldUp)
        : window(static_cast<GLFWwindow*>(renderer.nativeWindowHandle())),
          worldUp(requestedWorldUp) {
        if (window == nullptr) {
            throw std::runtime_error("cannot attach a camera to a null window");
        }

        const glm::vec3 initialDirection = initialTarget - initialPosition;
        if (glm::length(initialDirection) < 0.0001f || glm::length(worldUp) < 0.0001f) {
            throw std::runtime_error("camera direction and world-up must be non-zero");
        }
        worldUp = glm::normalize(worldUp);
        const glm::vec3 initialForward = glm::normalize(initialDirection);
        const float verticalComponent = std::clamp(glm::dot(initialForward, worldUp), -1.0f, 1.0f);

        pitchDegrees = glm::degrees(std::asin(verticalComponent));
        referenceForward = initialForward - verticalComponent * worldUp;
        if (glm::length(referenceForward) < 0.0001f) {
            throw std::runtime_error("initial camera direction cannot be parallel to world-up");
        }
        referenceForward = glm::normalize(referenceForward);
        referenceRight = glm::normalize(glm::cross(referenceForward, worldUp));

        camera.position = initialPosition;
        camera.up = worldUp;
        camera.verticalFieldOfViewDegrees = 45.0f;
        updateForward();

        controllers.emplace(window, this);
        glfwSetCursorPosCallback(window, cursorPositionCallback);
        glfwSetScrollCallback(window, scrollCallback);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
    }

    ~Impl() {
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        }
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetCursorPosCallback(window, nullptr);
        glfwSetScrollCallback(window, nullptr);
        controllers.erase(window);
    }

    void update(float deltaSeconds) {
        // A breakpoint or dragged window can produce a huge frame time. Capping
        // it prevents one delayed frame from teleporting the camera.
        deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.1f);

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        yawDegrees += pendingMouseX * lookSensitivity;
        pitchDegrees += pendingMouseY * lookSensitivity;
        pitchDegrees = std::clamp(pitchDegrees, -89.0f, 89.0f);
        pendingMouseX = 0.0f;
        pendingMouseY = 0.0f;
        updateForward();

        camera.verticalFieldOfViewDegrees -= pendingScrollY * zoomSensitivity;
        camera.verticalFieldOfViewDegrees =
            std::clamp(camera.verticalFieldOfViewDegrees, 15.0f, 90.0f);
        pendingScrollY = 0.0f;

        const float step = moveSpeed * deltaSeconds;
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
            camera.position += camera.forward * step;
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
            camera.position -= camera.forward * step;
        }

        const glm::vec3 right = glm::normalize(glm::cross(camera.forward, worldUp));
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
            camera.position += right * step;
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
            camera.position -= right * step;
        }
    }

    const CameraState& state() const {
        return camera;
    }

    void setMoveSpeed(float value) {
        moveSpeed = std::max(value, 0.0f);
    }

    void setLookSensitivity(float value) {
        lookSensitivity = std::max(value, 0.0f);
    }

    void setZoomSensitivity(float value) {
        zoomSensitivity = std::max(value, 0.0f);
    }

private:
    inline static std::unordered_map<GLFWwindow*, Impl*> controllers;

    GLFWwindow* window;
    CameraState camera{};
    glm::vec3 worldUp;
    glm::vec3 referenceForward;
    glm::vec3 referenceRight;

    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
    float moveSpeed = 2.5f;
    float lookSensitivity = 0.1f;
    float zoomSensitivity = 2.0f;
    float pendingMouseX = 0.0f;
    float pendingMouseY = 0.0f;
    float pendingScrollY = 0.0f;
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;
    bool receivedFirstMousePosition = false;

    void updateForward() {
        const float yaw = glm::radians(yawDegrees);
        const float pitch = glm::radians(pitchDegrees);
        const glm::vec3 horizontalDirection =
            std::cos(yaw) * referenceForward + std::sin(yaw) * referenceRight;
        camera.forward = glm::normalize(
            std::cos(pitch) * horizontalDirection + std::sin(pitch) * worldUp);
    }

    static Impl* controllerFor(GLFWwindow* callbackWindow) {
        const auto entry = controllers.find(callbackWindow);
        return entry == controllers.end() ? nullptr : entry->second;
    }

    static void cursorPositionCallback(GLFWwindow* callbackWindow, double x, double y) {
        Impl* controller = controllerFor(callbackWindow);
        if (controller == nullptr) {
            return;
        }

        if (!controller->receivedFirstMousePosition) {
            controller->lastMouseX = x;
            controller->lastMouseY = y;
            controller->receivedFirstMousePosition = true;
            return;
        }

        controller->pendingMouseX += static_cast<float>(x - controller->lastMouseX);
        controller->pendingMouseY += static_cast<float>(controller->lastMouseY - y);
        controller->lastMouseX = x;
        controller->lastMouseY = y;
    }

    static void scrollCallback(GLFWwindow* callbackWindow, double, double yOffset) {
        Impl* controller = controllerFor(callbackWindow);
        if (controller != nullptr) {
            controller->pendingScrollY += static_cast<float>(yOffset);
        }
    }
};

CameraController::CameraController(
    Renderer& renderer,
    const glm::vec3& initialPosition,
    const glm::vec3& initialTarget,
    const glm::vec3& worldUp)
    : impl(std::make_unique<Impl>(renderer, initialPosition, initialTarget, worldUp)) {
}

CameraController::~CameraController() = default;

void CameraController::update(float deltaSeconds) {
    impl->update(deltaSeconds);
}

const CameraState& CameraController::state() const {
    return impl->state();
}

void CameraController::setMoveSpeed(float unitsPerSecond) {
    impl->setMoveSpeed(unitsPerSecond);
}

void CameraController::setLookSensitivity(float degreesPerPixel) {
    impl->setLookSensitivity(degreesPerPixel);
}

void CameraController::setZoomSensitivity(float degreesPerScrollStep) {
    impl->setZoomSensitivity(degreesPerScrollStep);
}
