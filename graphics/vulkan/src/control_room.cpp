#include "control_room.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace controlroom {
namespace {
using V = glm::vec3;
constexpr float PanelZ = -3.67f, PanelWidth = 6.4f, PanelHeight = 2.12f;
const V copper(.34f, .16f, .065f), steel(.12f, .15f, .14f),
    amber(.95f, .43f, .08f), mint(.38f, .9f, .59f), dim(.11f, .3f, .23f),
    danger(.98f, .1f, .04f);
void box(std::vector<SceneInstance> &s, V p, V size, V color, float mode = 4) {
  SceneInstance i;
  i.transform = glm::scale(glm::translate(glm::mat4(1), p), size);
  i.tint = glm::vec4(color, mode);
  s.push_back(i);
}
void beam(std::vector<SceneInstance> &s, V a, V b, float width, V color,
          float mode = 4) {
  V axis = b - a;
  if (glm::length(axis) < .0001f)
    return;
  V y = glm::normalize(axis),
    x = glm::normalize(
        glm::cross(y, std::abs(y.z) < .9f ? V(0, 0, 1) : V(1, 0, 0)));
  SceneInstance i;
  i.transform[0] = glm::vec4(x * width, 0);
  i.transform[1] = glm::vec4(axis, 0);
  i.transform[2] = glm::vec4(glm::cross(x, y) * width, 0);
  i.transform[3] = glm::vec4((a + b) * .5f, 1);
  i.tint = glm::vec4(color, mode);
  s.push_back(i);
}
void hoop(std::vector<SceneInstance> &s, V center, float radius, V color,
          bool vertical = true) {
  for (int k = 0; k < 8; ++k) {
    float a = k * glm::pi<float>() / 4, b = (k + 1) * glm::pi<float>() / 4;
    V p(std::cos(a), vertical ? std::sin(a) : 0, vertical ? 0 : std::sin(a));
    V q(std::cos(b), vertical ? std::sin(b) : 0, vertical ? 0 : std::sin(b));
    beam(s, center + p * radius, center + q * radius, .045f, color);
  }
}
std::array<uint8_t, 7> glyph(char c) {
  switch (c) {
  case 'A':
    return {14, 17, 17, 31, 17, 17, 17};
  case 'B':
    return {30, 17, 17, 30, 17, 17, 30};
  case 'C':
    return {14, 17, 16, 16, 16, 17, 14};
  case 'D':
    return {30, 17, 17, 17, 17, 17, 30};
  case 'E':
    return {31, 16, 16, 30, 16, 16, 31};
  case 'F':
    return {31, 16, 16, 30, 16, 16, 16};
  case 'G':
    return {14, 17, 16, 23, 17, 17, 15};
  case 'H':
    return {17, 17, 17, 31, 17, 17, 17};
  case 'I':
    return {31, 4, 4, 4, 4, 4, 31};
  case 'J':
    return {7, 2, 2, 2, 18, 18, 12};
  case 'K':
    return {17, 18, 20, 24, 20, 18, 17};
  case 'L':
    return {16, 16, 16, 16, 16, 16, 31};
  case 'M':
    return {17, 27, 21, 21, 17, 17, 17};
  case 'N':
    return {17, 25, 21, 19, 17, 17, 17};
  case 'O':
    return {14, 17, 17, 17, 17, 17, 14};
  case 'P':
    return {30, 17, 17, 30, 16, 16, 16};
  case 'Q':
    return {14, 17, 17, 17, 21, 18, 13};
  case 'R':
    return {30, 17, 17, 30, 20, 18, 17};
  case 'S':
    return {15, 16, 16, 14, 1, 1, 30};
  case 'T':
    return {31, 4, 4, 4, 4, 4, 4};
  case 'U':
    return {17, 17, 17, 17, 17, 17, 14};
  case 'V':
    return {17, 17, 17, 17, 17, 10, 4};
  case 'W':
    return {17, 17, 17, 21, 21, 21, 10};
  case 'X':
    return {17, 17, 10, 4, 10, 17, 17};
  case 'Y':
    return {17, 17, 10, 4, 4, 4, 4};
  case 'Z':
    return {31, 1, 2, 4, 8, 16, 31};
  case '0':
    return {14, 17, 19, 21, 25, 17, 14};
  case '1':
    return {4, 12, 4, 4, 4, 4, 14};
  case '2':
    return {14, 17, 1, 2, 4, 8, 31};
  case '3':
    return {30, 1, 1, 14, 1, 1, 30};
  case '4':
    return {2, 6, 10, 18, 31, 2, 2};
  case '5':
    return {31, 16, 16, 30, 1, 1, 30};
  case '6':
    return {14, 16, 16, 30, 17, 17, 14};
  case '7':
    return {31, 1, 2, 4, 8, 8, 8};
  case '8':
    return {14, 17, 17, 14, 17, 17, 14};
  case '9':
    return {14, 17, 17, 15, 1, 1, 14};
  case '%':
    return {25, 25, 2, 4, 8, 19, 19};
  case '-':
    return {0, 0, 0, 31, 0, 0, 0};
  case ':':
    return {0, 4, 4, 0, 4, 4, 0};
  case '/':
    return {1, 2, 2, 4, 8, 8, 16};
  case '.':
    return {0, 0, 0, 0, 0, 4, 4};
  default:
    return {};
  }
}
V panelPoint(float x, float y, float depth = 0) {
  return {(x / 1000 - .5f) * PanelWidth, 3.17f - y / 500 * PanelHeight,
          PanelZ + depth};
}
void rect(std::vector<SceneInstance> &s, float x, float y, float w, float h,
          V c, float depth = .04f) {
  box(s, panelPoint(x + w * .5f, y + h * .5f, depth),
      {w / 1000 * PanelWidth, h / 500 * PanelHeight, .002f}, c, 3);
}
void text(std::vector<SceneInstance> &s, float x, float y,
          const std::string &value, float scale, V color, float depth = .07f) {
  for (char ch : value) {
    auto rows = glyph(ch);
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col)
        if (rows[row] & (1 << (4 - col)))
          rect(s, x + col * scale, y + row * scale * 1.4f, scale * .83f,
               scale * .83f * 1.4f, color, depth);
    x += 6 * scale;
  }
}
void label(std::vector<SceneInstance> &s, V topLeft, const std::string &str,
           float size, V color) {
  for (char c : str) {
    auto rows = glyph(c);
    for (int r = 0; r < 7; ++r)
      for (int k = 0; k < 5; ++k)
        if (rows[r] & (1 << (4 - k)))
          box(s, topLeft + V(k * size, -r * size, 0),
              {size * .8f, size * .8f, .008f}, color, 3);
    topLeft.x += size * 6;
  }
}
V rodColor(uint32_t r) {
  const V colors[] = {
      {.2f, .65f, .98f},  {.95f, .55f, .18f}, {.62f, .4f, .98f},
      {.25f, .87f, .51f}, {.96f, .34f, .52f}, {.85f, .86f, .28f},
      {.28f, .9f, .88f},  {.86f, .51f, .91f}, {.64f, .8f, .95f},
      {.9f, .68f, .5f},   {.5f, .95f, .63f},  {.8f, .5f, .36f}};
  return colors[r % 12];
}
V heatColor(float h) {
  if (h < .42f)
    return glm::mix(V(.035f, .16f, .13f), V(.2f, .65f, .32f), h / .42f);
  if (h < .77f)
    return glm::mix(V(.2f, .65f, .32f), amber, (h - .42f) / .35f);
  return glm::mix(amber, danger, std::clamp((h - .77f) / .23f, 0.0f, 1.0f));
}
glm::vec2 rodLocation(const plant::Config &c, uint32_t rod) {
  float column = float(rod % c.rodsPerReactor);
  return {50 + (column + .5f) * (510.0f / c.rodsPerReactor),
          100 + (rod / c.rodsPerReactor) * (270.0f / c.reactors)};
}
float rodHeight(const plant::Config &c) { return 270.0f / c.reactors - 58; }
float bankY(const plant::Config &c, uint32_t bank) {
  return 104 + bank * (287.0f / c.banks);
}
bool inRect(glm::vec2 p, float x, float y, float w, float h) {
  return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h;
}
} // namespace
Mesh cubeMesh() {
  Mesh mesh;
  const V corners[] = {{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, .5f, -.5f},
                       {-.5f, .5f, -.5f},  {-.5f, -.5f, .5f}, {.5f, -.5f, .5f},
                       {.5f, .5f, .5f},    {-.5f, .5f, .5f}};
  const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                           {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
  const float shade[] = {.45f, 1, .8f, .9f, .6f, .7f};
  for (int f = 0; f < 6; ++f) {
    uint32_t base = uint32_t(mesh.vertices.size());
    for (int v : faces[f])
      mesh.vertices.push_back({corners[v], V(shade[f]), {}});
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
      mesh.indices.push_back(base + i);
  }
  return mesh;
}
Room::Room(Settings s) : settings(s) { buildRoom(); }
void Room::buildRoom() {
  auto &s = architecture;
  s.reserve(8000);
  box(s, {0, -.15f, 0}, {10, .3f, 10}, {.12f, .13f, .11f});
  box(s, {0, 3.7f, 0}, {10, .3f, 10}, {.075f, .09f, .08f});
  box(s, {0, 1.8f, -4}, {10, 3.6f, .3f}, {.16f, .19f, .16f});
  box(s, {0, 1.8f, 5}, {10, 3.6f, .3f}, {.13f, .15f, .14f});
  for (int side : {-1, 1})
    box(s, {side * 5.0f, 1.8f, 0}, {.3f, 3.6f, 10}, {.12f, .14f, .12f});
  // Mismatched steel plates, panel seams, bolts and rust: all cheap instancing.
  for (int x = -4; x <= 4; ++x)
    for (int z = -4; z < 5; ++z) {
      float variation = float((x * 17 + z * 13 + 200) % 7) * .009f;
      box(s, {x + .5f, .009f, z + .5f}, {.965f, .015f, .965f},
          V(.14f + variation, .15f + variation, .13f + variation));
      for (float dx : {-.42f, .42f})
        box(s, {x + .5f + dx, .025f, z + .08f}, {.035f, .025f, .035f},
            copper * .6f);
      // Grating slots and pools of grime; no texture assets or dependencies.
      if ((x + z + 20) % 4 == 0)
        for (int k = 0; k < 7; ++k)
          box(s, {x + .5f, .027f, z + .21f + k * .085f}, {.65f, .01f, .025f},
              {.035f, .04f, .032f});
      if ((x * 5 + z * 3 + 100) % 6 == 0)
        box(s, {x + .55f, .029f, z + .53f}, {.37f, .008f, .26f},
            {.06f, .048f, .025f});
    }
  for (int x = -4; x <= 4; ++x) {
    box(s, {float(x), 1.8f, -3.81f}, {.055f, 3.5f, .05f}, steel * .45f);
    for (int y = 0; y < 4; ++y)
      for (float dx : {-.075f, .075f})
        box(s, {x + dx, .18f + y * .87f, -3.77f}, {.03f, .03f, .035f}, copper);
    // Repeated ribs over the ceiling and down the side walls.
    box(s, {0, 3.48f, float(x)}, {9.8f, .16f, .14f}, steel);
    for (int side : {-1, 1})
      box(s, {side * 4.8f, 1.8f, float(x)}, {.18f, 3.4f, .14f}, steel);
  }
  // Dense copper/iron pipework, octagonal flanges and red hand wheels.
  for (int k = 0; k < 5; ++k) {
    float x = -4.55f + k * .27f;
    beam(s, {x, 3.2f, -3.7f}, {x, 3.2f, 4.7f}, .16f, k % 2 ? steel : copper);
    for (int z = -3; z < 5; z += 2)
      box(s, {x, 3.2f, float(z)}, {.23f, .23f, .10f}, copper * .7f);
  }
  for (int k = 0; k < 3; ++k) {
    float x = 3.65f + k * .38f;
    beam(s, {x, .2f, -3.55f}, {x, 3.27f, -3.55f}, .2f, k % 2 ? copper : steel);
    beam(s, {x, 3.27f, -3.55f}, {x, 3.27f, 4.6f}, .2f, copper * .65f);
    for (float y : {.5f, 1.6f, 2.9f}) {
      hoop(s, {x, y, -3.38f}, .16f, copper);
      box(s, {x, y, -3.43f}, {.29f, .08f, .3f}, steel);
    }
    hoop(s, {x, 1.5f, -3.16f}, .2f, {.48f, .055f, .02f});
    beam(s, {x - .18f, 1.5f, -3.16f}, {x + .18f, 1.5f, -3.16f}, .035f, copper);
    beam(s, {x, 1.32f, -3.16f}, {x, 1.68f, -3.16f}, .035f, copper);
  }
  // CRT bezel, battered console plinth, protective trim and warning stripes.
  box(s, {0, 1.77f, -3.7f}, {6.9f, 3.1f, .36f}, {.18f, .2f, .15f});
  box(s, {0, 1.79f, -3.49f}, {6.66f, 2.96f, .08f}, {.03f, .04f, .025f});
  // Screen is in front of the bezel: panel coordinates are on its front plane.
  box(s, {0, 1.79f, PanelZ + .22f}, {6.48f, 2.82f, .02f}, {.025f, .038f, .026f},
      3);
  box(s, {0, .44f, -2.98f}, {6.7f, .64f, 1.15f}, {.16f, .19f, .14f});
  box(s, {0, .8f, -2.92f}, {6.8f, .12f, 1.15f}, steel);
  for (int k = -15; k <= 15; ++k) {
    box(s, {k * .2f, .87f, -2.61f}, {.12f, .04f, .13f},
        k % 4 ? V(.17f, .2f, .17f) : amber * .7f);
    box(s, {k * .21f, .48f, -2.39f}, {.075f, .38f, .015f}, {.04f, .05f, .035f});
  }
  for (int k = 0; k < 12; ++k) {
    box(s, {-3.26f + k * .59f, 3.27f, -3.38f}, {.035f, .035f, .035f}, copper);
    box(s, {-3.26f + k * .59f, .32f, -3.38f}, {.035f, .035f, .035f}, copper);
  }
  // Chunky equipment racks / cracked instruments at the sides.
  for (int side : {-1, 1}) {
    for (int k = 0; k < 3; ++k) {
      float z = -1.5f + k * 1.6f, x = side * 4.25f;
      box(s, {x, 1.3f, z}, {1.05f, 2.6f, 1.25f}, steel * .8f);
      box(s, {x - side * .55f, 1.55f, z}, {.03f, 1.9f, 1.06f},
          {.055f, .065f, .04f});
      for (int light = 0; light < 5; ++light)
        box(s, {x - side * .58f, .9f + light * .26f, z - .3f},
            {.025f, .065f, .065f}, light % 3 ? mint * .32f : amber, 3);
      for (int slot = 0; slot < 9; ++slot)
        box(s, {x - side * .58f, 2.1f, z - .35f + slot * .09f},
            {.025f, .3f, .025f}, steel * .6f);
    }
  }
  // Rear bulkhead door and warning sign; chamber is intentionally next
  // milestone.
  box(s, {.65f, 1.35f, 4.79f}, {2.5f, 2.7f, .12f}, copper * .8f);
  box(s, {.65f, 1.35f, 4.70f}, {2.12f, 2.48f, .12f}, steel);
  box(s, {.65f, 1.35f, 4.61f}, {.055f, 2.42f, .04f}, {.03f, .04f, .025f});
  for (int k = 0; k < 9; ++k) {
    box(s, {-.34f + k * .25f, .22f, 4.60f}, {.14f, .18f, .03f},
        k % 2 ? amber * .6f : V(.04f));
  }
  // Caged practical lamps; luminous strips, not a physics-heavy lighting
  // engine.
  for (int z : {-2, 2}) {
    box(s, {0, 3.46f, float(z)}, {1.7f, .13f, .42f}, steel * .5f);
    box(s, {0, 3.35f, float(z)}, {1.45f, .045f, .19f}, {.8f, .56f, .25f}, 3);
    for (int k = -3; k <= 3; ++k)
      box(s, {k * .2f, 3.31f, float(z)}, {.025f, .06f, .25f}, steel);
  }
  label(s, {-3.1f, 3.47f, -3.38f}, "VESSEL 09 / REACTOR CONTROL", .025f,
        amber * .7f);
  label(s, {-2.35f, 1.01f, -2.36f}, "E - USE CONSOLE    /    R - RESET PLANT",
        .019f, {.55f, .6f, .4f});
  // Loose cables on the floor, refuse, a toolbox and an abandoned stool.
  for (int k = 0; k < 14; ++k) {
    float a = float(k) * .25f;
    beam(s, {2.7f + std::sin(a) * .25f, .06f, -1.8f + a},
         {2.7f + std::sin(a + .25f) * .25f, .06f, -1.55f + a}, .035f,
         {.035f, .04f, .025f});
  }
  box(s, {-2.8f, .23f, 2.9f}, {.8f, .46f, .55f}, {.3f, .14f, .045f});
  beam(s, {-3, .48f, 2.9f}, {-2.6f, .48f, 2.9f}, .055f, steel);
  box(s, {-2.4f, .77f, -.2f}, {.52f, .12f, .48f}, {.23f, .15f, .075f});
  for (float dx : {-.2f, .2f})
    for (float dz : {-.18f, .18f})
      beam(s, {-2.4f + dx, .05f, -.2f + dz}, {-2.4f + dx, .72f, -.2f + dz},
           .055f, steel);
  for (int k = 0; k < 12; ++k) {
    SceneInstance scrap;
    scrap.transform = glm::scale(
        glm::rotate(glm::translate(glm::mat4(1), V(-3 + k * .22f, .055f,
                                                   2.2f + std::sin(float(k)))),
                    k * .7f, V(0, 1, 0)),
        V(.15f, .025f, .11f));
    scrap.tint = glm::vec4(k % 2 ? copper * .6f : V(.25f, .24f, .17f), 4);
    s.push_back(scrap);
  }
}
FrameTransforms Room::camera(float aspect) const {
  V forward(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
            -std::cos(yaw) * std::cos(pitch));
  FrameTransforms f;
  f.view = glm::lookAt(eye, eye + forward, V(0, 1, 0));
  f.projection =
      glm::perspective(glm::radians(settings.fieldOfView), aspect, .05f, 50.0f);
  f.projection[1][1] *= -1;
  return f;
}
bool Room::input(const GameControls &in, float dt, float aspect,
                 const plant::Config &cfg, uint32_t &routeRod,
                 uint32_t &routeBank) {
  if (in.interact || (in.pause && console)) {
    if (console) {
      eye = savedEye;
      yaw = savedYaw;
      pitch = savedPitch;
      console = false;
    } else if (eye.z < .7f && std::abs(eye.x) < 3.5f) {
      savedEye = eye;
      savedYaw = yaw;
      savedPitch = pitch;
      eye = {0, 1.80f, .15f};
      yaw = 0;
      pitch = 0;
      console = true;
    }
  }
  if (!console) {
    yaw += in.mouseX * settings.mouseSensitivity;
    pitch = std::clamp(pitch - in.mouseY * settings.mouseSensitivity, -1.35f,
                       1.35f);
    V move = V(std::sin(yaw), 0, -std::cos(yaw)) * in.vertical +
             V(std::cos(yaw), 0, std::sin(yaw)) * in.strafe;
    if (glm::length(move) > 0)
      move = glm::normalize(move);
    eye += move * (dt * settings.walkSpeed *
                   (in.sprint ? settings.sprintMultiplier : 1));
    // Walkable aisle: keeps the camera out of fixed racks/console geometry.
    eye.x = std::clamp(eye.x, -3.45f, 3.45f);
    eye.z = std::clamp(eye.z, -2.05f, 4.35f);
    hoveredRod = hoveredBank = -1;
    panelCursor = {-1, -1};
    return false;
  }
  FrameTransforms f = camera(aspect);
  glm::vec4 near = glm::inverse(f.projection * f.view) *
                   glm::vec4(in.cursorX * 2 - 1, in.cursorY * 2 - 1, 0, 1);
  glm::vec4 far = glm::inverse(f.projection * f.view) *
                  glm::vec4(in.cursorX * 2 - 1, in.cursorY * 2 - 1, 1, 1);
  V origin = V(near) / near.w,
    direction = glm::normalize(V(far) / far.w - origin);
  panelCursor = {-1, -1};
  hoveredRod = hoveredBank = -1;
  if (std::abs(direction.z) > .00001f) {
    float distance = (PanelZ + .33f - origin.z) / direction.z;
    if (distance > 0) {
      V point = origin + direction * distance;
      panelCursor = {(point.x / PanelWidth + .5f) * 1000,
                     (3.17f - point.y) / PanelHeight * 500};
    }
  }
  float cellWidth = 510.0f / cfg.rodsPerReactor;
  for (uint32_t r = 0; r < plant::rodCount(cfg); ++r) {
    auto p = rodLocation(cfg, r);
    if (inRect(panelCursor, p.x - cellWidth * .48f, p.y - 8, cellWidth * .96f,
               rodHeight(cfg) + 42))
      hoveredRod = int(r);
  }
  for (uint32_t b = 0; b < cfg.banks; ++b)
    if (inRect(panelCursor, 744, bankY(cfg, b) - 3, 229,
               287.0f / cfg.banks - 6))
      hoveredBank = int(b);
  if (in.secondary) {
    selected = -1;
    notice = "SELECT A ROD THEN A POWER BANK";
  }
  if (in.primary) {
    if (hoveredRod >= 0) {
      selected = hoveredRod;
      notice = "ROD " + std::to_string(selected + 1) +
               " SELECTED / CHOOSE POWER BANK";
    } else if (hoveredBank >= 0 && selected >= 0) {
      routeRod = uint32_t(selected);
      routeBank = uint32_t(hoveredBank);
      return true;
    }
  }
  return false;
}
void Room::routeResult(bool accepted) {
  notice =
      accepted ? "ROUTED / FORECAST REFRESHING" : "BANK FULL / CHOOSE ANOTHER";
  if (accepted)
    selected = -1;
}
void Room::inspectionView(bool panel) {
  if (panel) {
    console = true;
    eye = {0, 1.8f, .15f};
    yaw = 0;
    pitch = 0;
  } else {
    console = false;
    eye = {2.3f, 1.7f, 2.1f};
    yaw = -.28f;
    pitch = .015f;
  }
}
void Room::draw(std::vector<SceneInstance> &scene, const plant::Config &cfg,
                const plant::Panel &p, const plant::Forecast &forecast,
                bool predicting, float elapsed, bool paused,
                const char *computeName) const {
  scene = architecture;
  // Native world-space CRT. Every glyph/gradient/wire uses the shared cube
  // mesh. Screen front is in front of the console bezel (different Z layers
  // below).
  rect(scene, 0, 0, 1000, 500, {.008f, .026f, .02f}, .26f);
  for (int y = 8; y < 495; y += 9)
    rect(scene, 7, float(y), 986, .45f, {.014f, .04f, .028f}, .265f);
  text(scene, 28, 20, "VESSEL 09 / CORE LOAD MANAGEMENT", 2.2f, mint, .31f);
  text(scene, 29, 46,
       "INTEGER NEUTRON TRANSPORT / " + std::to_string(cfg.capacityGW) +
           " GW PER ROD",
       1.35f, dim * 1.6f, .31f);
  rect(scene, 25, 68, 950, 1, dim, .3f);
  for (uint32_t reactor = 0; reactor < cfg.reactors; ++reactor)
    text(scene, 10, 101 + reactor * (270.0f / cfg.reactors),
         "R" + std::to_string(reactor + 1), 1.25f, amber, .32f);
  for (uint32_t b = 0; b < cfg.banks; ++b) {
    float y = bankY(cfg, b), h = 287.0f / cfg.banks - 8;
    bool shortage = p.bankDeliveredGW[b] < p.bankDemandGW[b];
    bool hover = hoveredBank == int(b);
    rect(scene, 740, y - 3, 235, h,
         hover ? V(.2f, .3f, .18f) : V(.05f, .12f, .1f), .28f);
    rect(scene, 744, y + 1, 227, h - 8, {.012f, .028f, .022f}, .29f);
    const char *names[] = {"LIFE SUPPORT", "DRIVE CORE", "SHIP SYSTEMS",
                           "SHIELD GRID"};
    text(scene, 785, y + 4, b < 4 ? names[b] : "AUXILIARY", 1.55f, mint, .34f);
    text(scene, 785, y + 19,
         std::to_string(p.bankDeliveredGW[b]) + " / " +
             std::to_string(p.bankDemandGW[b]) + " GW",
         1.55f, shortage ? amber : mint * .7f, .34f);
    float bar = std::min(1.0f, float(p.bankDeliveredGW[b]) /
                                   std::max(1.0f, float(p.bankDemandGW[b])));
    rect(scene, 786, y + 35, 175, 4, dim * .4f, .3f);
    rect(scene, 786, y + 35, 175 * bar, 4, shortage ? danger : mint * .55f,
         .32f);
    for (uint32_t plug = 0; plug < cfg.plugsPerBank; ++plug) {
      float py = y + 8 + plug * (h - 18) / std::max(1u, cfg.plugsPerBank - 1);
      bool used = false;
      for (uint32_t r = 0; r < p.rods; ++r)
        used |= p.sockets[r] == b * cfg.plugsPerBank + plug;
      rect(scene, 752, py, 11, 5, used ? amber : dim * .4f, .34f);
    }
    if (shortage && int(elapsed * 4) % 2 == 0) {
      text(scene, 785, y + h - 16, "NOT ENOUGH POWER", 1.35f, danger, .34f);
      for (int spark = 0; spark < 4; ++spark) {
        float angle = elapsed * 12 + spark * 1.57f;
        V center = panelPoint(970, y + 10, .36f);
        beam(scene, center,
             center + V(std::cos(angle) * .065f, std::sin(angle) * .065f, 0),
             .008f, amber, 3);
      }
    }
  }
  // Wires behind the rods. Animated white current moves in proportion to GW.
  for (uint32_t r = 0; r < p.rods; ++r) {
    auto start = rodLocation(cfg, r);
    start.y += rodHeight(cfg) + 4;
    uint32_t b = p.sockets[r] / cfg.plugsPerBank,
             plug = p.sockets[r] % cfg.plugsPerBank;
    float h = 287.0f / cfg.banks - 8;
    glm::vec2 end(757,
                  bankY(cfg, b) + 10 +
                      plug * (h - 18) / std::max(1u, cfg.plugsPerBank - 1));
    float elbow = 590 + float(r % 6) * 20;
    std::array<glm::vec2, 4> points = {start, glm::vec2(elbow, start.y),
                                       glm::vec2(elbow, end.y), end};
    V color = rodColor(r) * .36f;
    bool highlight = int(r) == selected || int(r) == hoveredRod;
    for (int k = 0; k < 3; ++k) {
      V a = panelPoint(points[k].x, points[k].y, .285f),
        z = panelPoint(points[k + 1].x, points[k + 1].y, .285f);
      if (highlight)
        beam(scene, a - V(0, 0, .003f), z - V(0, 0, .003f), .035f, rodColor(r),
             3);
      beam(scene, a, z, .019f, color, 3);
      float speed =
          .02f + std::pow(float(p.rodGW[r]) / cfg.capacityGW, 2.5f) * 2.8f;
      float u = std::fmod(elapsed * speed + float(r) * .17f + k * .3f, 1.0f);
      V at = glm::mix(a, z, u) + V(0, 0, .015f),
        tangent = glm::normalize(z - a);
      if (glm::length(z - a) > .01f && p.rodGW[r]) {
        beam(scene, at - tangent * .035f, at + tangent * .035f, .014f, V(.9f),
             3);
        V side(-tangent.y, tangent.x, 0);
        beam(scene, at + tangent * .035f, at + side * .025f, .012f, V(.9f), 3);
        beam(scene, at + tangent * .035f, at - side * .025f, .012f, V(.9f), 3);
      }
    }
  }
  float width = std::min(32.0f, 510.0f / cfg.rodsPerReactor * .48f),
        height = rodHeight(cfg);
  bool valid = forecast.trials && forecast.revision == p.revision &&
               p.tick >= forecast.tick && p.tick - forecast.tick <= 10;
  for (uint32_t r = 0; r < p.rods; ++r) {
    auto location = rodLocation(cfg, r);
    float x = location.x, y = location.y;
    bool highlight = int(r) == selected || int(r) == hoveredRod;
    rect(scene, x - width * .5f - 3, y - 3, width + 6, height + 6,
         highlight ? rodColor(r) : dim * .6f, .31f);
    for (uint32_t segment = 0; segment < cfg.segments; ++segment) {
      float heat =
          float(p.heatQ6[r * cfg.segments + segment]) / (cfg.criticalHeat * 64);
      rect(scene, x - width * .5f, y + height * segment / cfg.segments, width,
           height / cfg.segments + .06f, heatColor(heat), .33f);
    }
    rect(scene, x - width * .5f, y - 7, width, 3, rodColor(r), .35f);
    text(scene, x - 12, y + height + 8, std::to_string(r + 1), 1.2f,
         rodColor(r), .35f);
    text(scene, x - 21, y + height + 21, std::to_string(p.rodGW[r]) + "GW",
         1.05f, mint * .8f, .35f);
    const std::string risk =
        valid ? std::to_string((forecast.hits[r] * 100 + forecast.trials / 2) /
                               forecast.trials) +
                    "%"
              : "--";
    text(scene, x - 21, y - 20, risk, 1.2f,
         valid && forecast.hits[r] * 10 > forecast.trials * 7 ? danger : amber,
         .35f);
    if (p.runaway[r] && int(elapsed * 3) % 2 == 0)
      rect(scene, x - width * .5f, y + height * .45f, width, 3, danger, .36f);
  }
  rect(scene, 25, 392, 950, 1, dim, .31f);
  text(scene, 30, 406,
       "ROD % = MELTDOWN RISK / NEXT " +
           std::to_string(forecast.horizonTicks ? forecast.horizonTicks : 60) +
           " SECONDS",
       1.5f, mint * .7f, .34f);
  text(scene, 30, 428, console ? notice : "E - USE CONSOLE / WALK CLOSER", 1.6f,
       console ? amber : mint, .34f);
  text(scene, 30, 450,
       console ? "CLICK ROD THEN BANK / RIGHT CLICK CANCEL / E EXIT"
               : "WASD WALK / MOUSE LOOK / SHIFT HURRY",
       1.25f, dim * 1.5f, .34f);
  text(scene, 30, 473,
       paused ? "PAUSED"
              : (!console     ? "THERMAL CLOCK HELD / CONSOLE OFFLINE"
                 : predicting ? "FORECAST SAMPLING"
                              : "THERMAL CLOCK ACTIVE"),
       1.15f, mint * .6f, .34f);
  text(scene, 750, 474, "T " + std::to_string(p.tick) + " / " + computeName,
       1.15f, dim * 1.7f, .34f);
  // Very restrained aiming reticle; console uses the ordinary OS mouse cursor.
  if (!console && !paused) {
    SceneInstance dot;
    dot.transform = glm::scale(glm::mat4(1), V(.002f, .0035f, 0));
    dot.tint = glm::vec4(.5f, .58f, .42f, -1);
    scene.push_back(dot);
  }
}
bool Room::selfTest() {
  plant::Config cfg;
  Room r;
  uint32_t rod = 0, bank = 0;
  GameControls move;
  move.vertical = 1;
  for (int i = 0; i < 100; ++i)
    r.input(move, .1f, 16.0f / 9, cfg, rod, bank);
  if (r.eye.z < -2.051f)
    throw std::runtime_error("walk collision");
  move = {};
  move.interact = true;
  r.input(move, 0, 16.0f / 9, cfg, rod, bank);
  if (!r.console)
    throw std::runtime_error("console enter");
  // Project a rod/bank point through the actual camera, then feed that cursor
  // back through the inverse ray test. This covers Vulkan's flipped Y too.
  auto click = [&](glm::vec2 point) {
    auto f = r.camera(16.0f / 9);
    glm::vec4 clip = f.projection * f.view *
                     glm::vec4(panelPoint(point.x, point.y, .33f), 1);
    GameControls c;
    c.cursorX = (clip.x / clip.w + 1) * .5f;
    c.cursorY = (clip.y / clip.w + 1) * .5f;
    c.primary = true;
    return r.input(c, 0, 16.0f / 9, cfg, rod, bank);
  };
  auto first = rodLocation(cfg, 0);
  first.y += 20;
  click(first);
  bool route = click({870, bankY(cfg, 1) + 20});
  if (!route || rod != 0 || bank != 1)
    throw std::runtime_error("panel picking");
  r.input(move, 0, 16.0f / 9, cfg, rod, bank);
  if (r.console)
    throw std::runtime_error("console exit");
  return true;
}
} // namespace controlroom
