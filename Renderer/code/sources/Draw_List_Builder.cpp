#include <Draw_List_Builder.hpp>
#include <Renderer_Limits.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cassert>
#include <iostream>

namespace Renderer_System
{

    Draw_List_Builder::Draw_List_Builder()
    {
        opaque_draws.reserve(MAX_OBJECTS);
        transparent_draws.reserve(MAX_OBJECTS);
    }

    void Draw_List_Builder::Request_frustum_capture()
    {
        capture_frozen_frustum = true;
    }

    void Draw_List_Builder::Update_culling_frustum(const CoreTypes::Frustum& _view_frustum, bool _freeze)
    {
        if (capture_frozen_frustum)
        {
            frozen_frustum = _view_frustum;
            capture_frozen_frustum = false;
        }

        culling_frustum = _freeze ? frozen_frustum : _view_frustum;
    }

    Opaque_Draw_Path Draw_List_Builder::Resolve_opaque_path(Opaque_Draw_Path _requested, const CoreTypes::RenderPacket& _packet,
                                                            uint32_t _max_draw_indirect_count)
    {
        if (!Is_gpu_draw_path(_requested))
            return _requested;

        bool     single_pipeline = true;
        bool     first_found = false;
        uint8_t  first_pipeline_id = 0;
        uint32_t opaque_items = 0;

        for (const CoreTypes::Draw_Item& item : _packet.opaque_items)
        {
            if ((item.pass_mask & CoreTypes::Render_Pass_Bit::Opaque) == 0)
                continue;

            ++opaque_items;

            const uint8_t pipeline_id = CoreTypes::Get_pipeline_id(item.sort_key);

            if (!first_found)
            {
                first_pipeline_id = pipeline_id;
                first_found = true;
            }
            else if (pipeline_id != first_pipeline_id)
            {
                single_pipeline = false;
            }
        }

        if (single_pipeline && opaque_items <= _max_draw_indirect_count)
            return _requested;

        if (!warned_gpu_path_fallback)
        {
            std::cerr << "[Renderer] The GPU draw path needs every opaque item on one pipeline and at most maxDrawIndirectCount ("
                << _max_draw_indirect_count << ") of them; frames that break it use CPU-written indirect "
                "commands instead. Further occurrences are not reported.\n";
            warned_gpu_path_fallback = true;
        }

        return Opaque_Draw_Path::Cpu_Indirect;
    }

