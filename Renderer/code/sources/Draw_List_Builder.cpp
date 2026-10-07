#include <Draw_List_Builder.hpp>
#include <Renderer_Limits.hpp>

#include <Matrix4.hpp>

#include <algorithm>
#include <cassert>
#include <iostream>

namespace Renderer_System
{

    Draw_List_Builder::Draw_List_Builder()
    {
        valid_opaque.reserve(MAX_OBJECTS);
        opaque_draws.reserve(MAX_OBJECTS);
        transparent_draws.reserve(MAX_OBJECTS);
        opaque_buckets.reserve(MAX_DRAW_BUCKETS);
    }

    void Draw_List_Builder::Request_frustum_capture()
    {
        capture_frozen_frustum = true;
    }

    void Draw_List_Builder::Update_culling_frustum(const Frustum& _view_frustum, bool _freeze)
    {
        if (capture_frozen_frustum)
        {
            frozen_frustum = _view_frustum;
            capture_frozen_frustum = false;
        }

        culling_frustum = _freeze ? frozen_frustum : _view_frustum;
    }

    bool Draw_List_Builder::Build_buckets(size_t _count, uint32_t _max_draw_indirect_count)
    {
        opaque_buckets.clear();

        // The items arrive grouped by pipeline (sort key), so the bucket of
        // an item is almost always the last one used: it is tried first.
        size_t last_bucket = 0;

        for (size_t i = 0; i < _count; ++i)
        {
            Valid_Opaque_Item& valid = valid_opaque[i];

            const auto matches = [&](const Draw_Bucket& _bucket)
                {
                    return _bucket.pipeline_id == valid.pipeline_id && _bucket.mirrored == valid.mirrored;
                };

            size_t found = opaque_buckets.size();

            if (last_bucket < opaque_buckets.size() && matches(opaque_buckets[last_bucket]))
            {
                found = last_bucket;
            }
            else
            {
                const auto it = std::find_if(opaque_buckets.begin(), opaque_buckets.end(), matches);

                if (it != opaque_buckets.end())
                    found = static_cast<size_t>(it - opaque_buckets.begin());
            }

            if (found == opaque_buckets.size())
            {
                if (opaque_buckets.size() >= MAX_DRAW_BUCKETS)
                {
                    opaque_buckets.clear();
                    return false;
                }

                Draw_Bucket bucket;
                bucket.pipeline_id = valid.pipeline_id;
                bucket.mirrored = valid.mirrored;
                opaque_buckets.push_back(bucket);
            }

            valid.bucket = static_cast<uint32_t>(found);
            ++opaque_buckets[found].capacity;
            last_bucket = found;
        }

        // One indirect draw takes at most maxDrawIndirectCount commands, and
        // a bucket is one draw.
        uint32_t next_command = 0;

        for (Draw_Bucket& bucket : opaque_buckets)
        {
            if (bucket.capacity > _max_draw_indirect_count)
            {
                opaque_buckets.clear();
                return false;
            }

            bucket.first_command = next_command;
            next_command += bucket.capacity;
        }

        return true;
    }

