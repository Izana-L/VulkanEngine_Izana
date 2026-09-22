#pragma once

// =========================================================
// Job_Graph.hpp
// =========================================================
//
// Reserved for the dependency-graph layer of the Job System: a set of
// named jobs with explicit "runs after" edges, scheduled onto
// Thread_Dispatcher through Atomic_Counter::Set_on_zero continuations.
//
// Nothing is implemented yet. This header exists (with an include guard)
// so the design references in Thread_Dispatcher and Atomic_Counter point
// at a real file, and so including it is harmless. Do not include it
// expecting functionality until the graph is implemented.
