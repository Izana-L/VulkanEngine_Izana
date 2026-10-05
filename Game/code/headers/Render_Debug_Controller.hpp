#pragma once

#include <System.hpp>

#include <cstddef>

namespace Input_System { class Input; }

namespace Game
{

    // Render_Debug_Controller: applies the debug actions pressed this frame
    // to the Renderer's runtime switches (Renderer_System::Render_Debug_Settings),
    // reached through Engine_Context::Render_debug():
    //   DebugLightCulling   - clustered lights <-> every light (reference)
    //   DebugClusterView    - cycles the cluster grid overlays
    //   DebugOpaquePath     - cycles direct / CPU indirect / GPU indirect /
    //                         GPU culled opaque draws
    //   DebugFreezeCulling  - freezes the culling camera
    //   DebugShowBounds     - bounding volume wireframes (the ellipsoids
    //                         the culling tests)
    //   DebugStats          - GPU timings and counters every second
    //   DebugIsolateTimings - isolated GPU timing scopes (a full barrier
    //                         before each; the totals are not frame times)
    // Actions missing from the input JSON never fire (Bind_actions()
    // reports each one at startup).
    class Render_Debug_Controller : public EngineCore::System
    {
    public:

        Render_Debug_Controller() = default;
        ~Render_Debug_Controller() = default;

        // Resolves the debug actions listed above to ids, once. Call it after
        // Input::Load_actions() (which invalidates every cached id) and
        // before the first Update(). A name missing from the input JSON is
        // reported on std::cerr and its switch never fires.
        void Bind_actions(const Input_System::Input& _input);

        void Update(EngineCore::Engine_Context& _context, float _dt) override;

    private:

        // Action ids of the debug switches, set by Bind_actions().
        struct Debug_Action_Ids
        {
            size_t light_culling = static_cast<size_t>(-1);
            size_t cluster_view = static_cast<size_t>(-1);
            size_t opaque_path = static_cast<size_t>(-1);
            size_t freeze_culling = static_cast<size_t>(-1);
            size_t show_bounds = static_cast<size_t>(-1);
            size_t stats = static_cast<size_t>(-1);
            size_t isolate_timings = static_cast<size_t>(-1);
        };

        Debug_Action_Ids debug_action_ids;
    };

} // namespace EngineCore