    void Draw_List_Builder::Build(const RenderPacket& _packet, Object_GPU* _objects,
                                  const Mesh_Registry& _meshes, const Pipeline_Registry& _pipelines,
                                  uint32_t _material_count, const Build_Settings& _settings)
    {
        assert(_objects != nullptr && "Draw_List_Builder::Build: the object buffer of the frame is not mapped");

        // Rewritten from entry 0 every frame: the GPU finished reading this
        // slot's copy before its slot was waited on.
        valid_opaque.clear();
        opaque_draws.clear();
        transparent_draws.clear();
        opaque_runs.clear();
        opaque_buckets.clear();
        opaque_candidates = 0;
        transparent_candidates = 0;
        opaque_path = _settings.requested_path;
        opaque_culled_on_gpu = false;

        uint32_t object_count = 0;

        // What went wrong in this frame, reported at the end of the build
        // (see the end of this function for when).
        bool object_overflow = false;
        bool gpu_path_fallback = false;

        uint32_t invalid_items = 0;

        struct First_Invalid_Item
        {
            uint8_t  pipeline_id = 0;
            uint32_t subpass = 0;
            uint32_t mesh_id = 0;
            uint32_t transform_idx = 0;
            uint32_t material_index = 0;
        };

        First_Invalid_Item first_invalid;

        // Whether _item can be drawn in _subpass; its pipeline id comes
        // back in _out_pipeline_id. Every reference is validated in every
        // build, before its entry is written: an out-of-range transform
        // index would read past the packet's array, an out-of-range
        // material index past the written slots of the material table, a
        // released mesh may already have lost its geometry, and a pipeline
        // of another subpass is invalid in this one. The first invalid item
        // of the frame is kept for the report.
        const auto validate = [&](const Draw_Item& _item, uint32_t _subpass, uint8_t& _out_pipeline_id) -> bool
            {
                _out_pipeline_id = Get_pipeline_id(_item.sort_key);
                const VkPipeline pipeline = _pipelines.Get_by_id(_out_pipeline_id);

                const bool valid =
                    pipeline != VK_NULL_HANDLE &&
                    _pipelines.Get_subpass(_out_pipeline_id) == _subpass &&
                    _meshes.Is_drawable(_item.mesh_gpu_id) &&
                    _item.transform_idx < _packet.transform_count &&
                    _item.material_index < _material_count;

                if (!valid)
                {
                    if (invalid_items == 0)
                    {
                        first_invalid.pipeline_id = _out_pipeline_id;
                        first_invalid.subpass = _subpass;
                        first_invalid.mesh_id = _item.mesh_gpu_id;
                        first_invalid.transform_idx = _item.transform_idx;
                        first_invalid.material_index = _item.material_index;
                    }

                    ++invalid_items;
                }

                return valid;
            };

        // -- Object entry --
        // Its index is the draw's firstInstance, which the vertex shader
        // receives as gl_InstanceIndex. No push constants: the shaders read
        // everything per draw from this entry, the material table and the
        // mesh table. Returns the index of the entry.
        const auto write_object = [&](const Draw_Item& _item, const MathLib::Matrix4& _model,
                                      bool _mirrored, uint32_t _bucket_bits) -> uint32_t
            {
                const uint32_t object_index = object_count++;

                Object_GPU& object = _objects[object_index];
                object.model = _model;
                // Normal matrix computed once per draw here instead of
                // once per vertex in mesh.vert; correct under non-uniform
                // scale, and finite for a collapsed object, where the
                // inverse-transpose it replaces is NaN (Mat4::Normal_matrix).
                // Not unit scale: the shaders normalize the normal.
                object.normal_matrix = MathLib::Matrix4(MathLib::Mat4::Normal_matrix(_model));
                object.material_index = _item.material_index;
                object.mesh_index = _item.mesh_gpu_id;
                object.flags = (static_cast<uint32_t>(_item.pass_mask) & Object_Flag::Pass_Mask) | Object_Flag::Active |
                               (_mirrored ? Object_Flag::Mirrored : 0u) | _bucket_bits;
                object._padding0 = 0;

                return object_index;
            };

        // CPU culling: the test of cull_objects.comp, the same formula on
        // the same planes (culling_frustum is what the UBO carries): the
        // mesh bounding sphere placed by the model matrix, an ellipsoid,
        // tested exactly against every plane, shear included.
        const auto outside_frustum = [&](const Draw_Item& _item, const MathLib::Matrix4& _model) -> bool
            {
                const Mesh_GPU& mesh = _meshes.Get(_item.mesh_gpu_id);

                return !culling_frustum.Intersects_ellipsoid(_model, MathLib::Vector4(mesh.bounds_center, mesh.bounds_radius));
            };

        // -- Opaque items: validation --
        // First the validated items alone: the path and the buckets are
        // decided from this list, so an invalid item cannot influence them.
        // An item that does not take part in the pass is skipped, so a
        // pass_mask actually selects passes instead of being decoration.
        for (const Draw_Item& item : _packet.opaque_items)
        {
            if ((item.pass_mask & Render_Pass_Bit::Opaque) == 0)
                continue;

            uint8_t pipeline_id = 0;

            if (!validate(item, Render_Subpass::Opaque, pipeline_id))
                continue;

            Valid_Opaque_Item valid;
            valid.item = &item;
            valid.pipeline_id = pipeline_id;
            valid.mirrored = Inverts_winding(_packet.transforms[item.transform_idx]);
            valid_opaque.push_back(valid);
        }

        opaque_candidates = static_cast<uint32_t>(valid_opaque.size());

        // -- Opaque items: path --
        // A GPU path draws the objects that fit the object buffer, all of
        // them (the culling pass decides on the GPU which survive), grouped
        // in buckets.
        if (Is_gpu_draw_path(_settings.requested_path))
        {
            const size_t drawable = std::min<size_t>(valid_opaque.size(), MAX_OBJECTS);

            if (!Build_buckets(drawable, _settings.max_draw_indirect_count))
            {
                gpu_path_fallback = true;
                opaque_path = Opaque_Draw_Path::Cpu_Indirect;
            }
        }

        opaque_culled_on_gpu = Is_gpu_draw_path(opaque_path) && _settings.frustum_culling;

        // The opaque objects are culled on the CPU only when no GPU path
        // draws them.
        const bool cull_opaque_on_cpu = _settings.frustum_culling && !Is_gpu_draw_path(opaque_path);

        // -- Opaque items: entries [0, opaque) --
        for (const Valid_Opaque_Item& valid : valid_opaque)
        {
            const Draw_Item& item = *valid.item;
            const MathLib::Matrix4&     model = _packet.transforms[item.transform_idx];

            if (cull_opaque_on_cpu && outside_frustum(item, model))
                continue;

            // The object buffer is full: this and every later item of the
            // frame are skipped.
            if (object_count >= MAX_OBJECTS)
            {
                object_overflow = true;
                break;
            }

            const uint32_t bucket_bits = Is_gpu_draw_path(opaque_path) ? Object_Flag::Make_bucket_bits(valid.bucket) : 0u;
            const uint32_t object_index = write_object(item, model, valid.mirrored, bucket_bits);

            opaque_draws.push_back({ object_index, item.mesh_gpu_id, valid.pipeline_id, valid.mirrored });
        }

        // -- Transparent items: entries continue from the opaque ones --
        // The culling pass relies on it: it processes [0, opaque) only.
        for (const Draw_Item& item : _packet.transparent_items)
        {
            if ((item.pass_mask & Render_Pass_Bit::Transparent) == 0)
                continue;

            uint8_t pipeline_id = 0;

            if (!validate(item, Render_Subpass::Transparent, pipeline_id))
                continue;

            ++transparent_candidates;

            const MathLib::Matrix4& model = _packet.transforms[item.transform_idx];

            if (_settings.frustum_culling && outside_frustum(item, model))
                continue;

            if (object_count >= MAX_OBJECTS)
            {
                object_overflow = true;
                break;
            }

            const bool     mirrored = Inverts_winding(model);
            const uint32_t object_index = write_object(item, model, mirrored, 0u);

            transparent_draws.push_back({ object_index, item.mesh_gpu_id, pipeline_id, mirrored });
        }

        // -- Report --
        // What went wrong in this frame is reported when it starts, not on
        // every frame it lasts: a report is about an episode, never about
        // the whole session, or a second problem would go unnoticed behind
        // the first. An episode ends after Episode_Report::REARM_FRAMES
        // frames without the problem, so one that flickers is reported once,
        // not at every appearance. The skipped items add that a number
        // larger than any reported is news within the episode: a mesh
        // released while the stale items of an earlier one are still being
        // sent is a new problem.
        if (invalid_items_report.Update(invalid_items))
        {
            std::cerr << "[Renderer] " << invalid_items << " draw item(s) skipped; the first: pipeline " << int(first_invalid.pipeline_id)
                << " (built for subpass " << _pipelines.Get_subpass(first_invalid.pipeline_id) << ", drawn in subpass "
                << first_invalid.subpass << "), mesh " << first_invalid.mesh_id << ", transform " << first_invalid.transform_idx
                << ", material " << first_invalid.material_index
                << " (registered meshes: " << _meshes.Get_count() << ", transforms in packet: "
                << _packet.transform_count << ", registered materials: " << _material_count
                << "; a released mesh is also skipped).\n";
        }

        if (object_overflow_report.Update(object_overflow ? 1u : 0u))
        {
            std::cerr << "[Renderer] More than " << MAX_OBJECTS << " draws in one frame: the rest are skipped. "
                "Raise MAX_OBJECTS in Renderer_Limits.hpp.\n";
        }

        if (gpu_path_fallback_report.Update(gpu_path_fallback ? 1u : 0u))
        {
            std::cerr << "[Renderer] The GPU draw path needs the opaque items in at most " << MAX_DRAW_BUCKETS
                << " groups (one per pipeline and triangle winding), none with more than maxDrawIndirectCount ("
                << _settings.max_draw_indirect_count << ") objects; frames that break it use CPU-written indirect "
                "commands instead.\n";
        }
    }

    bool Draw_List_Builder::Episode_Report::Update(uint32_t _size)
    {
        if (_size == 0)
        {
            if (clean_frames < REARM_FRAMES)
                ++clean_frames;

            // The episode is over: its next appearance reports again.
            if (clean_frames >= REARM_FRAMES)
                reported_size = 0;

            return false;
        }

        clean_frames = 0;

        if (_size <= reported_size)
            return false;

        reported_size = _size;

        return true;
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
            const bool    mirrored = opaque_draws[run_begin].mirrored;
            size_t        run_end = run_begin + 1;

            // A run ends when the pipeline or the winding changes: both are
            // state the recording sets between the draws.
            while (run_end < opaque_draws.size() &&
                   opaque_draws[run_end].pipeline_id == pipeline_id &&
                   opaque_draws[run_end].mirrored == mirrored)
                ++run_end;

            opaque_runs.push_back({ pipeline_id, mirrored, static_cast<uint32_t>(run_begin), static_cast<uint32_t>(run_end - run_begin) });

            run_begin = run_end;
        }
    }

} // namespace Renderer_System
