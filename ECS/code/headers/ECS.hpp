#pragma once

// =========================================================
// ECS.hpp
// Single include point for the entire ECS module.
// Include this file to access entities, components, and
// the World container.
// =========================================================


// --- Entity ---
#include <Entity.hpp>

// --- Storage infrastructure ---
#include <Sparse_Array.hpp>
#include <Sparse_Set.hpp>
#include <IComponent_Storage.hpp>
#include <Component_Storage.hpp>

// --- World ---
#include <World.hpp>