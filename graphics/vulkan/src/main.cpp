#include "renderer.hpp"
#include "camera.hpp"
#include "profiling.hpp"
#include <cmath>
#include <numbers>
#include <random>
#include <array>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <iomanip>


// N body config
// ######################################## 
int NUM_SPHERES = 1500;
// Reference galaxy at 1500 particles. Larger N expands it at constant surface
// density instead of packing more mass into the same radius.
float RADIUS_MAX = 4.0f;
float INIT_VELOCITY = 1.0f; // Multiplier of circular speed: 1 = balanced orbit.
float TOTAL_DISK_MASS = 0.0f; // Derived after reading --particles.
float CENTRAL_MASS = 500.0f;
float diskScale = 1.0f;
float STEP_SIZE = 0.001;
float GRAV_SOFT = 0.75;     // weakens extreme close encouters
float GRAV_CONS = 1.0;      // grav constant G

float X_POS_MIN = -2;
float X_POS_MAX =  2;
float Y_POS_MIN = -2;
float Y_POS_MAX =  2;
float Z_POS_MIN = -2;
float Z_POS_MAX =  2;
float X_VEL_MIN = -0.0;
float X_VEL_MAX =  0.0;
float Y_VEL_MIN = -0.0;
float Y_VEL_MAX =  0.0;
float Z_VEL_MIN = -0.0;
float Z_VEL_MAX =  0.0;
float X_ACL_MIN = -0.0;
float X_ACL_MAX =  0.0;
float Y_ACL_MIN = -0.0;
float Y_ACL_MAX =  0.0;
float Z_ACL_MIN = -0.0;
float Z_ACL_MAX =  0.0;
float MASS_MAX  =  0.5f;
float MASS_MIN  =  0.5f;

std::vector<float> x_pos; 
std::vector<float> y_pos; 
std::vector<float> z_pos; 
std::vector<float> x_vel; 
std::vector<float> y_vel; 
std::vector<float> z_vel; 
std::vector<float> x_acl; 
std::vector<float> y_acl; 
std::vector<float> z_acl; 
std::vector<float> mass; 

std::vector<gpu_particle> gpu_particle_vector;
bool benchmarkInitialConditions = false;

// ######################################## 


