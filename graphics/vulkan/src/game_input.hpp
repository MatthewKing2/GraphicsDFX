#pragma once
class Renderer;
struct GameControls {
    float strafe = 0, forward = 0, mouseX = 0, mouseY = 0;
    bool jump = false, restart = false, pause = false, backend = false, guidance = false, stats = false;
    bool focused = true;
};
class GameInput {
public:
    explicit GameInput(Renderer&);
    ~GameInput();
    GameControls sample();
    void capture(bool);
    void title(const char*);
private:
    class Impl;
    Impl* impl;
};
