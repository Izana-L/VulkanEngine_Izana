// MathConstants.hpp
#pragma once
#include <cmath>

namespace MathLib::Constants {

    // =========================================================
    // Basic math constants
    // =========================================================

    constexpr float PI = 3.14159265358979323846f;
    constexpr float TWO_PI = PI * 2.0f;           // 360 grados en radianes
    constexpr float HALF_PI = PI / 2.0f;           // 90 grados en radianes
    constexpr float QUARTER_PI = PI / 4.0f;           // 45 grados en radianes
    constexpr float INV_PI = 1.0f / PI;           // util en calculos de iluminacion (BRDF)
    constexpr float INV_TWO_PI = 1.0f / TWO_PI;       // util en mapeo de entornos esfericos

    // =========================================================
    // Angle conversion
    // =========================================================

    constexpr float DEG_TO_RAD = PI / 180.0f;
    constexpr float RAD_TO_DEG = 180.0f / PI;

    // =========================================================
    // Float precision and tolerances
    // =========================================================

    constexpr float EPSILON = 1e-6f;   // tolerancia general de precision
    constexpr float EPSILON_SMALL = 1e-4f;   // tolerancia para comparaciones de vectores/matrices
    constexpr float EPSILON_LARGE = 1e-2f;   // tolerancia para comparaciones de angulos
    constexpr float FLOAT_MAX = 3.402823466e+38f;
    constexpr float FLOAT_MIN = 1.175494351e-38f;


    // =========================================================
    // Square roots (precalculated, avoids runtime sqrt calls)
    // =========================================================

    constexpr float SQRT2 = 1.41421356237f;   // diagonal de un cuadrado de lado 1
    constexpr float SQRT3 = 1.73205080757f;   // diagonal de un cubo de lado 1
    constexpr float INV_SQRT2 = 1.0f / SQRT2;     // util para normalizar diagonales 2D
    constexpr float INV_SQRT3 = 1.0f / SQRT3;     // util para normalizar diagonales 3D

    // =========================================================
    // Physics and simulation
    // =========================================================

    constexpr float GRAVITY = 9.81f;         // gravedad terrestre (m/s^2)
    constexpr float GRAVITY_MARS = 3.72f;         // gravedad marciana
    constexpr float GRAVITY_MOON = 1.62f;         // gravedad lunar
    constexpr float SPEED_OF_LIGHT = 299792458.0f;  // m/s (util en simulaciones)
    constexpr float SPEED_OF_SOUND = 343.0f;        // m/s en aire a 20°C

    // =========================================================
    // Rendering limits
    // =========================================================

    constexpr float NEAR_PLANE_DEFAULT = 1.f;          // plano cercano por defecto
    constexpr float FAR_PLANE_DEFAULT = 1000.0f;       // plano lejano por defecto
    constexpr float FOV_DEFAULT = 60.0f;         // campo de vision por defecto (grados)
    constexpr float FOV_MIN = 10.0f;         // fov minimo razonable
    constexpr float FOV_MAX = 170.0f;        // fov maximo razonable

    // =========================================================
    // Color
    // =========================================================

    constexpr float GAMMA = 2.2f;          // gamma estandar sRGB
    constexpr float INV_GAMMA = 1.0f / GAMMA;  // para linear -> gamma
    constexpr float HDR_MAX_LUMINANCE = 10000.0f;      // nits, maximo HDR10
    constexpr float SDR_MAX_LUMINANCE = 100.0f;        // nits, maximo SDR

    // =========================================================
    // Time
    // =========================================================

    constexpr float FIXED_TIMESTEP = 1.0f / 60.0f;  // paso fijo a 60hz
    constexpr float FIXED_TIMESTEP_120 = 1.0f / 120.0f; // paso fijo a 120hz
    constexpr float MAX_DELTA_TIME = 0.25f;          // delta maximo permitido (evita spiral of death)

    // =========================================================
    // Size limits
    // =========================================================

    constexpr int   MAX_ENTITIES = 100000;         // maximo de entidades en el ECS
    constexpr int   MAX_COMPONENTS = 256;            // maximo de tipos de componente distintos
    constexpr int   MAX_LIGHTS = 256;            // maximo de luces activas en escena
    constexpr int   MAX_BONES = 256;            // maximo de huesos por skeleton
    constexpr int   MAX_TEXTURE_SIZE = 8192;           // resolucion maxima de textura (px)
    constexpr int   MAX_MIP_LEVELS = 14;             // log2(8192) = 13, +1
    constexpr int   MAX_RENDER_TARGETS = 8;              // maximo de render targets simultaneos
    constexpr int   MAX_VERTEX_BUFFERS = 16;             // maximo de vertex buffers simultaneos

    // =========================================================
    // Audio
    // =========================================================

    constexpr float AUDIO_MAX_DISTANCE = 100.0f;         // distancia maxima de audio 3D
    constexpr int   AUDIO_SAMPLE_RATE = 44100;          // hz, estandar CD
    constexpr int   AUDIO_CHANNELS = 2;              // stereo por defecto
    constexpr int   MAX_AUDIO_SOURCES = 64;             // maximas fuentes de audio simultaneas

    // =========================================================
    // Input
    // =========================================================

    constexpr float AXIS_DEADZONE = 0.1f;           // zona muerta de analog sticks
    constexpr float MOUSE_SENSITIVITY = 0.1f;           // sensibilidad de raton por defecto
    constexpr float GAMEPAD_SENSITIVITY = 1.0f;           // sensibilidad de gamepad por defecto

} // namespace MathLib::Constants