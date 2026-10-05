#pragma once

#include <System.hpp>
#include <Render_Debug_Settings.hpp>

#include <array>
#include <cstddef>
#include <vector>

namespace ECS { class World; class Transform_System; }
namespace Input_System { class Input; }
namespace Platform { class Time; }
namespace Renderer_System { class Renderer; }

namespace EngineCore
{

    class Gpu_Assets;

    // Render_Debug: the application's view of the Renderer's runtime
    // switches. It exposes only the plain-value settings, so the application
    // never holds the Renderer itself.
    class Render_Debug
    {
    public:

        explicit Render_Debug(Renderer_System::Renderer& _renderer);

        const Renderer_System::Render_Debug_Settings& Get_settings() const;
        void Set_settings(const Renderer_System::Render_Debug_Settings& _settings);

    private:

        Renderer_System::Renderer& renderer;
    };

    // Engine_Context: everything an Application or a System may touch, and
    // nothing else. A new engine subsystem (physics, audio) adds an accessor
    // here and no interface changes.
    class Engine_Context
    {
    public:

        Engine_Context(ECS::World& _world,
            ECS::Transform_System& _transforms,
            Input_System::Input& _input,
            Gpu_Assets& _assets,
            const Platform::Time& _time,
            Render_Debug& _render_debug);

        Engine_Context(const Engine_Context&) = delete;
        Engine_Context& operator=(const Engine_Context&) = delete;

        ECS::World& World() { return world; }
        ECS::Transform_System& Transforms() { return transforms; }
        Input_System::Input& Input() { return input; }
        Gpu_Assets& Assets() { return assets; }
        const Platform::Time& Time() const { return time; }
        Render_Debug& Render_debug() { return render_debug; }

        // Registers _system in _phase. Systems of one phase run in
        // registration order. The system must outlive the loop.
        void Add_system(Phase _phase, System& _system);

    private:

        // Only the engine runs phases: the order of the frame is not the
        // application's to change.
        friend class Engine_Loop;
        void Run_phase(Phase _phase, float _dt);

        ECS::World& world;
        ECS::Transform_System& transforms;
        Input_System::Input& input;
        Gpu_Assets& assets;
        const Platform::Time& time;
        Render_Debug& render_debug;

        std::array<std::vector<System*>, static_cast<size_t>(Phase::Count)> systems;
    };

} // namespace EngineCore