    void Draw_List_Builder::Build(const CoreTypes::RenderPacket& _packet, Object_GPU* _objects,
                                  const Mesh_Registry& _meshes, const Pipeline_Registry& _pipelines,
                                  uint32_t _material_count, bool _cull_transparents)
    {
        assert(_objects != nullptr && "Draw_List_Builder::Build: the object buffer of the frame is not mapped");

        // Rewritten from entry 0 every frame: the GPU finished reading this
        // slot's copy before its fence was waited on.
        opaque_draws.clear();
        transparent_draws.clear();
        opaque_runs.clear();
        transparent_candidates = 0;

        uint32_t object_count = 0;

        // _subpass: the subpass the list is drawn in; an item whose pipeline
        // was built for another one cannot draw there.
        // _candidates, when not null, counts the valid items of the list
        // before the frustum test.
        const auto add_items = [&](const std::vector<CoreTypes::Draw_Item>& _items, uint8_t _pass_bit, uint32_t _subpass,
                                   std::vector<Draw_Record>& _out, bool _frustum_test, uint32_t* _candidates)
            {
                for (const CoreTypes::Draw_Item& item : _items)
                {
                    // An item that does not take part in this pass is skipped,
                    // so a pass_mask actually selects passes instead of being
                    // decoration.
                    if ((item.pass_mask & _pass_bit) == 0) continue;

                    const uint8_t    item_pipeline_id = CoreTypes::Get_pipeline_id(item.sort_key);
                    const VkPipeline pipeline = _pipelines.Get_by_id(item_pipeline_id);

                    // Every reference is validated in every build, before its
                    // entry is written: an out-of-range transform index would
                    // read past the packet's array, an out-of-range material
                    // index past the written slots of the material table, a
                    // released mesh may already have lost its geometry, and a
                    // pipeline of another subpass is invalid in this one.
                    const bool valid =
                        pipeline != VK_NULL_HANDLE &&
                        _pipelines.Get_subpass(item_pipeline_id) == _subpass &&
                        _meshes.Is_drawable(item.mesh_gpu_id) &&
                        item.transform_idx < _packet.transform_count &&
                        item.material_index < _material_count;

                    if (!valid)
                    {
                        if (!warned_invalid_item)
                        {
                            std::cerr << "[Renderer] Draw item skipped: pipeline " << int(item_pipeline_id)
                                << " (built for subpass " << _pipelines.Get_subpass(item_pipeline_id) << ", drawn in subpass "
                                << _subpass << "), mesh " << item.mesh_gpu_id << ", transform " << item.transform_idx
                                << ", material " << item.material_index
                                << " (registered meshes: " << _meshes.Get_count() << ", transforms in packet: "
                                << _packet.transform_count << ", registered materials: " << _material_count
                                << "; a released mesh is also skipped). Further occurrences are not reported.\n";
                            warned_invalid_item = true;
                        }
                        continue;
                    }

                    const MathLib::Matrix4& model = _packet.transforms[item.transform_idx];
                    const Mesh_GPU&         mesh = _meshes.Get(item.mesh_gpu_id);

                    if (_candidates != nullptr)
                        ++(*_candidates);

                    // CPU culling: the test of cull_objects.comp, the same
                    // formula on the same planes (culling_frustum is what the
                    // UBO carries): the mesh bounding sphere placed by the
                    // model matrix, an ellipsoid, tested exactly against
                    // every plane, shear included.
                    if (_frustum_test)
                    {
                        if (!culling_frustum.Intersects_ellipsoid(model, MathLib::Vector4(mesh.bounds_center, mesh.bounds_radius)))
                            continue;
                    }

                    // The object buffer is full: this and every later item of
                    // the frame are skipped.
                    if (object_count >= MAX_OBJECTS)
                    {
                        if (!warned_object_overflow)
                        {
                            std::cerr << "[Renderer] More than " << MAX_OBJECTS << " draws in one frame: the rest are skipped. "
                                "Raise MAX_OBJECTS in Renderer_Limits.hpp. Further occurrences are not reported.\n";
                            warned_object_overflow = true;
                        }
                        return;
                    }

                    // -- Object entry --
                    // Its index is the draw's firstInstance, which the vertex
                    // shader receives as gl_InstanceIndex. No push constants:
                    // the shaders read everything per draw from this entry,
                    // the material table and the mesh table.
                    const uint32_t object_index = object_count++;

                    Object_GPU& object = _objects[object_index];
                    object.model = model;
                    // Inverse-transpose computed once per draw here instead of
                    // once per vertex in mesh.vert; correct under non-uniform
                    // scale.
                    object.normal_matrix = glm::transpose(glm::inverse(model));
                    object.material_index = item.material_index;
                    object.mesh_index = item.mesh_gpu_id;
                    object.flags = (static_cast<uint32_t>(item.pass_mask) & Object_Flag::Pass_Mask) | Object_Flag::Active;
                    object._padding0 = 0;

                    _out.push_back({ object_index, item.mesh_gpu_id, item_pipeline_id });
                }
            };

        // Opaque items take entries [0, opaque) and transparent items
        // continue from there. The culling pass relies on it: it processes
        // [0, opaque) only.
        add_items(_packet.opaque_items, CoreTypes::Render_Pass_Bit::Opaque, Render_Subpass::Opaque,
                  opaque_draws, false, nullptr);
        add_items(_packet.transparent_items, CoreTypes::Render_Pass_Bit::Transparent, Render_Subpass::Transparent,
                  transparent_draws, _cull_transparents, &transparent_candidates);
    }

    void Draw_List_Builder::Write_cpu_draw_commands(VkDrawIndexedIndirectCommand* _commands, const Mesh_Registry& _meshes) const
    {
        assert(_commands != nullptr && "Draw_List_Builder::Write_cpu_draw_commands: the command buffer of the frame is not mapped");

        // Same order as the opaque list, so the commands of one pipeline
        // are contiguous. Host-coherent memory written before the submit:
        // no barrier.
        for (size_t i = 0; i < opaque_draws.size(); ++i)
            _commands[i] = _meshes.Get(opaque_draws[i].mesh_id).Make_indirect_command(opaque_draws[i].object_index);
    }

    void Draw_List_Builder::Group_opaque_by_pipeline()
    {
        opaque_runs.clear();

        size_t run_begin = 0;

        while (run_begin < opaque_draws.size())
        {
            const uint8_t pipeline_id = opaque_draws[run_begin].pipeline_id;
            size_t        run_end = run_begin + 1;

            while (run_end < opaque_draws.size() && opaque_draws[run_end].pipeline_id == pipeline_id)
                ++run_end;

            opaque_runs.push_back({ pipeline_id, static_cast<uint32_t>(run_begin), static_cast<uint32_t>(run_end - run_begin) });

            run_begin = run_end;
        }
    }

} // namespace Renderer_System