namespace {

constexpr uint32_t WINDOW_WIDTH = 800;
constexpr uint32_t WINDOW_HEIGHT = 600;
const std::string MODEL_PATH = "models/viking_room.obj";
const std::string TEXTURE_PATH = "textures/viking_room.png";

void hashCombine(std::size_t& seed, float value) {
    seed ^= std::hash<float>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct VertexHash {
    std::size_t operator()(const Vertex& vertex) const {
        std::size_t seed = 0;
        hashCombine(seed, vertex.position.x);
        hashCombine(seed, vertex.position.y);
        hashCombine(seed, vertex.position.z);
        hashCombine(seed, vertex.color.x);
        hashCombine(seed, vertex.color.y);
        hashCombine(seed, vertex.color.z);
        hashCombine(seed, vertex.textureCoordinate.x);
        hashCombine(seed, vertex.textureCoordinate.y);
        return seed;
    }
};


// -1   -0.5    0   0.5     1 
// -0.5   *          *
// 0
// 0.5    *          *
// 1


Mesh makeSphere(float radius, float z_offset){

    Mesh matthew_sphere;
    
    float pi = (float)(4.0 * std::atan(1.0));

    // 8 verticies center ring: 
    for(int i = 0; i < 8; i ++){
        float theta = (i*(360.0 / 8.0))*(pi / 180.0);
        float x = (float)(radius * std::sin(theta));
        float y = (float)(radius * std::cos(theta));

        Vertex v_i{};
        v_i.position = {x, y, z_offset};
        v_i.color = {1.0, 1.0, 1.0};
        matthew_sphere.vertices.push_back(v_i);
    }

    // 8 verticies upper and lower ring: 
    float phi = (45.0/2); // phase offshift 
    float smaller_radius = 0.75 * radius; 
    float rise = radius/2;
    for(int i = 0; i < 8; i ++){
        float theta = (i*(360.0 / 8.0) + phi)*(pi / 180.0);
        float x = (float)(smaller_radius * std::sin(theta));
        float y = (float)(smaller_radius * std::cos(theta));

        Vertex v_i{};
        v_i.position = {x, y, z_offset + rise};
        v_i.color = {1.0, 1.0, 1.0};
        matthew_sphere.vertices.push_back(v_i);
    }
    for(int i = 0; i < 8; i ++){
        float theta = (i*(360.0 / 8.0) + phi)*(pi / 180.0);
        float x = (float)(smaller_radius * std::sin(theta));
        float y = (float)(smaller_radius * std::cos(theta));

        Vertex v_i{};
        v_i.position = {x, y, z_offset - rise};
        v_i.color = {1.0, 1.0, 1.0};
        matthew_sphere.vertices.push_back(v_i);
    }


    // Temp indicies to see if this is working: 

    // Connect middle to upper ring 
    for(int i = 0; i < 8; i ++){
        matthew_sphere.indices.push_back(i);        // middle
        matthew_sphere.indices.push_back(i+8);      // upper
        matthew_sphere.indices.push_back((i+1)%8);  // middle
    }

    // Connect upper ring to middle
    int rotate = 7;
    for(int i = 0; i < 8; i ++){
        matthew_sphere.indices.push_back(((i+1+rotate)%8)+8);   // upper
        matthew_sphere.indices.push_back(i);                    // middle
        matthew_sphere.indices.push_back(((i+rotate)%8)+8);     // upper
    }


    // Connect middle to lower ring 
    for(int i = 0; i < 8; i ++){
        matthew_sphere.indices.push_back((i+1)%8);  // middle
        matthew_sphere.indices.push_back(i+16);     // lower
        matthew_sphere.indices.push_back(i);        // middle
    }

    // Connect lower ring to middle
    for(int i = 0; i < 8; i ++){
        matthew_sphere.indices.push_back(((i+rotate)%8)+16);     // lower
        matthew_sphere.indices.push_back(i);                     // middle
        matthew_sphere.indices.push_back(((i+1+rotate)%8)+16);   // lower
    }


    // two more verticies for the top and bottom 
    Vertex v_u{};
    v_u.position = {0.0, 0.0, z_offset + radius};
    v_u.color = {1.0, 1.0, 1.0};
    matthew_sphere.vertices.push_back(v_u);
    Vertex v_b{};
    v_b.position = {0.0, 0.0, z_offset - radius};
    v_b.color = {1.0, 1.0, 1.0};
    matthew_sphere.vertices.push_back(v_b);


    // connect upper ring to top 
    // Connect upper ring to middle
    int top_index = 24;
    int bottom_index = 25;
    for(int i = 0; i < 8; i ++){
        matthew_sphere.indices.push_back(((i+rotate)%8)+8);     // upper
        matthew_sphere.indices.push_back(top_index);            // top
        matthew_sphere.indices.push_back(((i+1+rotate)%8)+8);   // upper
    }
    for(int i = 0; i < 8; i ++){
        matthew_sphere.indices.push_back(((i+1+rotate)%8)+16);   // lower
        matthew_sphere.indices.push_back(bottom_index);          // middle
        matthew_sphere.indices.push_back(((i+rotate)%8)+16);     // lower
    }

    return matthew_sphere;

}




Mesh makeQuad(float z_offset) {
    Mesh matthew_quad;  // confused about the memory managment here, is this on the stack or the heap. id guess stack but then idk how we are returning it? i guess that we can return it by just pushing the values onto the stack? can return more than just an interger value? should know this tbh

    Vertex matthew_v1{};
    matthew_v1.position = {-0.5f, -0.5f, z_offset};         // glm::vec3 is 3-component fp32 vector, position = x,y,z
    matthew_v1.color = {1.0f, 0.0f, 0.0f};         // glm::vec3 is 3-component fp32 vector, color    = r,g,b
    matthew_v1.textureCoordinate = {0.0f, 0.0f};        // glm::vec2 is 2-component fp32 vector, text coordinates are now the textecure maps to the surface, ignoring for now
    matthew_quad.vertices.push_back(matthew_v1);
    matthew_quad.indices.push_back(0);

    Vertex matthew_v2{};
    matthew_v2.position = {-0.5f, 0.5f, z_offset};          // glm::vec3 is 3-component fp32 vector, position = x,y,z
    matthew_v2.color = {0.0f, 1.0f, 0.0f};         // glm::vec3 is 3-component fp32 vector, color    = r,g,b
    matthew_v2.textureCoordinate = {0.0f, 0.0f};        // glm::vec2 is 2-component fp32 vector, text coordinates are now the textecure maps to the surface, ignoring for now
    matthew_quad.vertices.push_back(matthew_v2);
    matthew_quad.indices.push_back(1);

    Vertex matthew_v3{};
    matthew_v3.position = {0.5f, -0.5f, z_offset};          // glm::vec3 is 3-component fp32 vector, position = x,y,z
    matthew_v3.color = {0.0f, 0.0f, 1.0f};         // glm::vec3 is 3-component fp32 vector, color    = r,g,b
    matthew_v3.textureCoordinate = {0.0f, 0.0f};        // glm::vec2 is 2-component fp32 vector, text coordinates are now the textecure maps to the surface, ignoring for now
    matthew_quad.vertices.push_back(matthew_v3);
    matthew_quad.indices.push_back(2);

    Vertex matthew_v4{};
    matthew_v4.position = {0.5f, 0.5f, z_offset};           // glm::vec3 is 3-component fp32 vector, position = x,y,z
    matthew_v4.color = {1.0f, 0.0f, 1.0f};         // glm::vec3 is 3-component fp32 vector, color    = r,g,b
    matthew_v4.textureCoordinate = {0.0f, 0.0f};        // glm::vec2 is 2-component fp32 vector, text coordinates are now the textecure maps to the surface, ignoring for now
    matthew_quad.vertices.push_back(matthew_v4);
    matthew_quad.indices.push_back(2);                  //  
    matthew_quad.indices.push_back(1);
    matthew_quad.indices.push_back(3);
    // indices tells the GPU which vertices to connect, using their positions in the vertices vector.
    // For example, when drawing triangles: indices = {0, 1, 2,  2, 1, 3};
    // Each group of three describes one triangle:
        // - First triangle: vertices[0], vertices[1], vertices[2].
        // - Second triangle: vertices[2], vertices[1], vertices[3].

    // Fill in the back of the square so can see front and back
    matthew_quad.indices.push_back(3);
    matthew_quad.indices.push_back(1);
    matthew_quad.indices.push_back(2);

    matthew_quad.indices.push_back(2);
    matthew_quad.indices.push_back(1);
    matthew_quad.indices.push_back(0);



    // local vertex
    //     → model matrix       places it in the world
    //     → view matrix        expresses it relative to the camera
    //     → projection matrix  applies perspective
    //     → screen
    // projection * view * model * position




    return matthew_quad;
}


Mesh loadObj(const std::string& path) {
    tinyobj::attrib_t attributes;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warning;
    std::string error;

    if (!tinyobj::LoadObj(
            &attributes,
            &shapes,
            &materials,
            &warning,
            &error,
            path.c_str(),
            nullptr,
            true)) {
        throw std::runtime_error(error);
    }

    if (!warning.empty()) {
        std::cout << "OBJ warning: " << warning << '\n';
    }

    Mesh mesh;
    std::unordered_map<Vertex, uint32_t, VertexHash> uniqueVertices;

    for (const auto& shape : shapes) {
        for (const auto& index : shape.mesh.indices) {
            Vertex vertex{};
            vertex.position = {
                attributes.vertices[3 * index.vertex_index + 0],
                attributes.vertices[3 * index.vertex_index + 1],
                attributes.vertices[3 * index.vertex_index + 2]
            };
            vertex.color = {1.0f, 1.0f, 1.0f};

            if (index.texcoord_index >= 0) {
                vertex.textureCoordinate = {
                    attributes.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attributes.texcoords[2 * index.texcoord_index + 1]
                };
            }

            auto [entry, inserted] =
                uniqueVertices.emplace(vertex, static_cast<uint32_t>(mesh.vertices.size()));
            if (inserted) {
                mesh.vertices.push_back(vertex);
            }
            mesh.indices.push_back(entry->second);
        }
    }

    return mesh;
}


glm::mat4 makeModelMatrixStar(float timeSeconds, float radius, float speed, float phase_offset, float z_offset){
    float pi = (float)(4.0 * std::atan(1.0));
    float theta = (60*timeSeconds + phase_offset) * (pi / 180.0) * speed;
    float x = (float)(radius * std::sin(theta));
    float y = (float)(radius * std::cos(theta));

    glm::vec3 translation_vector = glm::vec3(x, y, z_offset);

    return glm::translate(glm::mat4(1.0f), translation_vector);
}


glm::mat4 makeModelMatrixStar2(float x, float y, float z){
    glm::vec3 translation_vector = glm::vec3(x, y, z);
    return glm::translate(glm::mat4(1.0f), translation_vector);
}



glm::mat4 makeModelMatrix(float timeSeconds, float degreesPerSecond, const glm::vec3& axis) {
    // Per-object math remains here, not in the renderer.
    return glm::rotate(
        glm::mat4(1.0f),
        timeSeconds * glm::radians(degreesPerSecond),
        axis);
}

FrameTransforms makeTransforms(float aspectRatio, const CameraState& camera) {

    // Your mesh stores vertex positions. The model, view, and projection matrices transform those positions to determine where the mesh appears on screen.
    // - Model: Places the mesh in the world. Your square might be defined around (0, 0, 0); this matrix can move it somewhere else, rotate it, or resize it.
    // - View: Expresses the world relative to the camera. It accounts for where the camera is and which way it faces.
    // - Projection: Determines how that camera’s view becomes a flat image. A perspective projection makes distant objects appear smaller; an orthographic projection keeps their size independent of distance.

    // Shared camera matrices; model matrices are kept separately per mesh.
    FrameTransforms transforms{};
    transforms.view = glm::lookAt(
        camera.position,                    // where camera is in the world (x, y, z)
        camera.position + camera.forward,   // point the camera looks towards 
        camera.up                           // reference "up" direction, positive z (x,y,z)
    );

    transforms.projection = glm::perspective(
        glm::radians(camera.verticalFieldOfViewDegrees),    // how wide an angle the camera sees veritically 
        aspectRatio,                                        // viewpoint (camera fov) width / height (ratio)
        0.1f,                                               // the near clipping plane (no clue what that mean)
        benchmarkInitialConditions ? 10.0f : 30.0f * (RADIUS_MAX / 4.0f) // include the whole disk
    );

    // GLM uses an OpenGL-style Y axis; Vulkan's clip-space Y axis is inverted.
    transforms.projection[1][1] *= -1.0f;
    return transforms;
}

} // namespace

// A function that returns a pseudo-random number based entirely on the input_number
int getRandomFromInput(int input_number, int min_range, int max_range) {
    std::mt19937 engine(input_number);
    std::uniform_int_distribution<int> distribution(min_range, max_range);
    return distribution(engine);
}

float getRandomFloat(float min_range, float max_range) {
    // Profiling runs repeat the same starting scene; normal runs stay random.
    static thread_local std::mt19937 engine{
        (profiling::ENABLED || benchmarkInitialConditions) ? profiling::RANDOM_SEED : std::random_device{}()};
    std::uniform_real_distribution<float> distribution(min_range, max_range);
    return distribution(engine);
}


// One-time setup, NOT a force solver used every frame. Approximate the smooth
// disk with equal-mass ring samples and tabulate its inward acceleration.
// Unlike G*M_enclosed/r^2, this includes the pull of rings outside the orbit.
constexpr int ROTATION_SAMPLES = 128;
std::array<float, ROTATION_SAMPLES + 1> makeDiskRotationCurve(float radiusMin) {
    constexpr int RINGS = 64;
    constexpr int ANGLES = 128;
    constexpr double PI = 3.14159265358979323846;
    std::vector<glm::dvec2> sources;
    sources.reserve(RINGS * ANGLES);
    const double innerSquared = double(radiusMin) * radiusMin;
    const double outerSquared = double(RADIUS_MAX) * RADIUS_MAX;
    for (int ring = 0; ring < RINGS; ++ring) {
        // Equal-area rings match the uniform-area particle distribution below.
        const double radius = std::sqrt(innerSquared +
            (outerSquared - innerSquared) * (ring + 0.5) / RINGS);
        for (int angle = 0; angle < ANGLES; ++angle) {
            const double phi = 2.0 * PI * (angle + 0.5) / ANGLES;
            sources.emplace_back(radius * std::cos(phi), radius * std::sin(phi));
        }
    }

    std::array<float, ROTATION_SAMPLES + 1> speeds{};
    const double softSquared = double(GRAV_SOFT) * GRAV_SOFT;
    const double sampleMass = double(TOTAL_DISK_MASS) / sources.size();
    for (int sample = 1; sample <= ROTATION_SAMPLES; ++sample) {
        const double radius = double(RADIUS_MAX) * sample / ROTATION_SAMPLES;
        const double centralDistanceSquared = radius * radius + softSquared;
        double inwardAcceleration = GRAV_CONS * CENTRAL_MASS * radius /
            (centralDistanceSquared * std::sqrt(centralDistanceSquared));
        for (const auto& source : sources) {
            const double dx = source.x - radius;
            const double distanceSquared = dx * dx + source.y * source.y + softSquared;
            inwardAcceleration -= GRAV_CONS * sampleMass * dx /
                (distanceSquared * std::sqrt(distanceSquared));
        }
        // Centripetal acceleration v^2/r must match the inward gravitational pull.
        speeds[sample] = float(std::sqrt(std::max(0.0, radius * inwardAcceleration)));
    }
    return speeds;
}

void configureDisk() {
    if (benchmarkInitialConditions) {
        // Preserve the historical benchmark scene and physics for comparisons.
        MASS_MAX = 750.0f / std::max(1, NUM_SPHERES - 1);
        return;
    }
    if (!std::isfinite(RADIUS_MAX) || RADIUS_MAX <= 0.0f ||
        !std::isfinite(INIT_VELOCITY) || INIT_VELOCITY < 0.0f ||
        !std::isfinite(GRAV_SOFT) || GRAV_SOFT <= 0.0f) {
        throw std::invalid_argument("invalid disk radius, velocity multiplier or softening");
    }
    const float countScale = std::max(1.0f, float(NUM_SPHERES - 1) / 1499.0f);
    diskScale = std::sqrt(countScale);
    RADIUS_MAX *= diskScale;
    GRAV_SOFT *= diskScale; // Keep the same relative smoothing of close encounters.
    TOTAL_DISK_MASS = 0.5f * (NUM_SPHERES - 1);
    CENTRAL_MASS *= countScale;
    MASS_MAX = MASS_MIN = NUM_SPHERES > 1 ? TOTAL_DISK_MASS / (NUM_SPHERES - 1) : 0.0f;
}

void init_disk2(){
    gpu_particle_vector.clear();
    gpu_particle_vector.reserve(NUM_SPHERES);
    const float radiusMin = 0.05f * RADIUS_MAX;
    const auto circularSpeeds = benchmarkInitialConditions
        ? std::array<float, ROTATION_SAMPLES + 1>{} : makeDiskRotationCurve(radiusMin);
    // Each particle needs Position, Mass, Velocity
    glm::vec4 positionMass; // x, y, z, mass
    glm::vec4 velocity;     // x, y, z, padding 

    for(int i = 0; i < NUM_SPHERES-1; i ++){
        // Init position: 
        // sqrt(uniform radius-squared) distributes stars uniformly over AREA.
        float radius = benchmarkInitialConditions ? getRandomFloat(0.2f, 4.0f) :
            std::sqrt(getRandomFloat(radiusMin * radiusMin, RADIUS_MAX * RADIUS_MAX));
        float theta = getRandomFloat(0.0f, 2.0f *
            (benchmarkInitialConditions ? 3.14156f : 3.14159265358979323846f));
        positionMass = glm::vec4(radius*std::cos(theta), radius*std::sin(theta), 0.0f, MASS_MAX);

        // Init velocity: 
        const float tablePosition = std::clamp(radius / RADIUS_MAX * ROTATION_SAMPLES,
            0.0f, float(ROTATION_SAMPLES));
        const int lower = std::min(int(tablePosition), ROTATION_SAMPLES - 1);
        const float fraction = tablePosition - lower;
        const float speed = benchmarkInitialConditions ? 10.0f * std::sqrt(radius) :
            INIT_VELOCITY * ((1.0f - fraction) * circularSpeeds[lower] +
                fraction * circularSpeeds[lower + 1]);
        velocity = glm::vec4(-1.0*speed*std::sin(theta), 1.0*speed*std::cos(theta), 0.0f, 0.0f);

        // Push particle to array 
        gpu_particle particle = {positionMass, velocity};
        gpu_particle_vector.push_back(particle);
    }

    // Put a massive star at the center of the galaxy
    const float centralMass = benchmarkInitialConditions ? 500.0f : CENTRAL_MASS;
    positionMass = glm::vec4(0.0f, 0.0f, 0.0f, centralMass);
    velocity = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);                   // velocity
    gpu_particle particle = {positionMass, velocity};               // struct
    gpu_particle_vector.push_back(particle);                        // push

