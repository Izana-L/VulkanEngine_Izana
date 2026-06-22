#pragma once

// =========================================================
// ECS.hpp
// Single include point for the entire ECS module.
// Include this file to access entities, components, and
// the World container.
// =========================================================

// --- Hashing and ID utilities ---
#include <fnv.hpp>
#include <Id.hpp>
#include <Id_Provider.hpp>

// --- Entity ---
#include <Entity.hpp>

// --- Storage infrastructure ---
#include <Sparse_Array.hpp>
#include <Sparse_Set.hpp>
#include <IComponent_Storage.hpp>
#include <Component_Storage.hpp>

// --- World ---
#include <World.hpp>