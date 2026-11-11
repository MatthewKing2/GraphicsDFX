#pragma once
class Renderer;
struct GameControls {
    float strafe = 0, vertical = 0, mouseX = 0, mouseY = 0;
    bool jump = false, restart = false, pause = false, backend = false, guidance = false,
         stats = false;
    bool focused = true;
    // Additive first-person/console controls; existing tunnel controls unchanged.
    bool interact = false, sprint = false, primary = false, secondary = false;
    float cursorX = 0, cursorY = 0; // normalized window coordinates, top-left 0,0
};
class GameInput {
  public:
    explicit GameInput(Renderer &);
    ~GameInput();
    GameControls sample();
    void capture(bool);
    void title(const char *);

  private:
    class Impl;
    Impl *impl;
};
