#pragma once

#include <cstdint>

namespace EngineCore
{

    class Engine_Context;

    // Phase: where in the frame a system runs. The engine owns the loop and
    // the order of the phases; an application only chooses the phase of each
    // of its systems. Execution order, every frame:
    //   Input        - after the engine has polled and published the input
    //   Gameplay     - game logic that reads input and moves entities
    //   Fixed_Update - reserved for physics. It runs once per frame with the
    //                  frame delta for now: the fixed timestep accumulator
    //                  arrives with the first system that needs it
    //   Simulation   - systems that change the world; the engine recomputes
    //                  the transform matrices after the last one
    //   Extract      - right before the ECS is copied into the RenderPacket
    //   Render       - right before the frame is drawn
    enum class Phase : uint8_t
    {
        Input,
        Gameplay,
        Fixed_Update,
        Simulation,
        Extract,
        Render,
        Count
    };

    // System: one unit of per-frame logic, registered in a Phase through
    // Engine_Context::Add_system. The registrant owns the object; the
    // context only keeps a pointer to it.
    class System
    {
    public:

        virtual ~System() = default;

        virtual void Update(Engine_Context& _context, float _dt) = 0;
    };

} // namespace EngineCore