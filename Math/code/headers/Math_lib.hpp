#pragma once

// =========================================================
// Math.hpp
// Single include point for the entire Math module.
// Include this file when you need access to vectors, matrices,
// quaternions, constants, and general math utilities all at once.
// =========================================================

// --- Base templates ---
#include "Vector.hpp"
#include "Matrix.hpp"

// --- Vector types and operations ---
#include "Vector2.hpp"
#include "Vector3.hpp"
#include "Vector4.hpp"

// --- Matrix types and operations ---
#include "Matrix2.hpp"
#include "Matrix3.hpp"
#include "Matrix4.hpp"

// --- Rotation ---
#include "Quaternion.hpp"

// --- Constants ---
#include "MathConstants.hpp"

// --- General float utilities ---
#include "MathUtils.hpp"