    return;
}


void init_disk(){
    for(int i = 0; i < NUM_SPHERES-1; i ++){

        // Initlize the position: 
        // ---------------------------------------------------
        float radius = getRandomFloat(0.2, 4.0);
        float theta = getRandomFloat(0.0, 2*3.14156);
        x_pos.push_back(radius * std::cos(theta));
        y_pos.push_back(radius * std::sin(theta));
        z_pos.push_back(0.0);
        // ---------------------------------------------------


        // Initlize the velocity: 
        // ---------------------------------------------------
        const float speed = INIT_VELOCITY * std::pow(radius, 0.5f);
        const float direction = 1.0f; // +1 = counterclockwise, -1 = clockwise
        x_vel.push_back(-direction * speed * std::sin(theta));
        y_vel.push_back( direction * speed * std::cos(theta));
        z_vel.push_back(0.0f);


        // Initlize the acceleration and mass: 
        // ---------------------------------------------------
        x_acl.push_back(0.0);
        y_acl.push_back(0.0);
        z_acl.push_back(0.0);
        mass.push_back (MASS_MAX);

    }

    // Put a massive star at the center of the galaxy
    x_pos.push_back(0.0);
    y_pos.push_back(0.0);
    z_pos.push_back(0.0);
    x_vel.push_back(0.0);
    y_vel.push_back(0.0);
    z_vel.push_back(0.0);
    x_acl.push_back(0.0);
    y_acl.push_back(0.0);
    z_acl.push_back(0.0);
    mass.push_back (MASS_MAX*1000.0);


    return;
}


