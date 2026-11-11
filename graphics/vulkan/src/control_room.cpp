#include "control_room.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace controlroom {
namespace {
using V = glm::vec3;
constexpr float PanelZ = -3.85f, PanelWidth = 3.2f, PanelHeight = 1.6f,
                PanelTop = 2.85f;
const V amber(.95f, .43f, .08f), mint(.53f, .65f, .72f), dim(.19f, .22f, .29f),
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
// Closed, thin triangular prisms. Both sides render correctly without turning
// backface culling off. Large non-coplanar triangles are the room's actual
// shape.
void facet(std::vector<SceneInstance> &scene, V a, V b, V c, V color,
           float material = 5) {
  V middle = (a + b) * .5f, edge = b - a, tip = c - middle;
  if (glm::length(glm::cross(edge, tip)) < .00001f)
    return;
  SceneInstance object;
  object.transform[0] = glm::vec4(edge, 0);
  object.transform[1] =
      glm::vec4(glm::normalize(glm::cross(tip, edge)) * .008f, 0);
  object.transform[2] = glm::vec4(tip, 0);
  object.transform[3] = glm::vec4((middle + c) * .5f, 1);
  object.tint = glm::vec4(color, material);
  scene.push_back(object);
}
void shard(std::vector<SceneInstance> &scene, V center, V scale, V color) {
  const V points[] = {V(1, 0, 0), V(0, 0, 1), V(-1, 0, 0), V(0, 0, -1)};
  for (int side = 0; side < 4; ++side) {
    V a = center + points[side] * scale,
      b = center + points[(side + 1) % 4] * scale;
    facet(scene, a, b, center + V(0, scale.y, 0),
          color * (side % 2 ? .78f : 1.0f));
    facet(scene, b, a, center - V(0, scale.y, 0), color * .55f);
  }
}
void tendon(std::vector<SceneInstance> &scene, V a, V b, float radius, V color,
            float material = 5) {
  V direction = glm::normalize(b - a);
  V x = glm::normalize(glm::cross(
      direction, std::abs(direction.y) < .9f ? V(0, 1, 0) : V(0, 0, 1)));
  V y = glm::cross(direction, x);
  // Five-sided, deliberately angular tubing, not square-section pipes.
  for (int side = 0; side < 5; ++side) {
    float p = side * glm::two_pi<float>() / 5,
          q = (side + 1) * glm::two_pi<float>() / 5;
    V u = (x * std::cos(p) + y * std::sin(p)) * radius;
    V v = (x * std::cos(q) + y * std::sin(q)) * radius;
    facet(scene, a + u, b + u, b + v, color, material);
    facet(scene, a + u, b + v, a + v, color * .83f, material);
  }
}
glm::vec2 panelExtent(float aspect) {
  // A 2:1 dashboard letterboxed into any window. Same mapping for hit tests.
  float x = std::min(.94f, 1.8f / std::max(.1f, aspect));
  return {x, x * aspect * .5f};
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
  return {(x / 1000 - .5f) * PanelWidth, PanelTop - y / 500 * PanelHeight,
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
          rect(s, x + col * scale, y + row * scale, scale * .98f,
               scale * .98f, color, depth);
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
  auto &scene = architecture;
  scene.reserve(2400);
  // A skewed eight-sided hull, not a box with rectangular wall panels.
  const glm::vec2 section[] = {{-3.8f, 0},     {3.8f, 0},     {4.7f, 1.1f},
                               {4.25f, 3.45f}, {2.5f, 4.25f}, {-2.9f, 4.1f},
                               {-4.4f, 3.1f},  {-4.7f, 1.15f}};
  const float stations[] = {-4.45f, -2.5f, -.4f, 1.9f, 4.9f};
  auto point = [&](int ring, int side) {
    auto p = section[side % 8];
    float wobble = std::sin(float(ring * 13 + side * 7)) * .13f;
    p.x += wobble;
    if (side > 1)
      p.y += std::cos(float(ring * 5 + side * 3)) * .12f;
    return V(p, stations[ring]);
  };
  for (int ring = 0; ring < 4; ++ring)
    for (int side = 0; side < 8; ++side) {
      V a = point(ring, side), b = point(ring, (side + 1) % 8);
      V c = point(ring + 1, (side + 1) % 8), d = point(ring + 1, side);
      V center = (a + b + c + d) * .25f;
      V inward = glm::normalize(V(-center.x, 1.9f - center.y, 0));
      center +=
          inward * (.09f + .15f * std::abs(std::sin(float(side * 11 + ring))));
      float grey = .12f + .025f * float((side * 5 + ring * 3) % 4);
      V color(grey * .83f, grey * .86f, grey);
      if (side == 0)
        color *= .76f;
      facet(scene, a, b, center, color);
      facet(scene, b, c, center, color * .68f);
      facet(scene, c, d, center, color * .87f);
      facet(scene, d, a, center, color * .57f);
    }
  // Uneven end walls are also broad facets, with no square door frames.
  for (int end : {0, 4}) {
    V hub(.3f, 1.9f, stations[end] + (end == 0 ? .1f : -.1f));
    for (int side = 0; side < 8; ++side)
      facet(scene, point(end, side), point(end, (side + 1) % 8), hub,
            V(.115f, .12f, .14f) * (side % 2 ? .7f : 1.0f));
  }
  // Structural ribs read as black bones rather than a neat grid of pipework.
  for (int ring : {1, 3})
    for (int side = 1; side < 8; ++side) {
      V a = point(ring, side), b = point(ring, (side + 1) % 8);
      a = V(a.x * .94f, 1.9f + (a.y - 1.9f) * .94f, a.z);
      b = V(b.x * .94f, 1.9f + (b.y - 1.9f) * .94f, b.z);
      tendon(scene, a, b, .14f, {.075f, .072f, .085f});
    }
  // Sparse branching arteries follow coarse bent paths, with exposed purple
  // channels on their inward-facing side. Color pulses travel in the shader.
  auto artery = [&](const std::vector<V> &path, float radius, V inward) {
    for (size_t k = 1; k < path.size(); ++k) {
      tendon(scene, path[k - 1], path[k], radius, {.043f, .032f, .055f});
      V offset = inward * radius * .96f;
      tendon(scene, path[k - 1] + offset, path[k] + offset, radius * .27f,
             {.34f, .018f, .65f}, 6);
      if (k % 3 == 0)
        shard(scene, path[k], V(radius * 1.7f, radius * 1.5f, radius * 1.5f),
              {.065f, .045f, .08f});
    }
  };
  for (int side : {-1, 1}) {
    for (int track = 0; track < 2; ++track) {
      std::vector<V> path;
      for (int k = 0; k < 9; ++k) {
        float z = -4.2f + k * 1.1f;
        float y = .9f + track * 1.75f + std::sin(k * .9f + track) * .38f;
        path.push_back({side * (4.26f - .18f * std::sin(k * .8f)), y, z});
      }
      artery(path, track ? .14f : .19f, V(-float(side), 0, 0));
      for (int branch : {2, 5, 7}) {
        V a = path[branch];
        V b = a + V(-side * .13f, track ? -.42f : .45f, .35f);
        V c = b + V(-side * .12f, track ? -.65f : .68f, .55f);
        artery({a, b, c}, .065f, V(-float(side), 0, 0));
      }
    }
    // Knuckled root-like machinery growing out of each side of the floor.
    for (int k = 0; k < 4; ++k) {
      float z = -2.4f + k * 1.9f;
      shard(scene, {side * 3.8f, .55f, z}, {.65f, .9f, .65f},
            {.075f, .073f, .09f});
      tendon(scene, {side * 3.8f, .8f, z}, {side * 4.25f, 2.7f, z + .45f}, .11f,
             {.08f, .075f, .095f});
    }
  }
  std::vector<V> spine;
  for (int k = 0; k < 9; ++k)
    spine.push_back({-.65f + std::sin(k * .9f) * .55f,
                     3.86f + std::sin(k * .6f) * .09f, -4.2f + k * 1.1f});
  artery(spine, .18f, {0, -1, 0});
  // A rear sealed iris, with six large triangular vanes.
  V iris(0, 1.9f, 4.65f);
  for (int k = 0; k < 6; ++k) {
    float a = k * glm::two_pi<float>() / 6,
          b = (k + 1) * glm::two_pi<float>() / 6;
    V p = iris + V(std::cos(a) * 1.25f, std::sin(a) * 1.4f, 0);
    V q = iris + V(std::cos(b) * 1.25f, std::sin(b) * 1.4f, 0);
    facet(scene, p, q, iris + V(.14f, -.05f, -.16f), {.075f, .07f, .095f});
    tendon(scene, p, q, .045f, {.13f, .012f, .24f}, 6);
  }
  // The monitor is the only intentionally rectangular surface. Its surround
  // is a broken, bevelled chitin iris, embedded in a faceted pedestal.
  const glm::vec2 inner[] = {{-1.72f, -.57f}, {-1.72f, .57f}, {-1.4f, .9f},
                             {1.4f, .9f},     {1.72f, .57f},  {1.72f, -.57f},
                             {1.4f, -.9f},    {-1.4f, -.9f}};
  V center(0, 2.05f, -3.4f);
  for (int k = 0; k < 8; ++k) {
    auto p = inner[k], q = inner[(k + 1) % 8];
    V a = center + V(p, 0), b = center + V(q, 0);
    V c = center + V(q * 1.25f, -.35f), d = center + V(p * 1.25f, -.35f);
    facet(scene, a, b, c, {.13f, .12f, .16f});
    facet(scene, a, c, d, {.085f, .08f, .10f});
    if (k % 2 == 0)
      tendon(scene, a, b, .027f, {.21f, .025f, .38f}, 6);
  }
  shard(scene, {0, .65f, -3.55f}, {1.9f, .72f, .7f}, {.11f, .1f, .13f});
  for (int side : {-1, 1}) {
    artery({V(side * 3.9f, 1.9f, -4.05f), V(side * 2.7f, 2.3f, -3.95f),
            V(side * 2.1f, 2.55f, -3.7f)},
           .13f, {0, 0, 1});
    shard(scene, {side * 1.95f, .9f, -3.28f}, {.27f, .48f, .35f},
          {.08f, .075f, .10f});
  }
  label(scene, {-1.04f, 1.02f, -2.95f}, "E / ACCESS CORE", .025f,
        {.32f, .25f, .43f});
  // A few jagged mineral/sludge patches rather than hundreds of square tiles.
  for (int k = 0; k < 12; ++k) {
    float x = std::sin(k * 2.7f) * 2.8f, z = std::cos(k * 1.6f) * 3.1f;
    facet(scene, {x - .25f, .012f, z - .15f}, {x + .4f, .018f, z},
          {x - .05f, .025f, z + .4f}, {.04f, .035f, .05f});
  }
}
FrameTransforms Room::camera(float aspect, float elapsed) const {
  V forward(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
            -std::cos(yaw) * std::cos(pitch));
  FrameTransforms f;
  f.view = glm::lookAt(eye, eye + forward, V(0, 1, 0));
  f.projection =
      glm::perspective(glm::radians(settings.fieldOfView), aspect, .05f, 50.0f);
  f.projection[1][1] *= -1;
  f.atmosphere = {elapsed, settings.fogDensity, settings.grain,
                  settings.ambientLight};
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
    } else if (eye.z < .1f && std::abs(eye.x) < 2.25f && std::cos(yaw) > .35f) {
      savedEye = eye;
      savedYaw = yaw;
      savedPitch = pitch;
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
    // Walkable aisle inside the sloping hull and its thick, root-like ribs.
    eye.x = std::clamp(eye.x, -3.15f, 3.15f);
    eye.z = std::clamp(eye.z, -2.05f, 4.35f);
    hoveredRod = hoveredBank = -1;
    panelCursor = {-1, -1};
    return false;
  }
  // Console view is a flat, letterboxed dashboard, not a perspective ray cast.
  auto extent = panelExtent(aspect);
  panelCursor = {(in.cursorX * 2 - 1 + extent.x) / (2 * extent.x) * 1000,
                 (in.cursorY * 2 - 1 + extent.y) / (2 * extent.y) * 500};
  hoveredRod = hoveredBank = -1;
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
    savedEye = eye;
    savedYaw = yaw;
    savedPitch = pitch;
    console = true;
    eye = {0, 1.8f, .15f};
    yaw = 0;
    pitch = 0;
  } else {
    console = false;
    eye = {2.2f, 1.7f, 2.2f};
    yaw = -.32f;
    pitch = .08f;
  }
}
void Room::draw(std::vector<SceneInstance> &scene, const plant::Config &cfg,
                const plant::Panel &p, const plant::Forecast &forecast,
                bool predicting, float elapsed, bool paused,
                const char *computeName, float aspect) const {
  scene = architecture;
  if (console) {
    scene.clear();
    SceneInstance background;
    background.transform =
        glm::scale(glm::translate(glm::mat4(1), V(0, 0, .12f)), V(2, 2, 0));
    background.tint = glm::vec4(.006f, .008f, .013f, -1);
    scene.push_back(background);
  }
  const size_t panelStart = scene.size();
  rect(scene, 0, 0, 1000, 500, {.012f, .017f, .026f}, .26f);
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
         hover ? V(.26f, .19f, .36f) : V(.07f, .085f, .12f), .28f);
    rect(scene, 744, y + 1, 227, h - 8, {.018f, .024f, .037f}, .29f);
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
  if (console) {
    // Reuse the panel's native geometry/data in a readable 2D projection. Depth
    // layers survive the conversion, so cables cannot overwrite bank labels.
    auto extent = panelExtent(aspect);
    glm::mat4 projection(1);
    projection[0][0] = 2 * extent.x / PanelWidth;
    projection[1][1] = -2 * extent.y / PanelHeight;
    projection[3][1] = (2 * PanelTop / PanelHeight - 1) * extent.y;
    projection[2][2] = -.1f;
    projection[3][2] = .1f + .1f * PanelZ;
    for (size_t i = panelStart; i < scene.size(); ++i) {
      scene[i].transform = projection * scene[i].transform;
      scene[i].tint.w = -1;
    }
  }
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
  // The same aspect-aware letterbox is used for rendering and hit tests.
  for (float aspect : {4.0f / 3, 16.0f / 9, 21.0f / 9}) {
    auto click = [&](glm::vec2 point) {
      auto extent = panelExtent(aspect);
      GameControls c;
      c.primary = true;
      c.cursorX = ((point.x / 1000 * 2 - 1) * extent.x + 1) * .5f;
      c.cursorY = ((point.y / 500 * 2 - 1) * extent.y + 1) * .5f;
      return r.input(c, 0, aspect, cfg, rod, bank);
    };
    auto first = rodLocation(cfg, 0);
    first.y += 20;
    click(first);
    if (!click({870, bankY(cfg, 1) + 20}) || rod != 0 || bank != 1)
      throw std::runtime_error("letterboxed panel picking");
  }
  r.input(move, 0, 16.0f / 9, cfg, rod, bank);
  if (r.console)
    throw std::runtime_error("console exit");
  return true;
}
} // namespace controlroom
