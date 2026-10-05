#include <Engine_Context.hpp>

#include <Renderer.hpp>

#include <stdexcept>

namespace EngineCore
{

    Render_Debug::Render_Debug(Renderer_System::Renderer& _renderer)
        : renderer(_renderer)
    {}

    const Renderer_System::Render_Debug_Settings& Render_Debug::Get_settings() const
    {
        return renderer.Get_debug_settings();
    }

    void Render_Debug::Set_settings(const Renderer_System::Render_Debug_Settings& _settings)
    {
        renderer.Set_debug_settings(_settings);
    }

    Engine_Context::Engine_Context(ECS::World& _world, ECS::Transform_System& _transforms,
        Input_System::Input& _input, Gpu_Assets& _assets,
        const Platform::Time& _time, Render_Debug& _render_debug)
        : world(_world)
        , transforms(_transforms)
        , input(_input)
        , assets(_assets)
        , time(_time)
        , render_debug(_render_debug)
    {}

    void Engine_Context::Add_system(Phase _phase, System& _system)
    {
        if (_phase >= Phase::Count)
            throw std::invalid_argument("Engine_Context::Add_system: invalid phase");

        systems[static_cast<size_t>(_phase)].push_back(&_system);
    }

    void Engine_Context::Run_phase(Phase _phase, float _dt)
    {
        const std::vector<System*>& phase_systems = systems[static_cast<size_t>(_phase)];

        // Index loop on purpose: a system may register another one while it
        // runs, and that would invalidate iterators.
        for (size_t i = 0; i < phase_systems.size(); ++i)
            phase_systems[i]->Update(*this, _dt);
    }

} // namespace EngineCore