int main(int argc, char** argv) {
    try {
        RendererOptions options;
        std::string modeName = "gpu-all";
        bool benchmark = false, freeze = true;
        int measuredFrames = 30, warmupFrames = 5;
        float theta = 0.5f;
        for (int arg = 1; arg < argc; ++arg) {
            const std::string flag = argv[arg];
            auto value = [&]() -> std::string {
                if (++arg >= argc) throw std::invalid_argument("missing value for " + flag);
                return argv[arg];
            };
            if (flag == "--mode") modeName = value();
            else if (flag == "--particles") NUM_SPHERES = std::stoi(value());
            else if (flag == "--theta") theta = std::stof(value());
            else if (flag == "--frames") measuredFrames = std::stoi(value());
            else if (flag == "--warmup") warmupFrames = std::stoi(value());
            else if (flag == "--benchmark") benchmark = true;
            else if (flag == "--evolve") freeze = false;
            else if (flag == "--help") {
                std::cout << "--mode cpu-all|cpu-bh|gpu-all|gpu-bh --particles N --theta 0.5\n"
                          << "--benchmark [--frames 30 --warmup 5 --evolve]\n"
                          << "Benchmarks freeze positions by default, but still compute all forces,\n"
                          << "rebuild the tree every step, integrate and render the same sphere mesh.\n";
                return 0;
            } else throw std::invalid_argument("unknown option: " + flag);
        }
        if (modeName == "cpu-all") options.mode = SimulationMode::CpuAllPairs;
        else if (modeName == "cpu-bh") options.mode = SimulationMode::CpuBarnesHut;
        else if (modeName == "gpu-all") options.mode = SimulationMode::GpuAllPairs;
        else if (modeName == "gpu-bh") options.mode = SimulationMode::GpuBarnesHut;
        else throw std::invalid_argument("invalid simulation mode");
        if (NUM_SPHERES < 1 || NUM_SPHERES > (1 << 23) || measuredFrames < 1 || measuredFrames > 100000 ||
            warmupFrames < 0 || warmupFrames > 100000 ||
            !std::isfinite(theta) || theta < 0.0f) throw std::invalid_argument("invalid benchmark parameters");
        options.measureTimings = benchmark;
        options.uncappedPresentation = benchmark;
        benchmarkInitialConditions = benchmark;
        configureDisk();

        // Array to hold all of the meshes
        std::vector<Mesh> meshes;
        Mesh star = makeSphere(0.025f, 0.0f);
        meshes.push_back(star);

        // ########################################################################

        // Initialize particles on the CPU, then upload once in Renderer.
        // -------------------------------------------------------------------

        init_disk2();
        // ########################################################################


        // CPU chooses the initial conditions and physics parameters.
        // Renderer runs the selected CPU/GPU backend; camera math stays here.
        const SimulationParameters simulation = {
            benchmark && freeze ? 0.0f : STEP_SIZE, GRAV_SOFT, GRAV_CONS, theta};

        Renderer renderer(
            WINDOW_WIDTH,
            WINDOW_HEIGHT,
            "Vulkan learning renderer",
            meshes,
            gpu_particle_vector,
            TEXTURE_PATH, options);
        
        CameraController camera(
            renderer, 
            benchmark ? glm::vec3(4.0f, 4.0f, 0.0f) :
                (RADIUS_MAX / 4.0f) * glm::vec3(0.0f, -7.0f, 8.0f), // see the disk, not its edge
            glm::vec3(0.0f, 0.0f, 0.0f)         // initial target
        );
        camera.setMoveSpeed(3.0f * (RADIUS_MAX / 4.0f));

        if (benchmark) {
            // Completed-frame latency: no accumulating a queue of unfinished GPU work.
            // Initial conditions and camera are identical across all four modes.
            const auto transforms = makeTransforms(renderer.aspectRatio(), camera.state());
            FrameTimings sums;
            std::vector<double> frameTimes;
            double totalMs = 0.0;
            for (int frame = 0; frame < warmupFrames + measuredFrames; ++frame) {
                const auto start = std::chrono::steady_clock::now();
                renderer.pollEvents();
                renderer.drawFrame(transforms, simulation);
                const auto t = renderer.finishFrame();
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                if (frame < warmupFrames) continue;
                frameTimes.push_back(ms);
                totalMs += ms;
                sums.cpuBuildMs += t.cpuBuildMs; sums.cpuForceMs += t.cpuForceMs;
                sums.cpuIntegrateMs += t.cpuIntegrateMs; sums.uploadMs += t.uploadMs;
                sums.recordMs += t.recordMs; sums.fenceWaitMs += t.fenceWaitMs;
                sums.acquireMs += t.acquireMs; sums.submitMs += t.submitMs; sums.presentMs += t.presentMs;
                sums.gpuBuildMs += t.gpuBuildMs; sums.gpuForceMs += t.gpuForceMs; sums.gpuRenderMs += t.gpuRenderMs;
            }
            std::sort(frameTimes.begin(), frameTimes.end());
            const size_t p95 = (frameTimes.size() * 95 + 99) / 100 - 1;
            std::cout << std::fixed << std::setprecision(6)
                << "{\"mode\":\"" << modeName << "\",\"n\":" << NUM_SPHERES
                << ",\"theta\":" << theta << ",\"frames\":" << measuredFrames
                << ",\"frozen\":" << (freeze ? "true" : "false")
                << ",\"frame_ms\":" << totalMs / measuredFrames
                << ",\"p95_ms\":" << frameTimes[p95] << ",\"fps\":" << 1000.0 * measuredFrames / totalMs
                << ",\"cpu_build_ms\":" << sums.cpuBuildMs / measuredFrames
                << ",\"cpu_force_ms\":" << sums.cpuForceMs / measuredFrames
                << ",\"cpu_integrate_ms\":" << sums.cpuIntegrateMs / measuredFrames
                << ",\"upload_ms\":" << sums.uploadMs / measuredFrames
                << ",\"record_ms\":" << sums.recordMs / measuredFrames
                << ",\"fence_ms\":" << sums.fenceWaitMs / measuredFrames
                << ",\"acquire_ms\":" << sums.acquireMs / measuredFrames
                << ",\"submit_ms\":" << sums.submitMs / measuredFrames
                << ",\"present_ms\":" << sums.presentMs / measuredFrames
                << ",\"gpu_build_ms\":" << sums.gpuBuildMs / measuredFrames
                << ",\"gpu_force_ms\":" << sums.gpuForceMs / measuredFrames
                << ",\"gpu_render_ms\":" << sums.gpuRenderMs / measuredFrames << "}\n";
            return 0;
        }


        // While 1 forever loop body
        // ########################################################################
        auto previousTime = std::chrono::steady_clock::now();
        while (!renderer.shouldClose()) {
            if constexpr (profiling::ENABLED) profiling::profiler().beginFrame();
            {
                profiling::ScopedTimer timer(profiling::Stage::EventsCamera);
                renderer.pollEvents();
                const auto now = std::chrono::steady_clock::now();
                const float deltaSeconds = std::chrono::duration<float>(now - previousTime).count();
                previousTime = now;
                camera.update(deltaSeconds);
            }
            
            // CPU computes the camera; drawFrame runs selected physics then rendering.
            // As before, STEP_SIZE is simulation time per rendered frame, not wall time.
            FrameTransforms transforms;
            {
                profiling::ScopedTimer timer(profiling::Stage::CameraMatrices);
                transforms = makeTransforms(renderer.aspectRatio(), camera.state());
            }
            renderer.drawFrame(transforms, simulation);
            if constexpr (profiling::ENABLED) {
                profiling::profiler().endFrame(
                    gpu_particle_vector.size(), sizeof(FrameTransforms) +
                    ((options.mode == SimulationMode::CpuAllPairs || options.mode == SimulationMode::CpuBarnesHut)
                        ? gpu_particle_vector.size() * sizeof(glm::mat4) : 0));
            }
        }
        // ########################################################################
    } 
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
