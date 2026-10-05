#include <Render_Debug_Controller.hpp>

#include <Engine_Context.hpp>
#include <Input.hpp>

#include <cstdint>

namespace Game
{

    namespace
    {
        // Next value of a cyclic enumeration with a Count enumerator.
        template <typename ENUM>
        ENUM Next_value(ENUM _value)
        {
            const uint32_t next = static_cast<uint32_t>(_value) + 1u;
            return static_cast<ENUM>(next % static_cast<uint32_t>(ENUM::Count));
        }

        // Flips _value when _action was pressed this frame. True if it did.
        bool Toggle_on_press(const Input_System::Input& _input, size_t _action, bool& _value)
        {
            if (!_input.Was_action_pressed(_action))
                return false;

            _value = !_value;
            return true;
        }

        // Moves _value to its next enumerator when _action was pressed this
        // frame. True if it did.
        template <typename ENUM>
        bool Cycle_on_press(const Input_System::Input& _input, size_t _action, ENUM& _value)
        {
            if (!_input.Was_action_pressed(_action))
                return false;

            _value = Next_value(_value);
            return true;
        }
    }

    void Render_Debug_Controller::Bind_actions(const Input_System::Input& _input)
    {
        constexpr const char* CONSUMER = "Render_Debug_Controller";

        debug_action_ids.light_culling = _input.Resolve_action_id("DebugLightCulling", CONSUMER);
        debug_action_ids.cluster_view = _input.Resolve_action_id("DebugClusterView", CONSUMER);
        debug_action_ids.opaque_path = _input.Resolve_action_id("DebugOpaquePath", CONSUMER);
        debug_action_ids.freeze_culling = _input.Resolve_action_id("DebugFreezeCulling", CONSUMER);
        debug_action_ids.show_bounds = _input.Resolve_action_id("DebugShowBounds", CONSUMER);
        debug_action_ids.stats = _input.Resolve_action_id("DebugStats", CONSUMER);
        debug_action_ids.isolate_timings = _input.Resolve_action_id("DebugIsolateTimings", CONSUMER);
    }

    void Render_Debug_Controller::Update(EngineCore::Engine_Context& _context, float /*_dt*/)
    {
        const Input_System::Input& input = _context.Input();

        Renderer_System::Render_Debug_Settings settings = _context.Render_debug().Get_settings();

        // |= and not ||: every switch is evaluated, since several can be
        // pressed in the same frame. Light_Culling_Mode has two values, so
        // cycling it is the switch between clustered and brute force.
        bool changed = false;

        changed |= Cycle_on_press(input, debug_action_ids.light_culling, settings.light_culling);
        changed |= Cycle_on_press(input, debug_action_ids.cluster_view, settings.cluster_view);
        changed |= Cycle_on_press(input, debug_action_ids.opaque_path, settings.opaque_path);
        changed |= Toggle_on_press(input, debug_action_ids.freeze_culling, settings.freeze_culling);
        changed |= Toggle_on_press(input, debug_action_ids.show_bounds, settings.show_bounds);
        changed |= Toggle_on_press(input, debug_action_ids.stats, settings.print_stats);
        changed |= Toggle_on_press(input, debug_action_ids.isolate_timings, settings.isolate_gpu_timings);

        // The Renderer logs every value that changed.
        if (changed)
            _context.Render_debug().Set_settings(settings);
    }

} // namespace EngineCore