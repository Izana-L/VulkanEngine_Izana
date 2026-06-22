// ColorConstants.hpp
#pragma once
#include <glm/glm.hpp>

namespace MathLib::Colors {

    // =========================================================
    // Basic colors (RGBA, linear space)
    // =========================================================

    constexpr glm::vec4 WHITE = { 1.0f, 1.0f, 1.0f, 1.0f };
    constexpr glm::vec4 BLACK = { 0.0f, 0.0f, 0.0f, 1.0f };
    constexpr glm::vec4 TRANSPARENT = { 0.0f, 0.0f, 0.0f, 0.0f };

    constexpr glm::vec4 RED = { 1.0f, 0.0f, 0.0f, 1.0f };
    constexpr glm::vec4 GREEN = { 0.0f, 1.0f, 0.0f, 1.0f };
    constexpr glm::vec4 BLUE = { 0.0f, 0.0f, 1.0f, 1.0f };

    constexpr glm::vec4 YELLOW = { 1.0f, 1.0f, 0.0f, 1.0f };
    constexpr glm::vec4 CYAN = { 0.0f, 1.0f, 1.0f, 1.0f };
    constexpr glm::vec4 MAGENTA = { 1.0f, 0.0f, 1.0f, 1.0f };

    constexpr glm::vec4 ORANGE = { 1.0f, 0.5f, 0.0f, 1.0f };
    constexpr glm::vec4 PURPLE = { 0.5f, 0.0f, 0.5f, 1.0f };
    constexpr glm::vec4 PINK = { 1.0f, 0.4f, 0.7f, 1.0f };
    constexpr glm::vec4 BROWN = { 0.6f, 0.3f, 0.0f, 1.0f };

    constexpr glm::vec4 GRAY_DARK = { 0.2f, 0.2f, 0.2f, 1.0f };
    constexpr glm::vec4 GRAY = { 0.5f, 0.5f, 0.5f, 1.0f };
    constexpr glm::vec4 GRAY_LIGHT = { 0.8f, 0.8f, 0.8f, 1.0f };

    // =========================================================
    // Debug / gizmo colors (useful for editor visualization)
    // =========================================================

    constexpr glm::vec4 GIZMO_X = { 1.0f, 0.2f, 0.2f, 1.0f };  // X axis red
    constexpr glm::vec4 GIZMO_Y = { 0.2f, 1.0f, 0.2f, 1.0f };  // Y axis green
    constexpr glm::vec4 GIZMO_Z = { 0.2f, 0.2f, 1.0f, 1.0f };  // Z axis blue
    constexpr glm::vec4 GIZMO_SEL = { 1.0f, 0.8f, 0.0f, 1.0f };  // selected object yellow

    // =========================================================
    // Sky and environment defaults
    // =========================================================

    constexpr glm::vec4 SKY_DEFAULT = { 0.53f, 0.81f, 0.98f, 1.0f }; // light blue sky
    constexpr glm::vec4 FOG_DEFAULT = { 0.7f,  0.7f,  0.7f,  1.0f }; // neutral gray fog

    // =========================================================
    // Light colors
    // =========================================================

    constexpr glm::vec3 LIGHT_SUN = { 1.0f,  0.95f, 0.8f };  // warm sunlight
    constexpr glm::vec3 LIGHT_SKY = { 0.5f,  0.7f,  1.0f };  // cool skylight
    constexpr glm::vec3 LIGHT_MOON = { 0.6f,  0.65f, 0.9f };  // cool moonlight
    constexpr glm::vec3 LIGHT_FIRE = { 1.0f,  0.5f,  0.1f };  // warm fire/torch
    constexpr glm::vec3 LIGHT_FLUORESC = { 0.9f,  1.0f,  0.95f };  // cool fluorescent

} 