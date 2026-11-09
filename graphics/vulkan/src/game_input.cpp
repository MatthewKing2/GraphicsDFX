#include "game_input.hpp"
#include "renderer.hpp"
#include <GLFW/glfw3.h>
#include <array>
#include <stdexcept>

class GameInput::Impl {
public:
    GLFWwindow* window;
    std::array<bool,GLFW_KEY_LAST+1> previous{};
    bool captured = false, first = true;
    double lastX=0,lastY=0;
    float dx=0,dy=0;
    inline static Impl* active=nullptr;
    explicit Impl(void* handle):window(static_cast<GLFWwindow*>(handle)) {
        if (active) throw std::runtime_error("one game input controller supported");
        active=this;
        glfwSetCursorPosCallback(window,[](GLFWwindow*,double x,double y) {
            auto* p=active;
            if (!p || !p->captured) return;
            if (!p->first) {p->dx+=float(x-p->lastX); p->dy+=float(y-p->lastY);}
            p->first=false; p->lastX=x; p->lastY=y;
        });
    }
    bool down(int key) const {return glfwGetKey(window,key)==GLFW_PRESS;}
    bool pressed(int key) {bool value=down(key),edge=value&&!previous[key]; previous[key]=value; return edge;}
};
GameInput::GameInput(Renderer& renderer):impl(new Impl(renderer.nativeWindowHandle())) {capture(true);}
GameInput::~GameInput() {
    capture(false); glfwSetCursorPosCallback(impl->window,nullptr); Impl::active=nullptr; delete impl;
}
void GameInput::capture(bool enabled) {
    if (impl->captured==enabled) return;
    impl->captured=enabled; impl->first=true; impl->dx=impl->dy=0;
    glfwSetInputMode(impl->window,GLFW_CURSOR,enabled?GLFW_CURSOR_DISABLED:GLFW_CURSOR_NORMAL);
    if(glfwRawMouseMotionSupported()) glfwSetInputMode(impl->window,GLFW_RAW_MOUSE_MOTION,enabled?GLFW_TRUE:GLFW_FALSE);
}
void GameInput::title(const char* text) {glfwSetWindowTitle(impl->window,text);}
GameControls GameInput::sample() {
    GameControls c;
    c.focused=glfwGetWindowAttrib(impl->window,GLFW_FOCUSED)!=0;
    c.strafe=float(impl->down(GLFW_KEY_D))-float(impl->down(GLFW_KEY_A));
    c.forward=float(impl->down(GLFW_KEY_W))-float(impl->down(GLFW_KEY_S));
    c.mouseX=impl->dx; c.mouseY=impl->dy; impl->dx=impl->dy=0;
    c.jump=impl->pressed(GLFW_KEY_SPACE); c.restart=impl->pressed(GLFW_KEY_R)||impl->pressed(GLFW_KEY_ENTER);
    c.pause=impl->pressed(GLFW_KEY_ESCAPE); c.backend=impl->pressed(GLFW_KEY_F1);
    c.guidance=impl->pressed(GLFW_KEY_F2); c.stats=impl->pressed(GLFW_KEY_F3);
    return c;
}
