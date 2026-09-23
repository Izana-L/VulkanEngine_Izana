#include <Extractor.hpp>

#include <World.hpp>
#include <Transform_Component.hpp>
#include <Mesh_Component.hpp>
#include <Material_Component.hpp>
#include <Light_Component.hpp>
#include <Camera_Component.hpp>
#include <Resource_Manager.hpp>
#include <Matrix4.hpp>
#include <MathConstants.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace EngineCore
{

    bool Extractor::Extract(const ECS::World& _world,
        const ResourceManager::Resource_Manager& _resources,
        const Extract_Params& _params,
        CoreTypes::RenderPacket& _out_packet)
    {
        // =========================================================
        // Reset packet
        // =========================================================

        _out_packet.opaque_items.clear();
        _out_packet.transparent_items.clear();
        _out_packet.lights.clear();
        _out_packet.transforms = nullptr;
        _out_packet.transform_count = 0;
        transform_buffer.clear();

        // =========================================================
        // 1. Find active camera
        // =========================================================

        // Search for the Camera_Component with is_active=true and the
        // lowest render_order. A separate "found" flag, so a camera whose
        // render_order equals the largest int is still eligible.
        const ECS::Camera_Component* camera_comp = nullptr;
        const ECS::Transform_Component* camera_transform = nullptr;
        int                             best_order = std::numeric_limits<int>::max();

        _world.Each<ECS::Camera_Component>(
            [&](ECS::Entity entity, const ECS::Camera_Component& cam)
            {
                if (!cam.is_active) return;
                if (camera_comp != nullptr && cam.render_order >= best_order) return;

                const ECS::Transform_Component* t =
                    _world.Try_get_component<ECS::Transform_Component>(entity);
                if (!t) return;

                best_order = cam.render_order;
                camera_comp = &cam;
                camera_transform = t;
            });

        // No active camera: caller should skip Render this frame.
        if (!camera_comp) return false;

        // =========================================================
        // 2. Build RenderView (world space)
        // =========================================================

        const MathLib::Vector3 cam_pos = camera_transform->World_position();
        const MathLib::Vector3 cam_forward = camera_transform->World_forward();
        const MathLib::Vector3 cam_up = camera_transform->World_up();

        const MathLib::Matrix4 view = MathLib::Mat4::Look_at(cam_pos, cam_pos + cam_forward, cam_up);

        // Effective aspect ratio: camera override or viewport default.
        const float aspect = (camera_comp->aspect_ratio > 0.0f)
            ? camera_comp->aspect_ratio
            : _params.aspect_ratio;

        // Projection matrix (Reverse-Z, see Matrix4.hpp).
        MathLib::Matrix4 projection;

        if (camera_comp->projection == ECS::Camera_Component::Projection::Perspective)
        {
            projection = MathLib::Mat4::Perspective_reverse_z_infinite(
                camera_comp->fov * MathLib::Constants::DEG_TO_RAD, aspect, camera_comp->near_plane);
        }
        else
        {
            const float h = camera_comp->ortho_size;
            const float w = h * aspect;

            projection = MathLib::Mat4::Reverse_z_correction() *
                MathLib::Mat4::Orthographic(-w, w, -h, h, camera_comp->near_plane, camera_comp->far_plane);
        }

        // Vulkan clip space has Y flipped compared to OpenGL.
        // Flip the Y column of the projection to correct NDC orientation.
        // Independent of the Z reversal above: different axis, both needed.
        projection[1][1] *= -1.0f;

        _out_packet.view.view = view;
        _out_packet.view.projection = projection;
        _out_packet.view.view_projection = projection * view;
        _out_packet.view.camera_position = cam_pos;
        _out_packet.view.camera_forward = cam_forward;
        _out_packet.clear_color = camera_comp->clear_color;

        // =========================================================
        // 3. Build Draw_Items from (Transform + Mesh) entities
        // =========================================================

        _world.Query<ECS::Mesh_Component, ECS::Transform_Component>(
            [&](ECS::Entity                    entity,
                const ECS::Mesh_Component& mesh_comp,
                const ECS::Transform_Component& transform)
            {
                // Skip entities with no mesh assigned.
                if (!mesh_comp.mesh.Is_valid()) return;

                // INVALID_GPU_ID covers "never uploaded" and "stale handle".
                const uint32_t gpu_id = _resources.Get_gpu_id(mesh_comp.mesh);

                if (gpu_id == ResourceManager::Resource_Manager::INVALID_GPU_ID) return;

                // Optional material: per-draw tint, albedo texture and the
                // sampler preset it is read with. Without one the item draws
                // white, untextured, with the default sampler.
                CoreTypes::Draw_Item item{};
                item.mesh_gpu_id = gpu_id;
                item.base_color = { 1.0f, 1.0f, 1.0f, 1.0f };
                item.albedo_texture_index = CoreTypes::INVALID_TEXTURE_INDEX;
                item.albedo_sampler_index = static_cast<uint32_t>(CoreTypes::Sampler_Preset::Linear_Repeat);

                if (const ECS::Material_Component* material = _world.Try_get_component<ECS::Material_Component>(entity))
                {
                    item.base_color = material->base_color_factor;

                    // The preset value is already the slot in the bindless
                    // sampler array; no translation is needed.
                    item.albedo_sampler_index = static_cast<uint32_t>(material->sampler);

                    if (material->albedo.Is_valid())
                    {
                        const uint32_t texture_index = _resources.Get_image_gpu_id(material->albedo);

                        if (texture_index != ResourceManager::Resource_Manager::INVALID_GPU_ID)
                            item.albedo_texture_index = texture_index;
                    }
                }

                // Store the world matrix and record its index.
                item.transform_idx = static_cast<uint32_t>(transform_buffer.size());
                transform_buffer.push_back(transform.world_matrix);

                // Depth along the view direction, from the WORLD position:
                // the same space the matrix that draws the item lives in.
                const float depth = glm::dot(transform.World_position() - cam_pos, cam_forward);

                // Alpha below one routes the item to the transparent pass:
                // blended pipeline, back-to-front order, no depth writes.
                const bool transparent = item.base_color.a < 1.0f;

                if (transparent)
                {
                    item.pass_mask = CoreTypes::Render_Pass_Bit::Transparent;
                    item.sort_key = CoreTypes::Make_sort_key(_params.transparent_pipeline_id, 0,
                        static_cast<uint16_t>(gpu_id & 0xFFFF), CoreTypes::Depth_to_sortable_bits_back_to_front(depth));

                    _out_packet.transparent_items.push_back(item);
                }
                else
                {
                    item.pass_mask = CoreTypes::Render_Pass_Bit::Opaque;
                    item.sort_key = CoreTypes::Make_sort_key(_params.opaque_pipeline_id, 0,
                        static_cast<uint16_t>(gpu_id & 0xFFFF), CoreTypes::Depth_to_sortable_bits(depth));

                    _out_packet.opaque_items.push_back(item);
                }
            });

        // Point the packet's transform pointer at our buffer.
        _out_packet.transforms = transform_buffer.data();
        _out_packet.transform_count = static_cast<uint32_t>(transform_buffer.size());

        const auto by_key = [](const CoreTypes::Draw_Item& a, const CoreTypes::Draw_Item& b)
            {
                return a.sort_key < b.sort_key;
            };

        // Opaque: grouped by pipeline/material/mesh, front-to-back inside
        // each group. Transparent: same grouping, back-to-front (the key
        // already carries the inverted depth bits).
        std::sort(_out_packet.opaque_items.begin(), _out_packet.opaque_items.end(), by_key);
        std::sort(_out_packet.transparent_items.begin(), _out_packet.transparent_items.end(), by_key);

        // =========================================================
        // 4. Build GPU_Light array from (Transform + Light) entities
        // =========================================================

        _world.Query<ECS::Light_Component, ECS::Transform_Component>(
            [&](ECS::Entity                    /*entity*/,
                const ECS::Light_Component& light_comp,
                const ECS::Transform_Component& transform)
            {
                CoreTypes::GPU_Light gpu_light{};
                gpu_light.color = light_comp.color;
                gpu_light.intensity = light_comp.intensity;
                gpu_light.range = light_comp.range;
                gpu_light.inner_angle = light_comp.inner_angle;
                gpu_light.outer_angle = light_comp.outer_angle;

                // World-space direction and position: a light under a
                // rotating parent must follow it.
                const MathLib::Vector3 world_forward = transform.World_forward();
                const MathLib::Vector3 world_position = transform.World_position();

                switch (light_comp.type)
                {
                case ECS::Light_Component::Type::Directional:
                    // The direction the light POINTS TO. The shader negates
                    // it to get its L vector.
                    gpu_light.position_or_direction = world_forward;
                    gpu_light.type = 0;
                    break;

                case ECS::Light_Component::Type::Point:
                    gpu_light.position_or_direction = world_position;
                    gpu_light.type = 1;
                    break;

                case ECS::Light_Component::Type::Spot:
                    gpu_light.position_or_direction = world_position;
                    gpu_light.spot_direction = world_forward;
                    gpu_light.type = 2;
                    break;
                }

                _out_packet.lights.push_back(gpu_light);
            });

        return true;
    }

} // namespace EngineCore
