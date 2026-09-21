#include <Extractor.hpp>

#include <World.hpp>
#include <Transform_Component.hpp>
#include <Mesh_Component.hpp>
#include <Light_Component.hpp>
#include <Camera_Component.hpp>
#include <Resource_Manager.hpp>
#include <Matrix4.hpp>
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace EngineCore
{

    bool Extractor::Extract(const ECS::World& _world,
        const ResourceManager::Resource_Manager& _resources,
        float                                     _aspect_ratio,
        uint8_t                                   _opaque_pipeline_id,
        CoreTypes::RenderPacket& _out_packet)
    {
        // =========================================================
        // Reset packet
        // =========================================================
       
        _out_packet.opaque_items.clear();
        _out_packet.transparent_items.clear();
        _out_packet.lights.clear();
        transform_buffer.clear();

        // =========================================================
        // 1. Find active camera
        // =========================================================

        // Search for the Camera_Component with is_active=true and the
        // lowest render_order. Store the winning entity and component.
       
        const ECS::Camera_Component* camera_comp = nullptr;
        const ECS::Transform_Component* camera_transform = nullptr;
        int                             best_order = std::numeric_limits<int>::max();

        _world.Each<ECS::Camera_Component>(
            [&](ECS::Entity entity, const ECS::Camera_Component& cam)
            {
                if (!cam.is_active)           return;
                if (cam.render_order >= best_order) return;

                const ECS::Transform_Component* t =
                    _world.Try_get_component<ECS::Transform_Component>(entity);
                if (!t) return;

                best_order = cam.render_order;
                camera_comp = &cam;
                camera_transform = t;
            });

        // No active camera — caller should skip Render this frame.
        if (!camera_comp) return false;

        // =========================================================
        // 2. Build RenderView
        // =========================================================

        // View matrix: inverse of the camera's world transform.
        // glm::lookAt built from the camera's world position and direction.
        const MathLib::Vector3 cam_pos = camera_transform->position;
        const MathLib::Vector3 cam_forward = camera_transform->Forward();
        const MathLib::Vector3 cam_up = camera_transform->Up();

        const MathLib::Matrix4 view =
            glm::lookAt(cam_pos, cam_pos + cam_forward, cam_up);

        // Effective aspect ratio: camera override or viewport default.
        const float aspect = (camera_comp->aspect_ratio > 0.0f)
            ? camera_comp->aspect_ratio
            : _aspect_ratio;

        // Projection matrix.
        MathLib::Matrix4 projection;

        if (camera_comp->projection == ECS::Camera_Component::Projection::Perspective)
        {
            projection = MathLib::Mat4::Perspective_reverse_z_infinite( glm::radians(camera_comp->fov),aspect,  camera_comp->near_plane);
        }
        else
        {
  
            const float h = camera_comp->ortho_size;
            const float w = h * aspect;

            projection = MathLib::Mat4::Reverse_z_correction() * glm::ortho(-w, w, -h, h,camera_comp->near_plane,camera_comp->far_plane);
        }

        // Vulkan clip space has Y flipped compared to OpenGL.
        // Flip the Y column of the projection to correct NDC orientation.
        // Independent of the Z reversal above - different axis, both needed.
        projection[1][1] *= -1.0f;

        _out_packet.view.view = view;
        _out_packet.view.projection = projection;
        _out_packet.view.view_projection = projection * view;
        _out_packet.view.camera_position = cam_pos;
        _out_packet.view.near_plane = camera_comp->near_plane;
        _out_packet.view.far_plane =(camera_comp->projection == ECS::Camera_Component::Projection::Perspective)
                                      ? std::numeric_limits<float>::infinity() : camera_comp->far_plane;

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

               
                const uint32_t gpu_id = _resources.Get_gpu_id(mesh_comp.mesh);

                if (gpu_id == ResourceManager::Resource_Manager::INVALID_GPU_ID)
                {
                    return;   
                }

                // Store the world matrix and record its index.
                const uint32_t transform_idx =
                    static_cast<uint32_t>(transform_buffer.size());
                transform_buffer.push_back(transform.world_matrix);

                // Compute depth (distance from camera) for sort key.
                const MathLib::Vector3 delta =
                    transform.position - cam_pos;
                const float depth = glm::dot(delta, cam_forward);
                const uint32_t depth_bits = CoreTypes::Depth_to_sortable_bits(depth);

                // Pack sort key: material=0, pipeline=0, mesh, depth.
                const uint64_t sort_key = CoreTypes::Make_sort_key(_opaque_pipeline_id, 0, static_cast<uint16_t>(gpu_id & 0xFFFF),depth_bits);
                        
        
                CoreTypes::Draw_Item item{};
                item.mesh_gpu_id = gpu_id;
                item.material_id = 0;       // default material — Fase 2
                item.transform_idx = transform_idx;
                item.pass_mask = 0xFF;    // all passes
                item.sort_key = sort_key;

                _out_packet.opaque_items.push_back(item);
            });

        // Point the packet's transform pointer at our buffer.
        _out_packet.transforms = transform_buffer.data();
        _out_packet.transform_count = static_cast<uint32_t>(transform_buffer.size());

        // Sort opaque items front-to-back (minimizes overdraw).
        std::sort(_out_packet.opaque_items.begin(),
            _out_packet.opaque_items.end(),
            [](const CoreTypes::Draw_Item& a, const CoreTypes::Draw_Item& b)
            {
                return a.sort_key < b.sort_key;
            });

        // =========================================================
        // 4. Build GPU_Light array from (Transform + Light) entities
        // =========================================================

        _world.Query<ECS::Light_Component, ECS::Transform_Component>(
            [&](ECS::Entity                    entity,
                const ECS::Light_Component& light_comp,
                const ECS::Transform_Component& transform)
            {
                CoreTypes::GPU_Light gpu_light{};
                gpu_light.color = light_comp.color;
                gpu_light.intensity = light_comp.intensity;
                gpu_light.range = light_comp.range;
                gpu_light.inner_angle = light_comp.inner_angle;
                gpu_light.outer_angle = light_comp.outer_angle;

                switch (light_comp.type)
                {
                case ECS::Light_Component::Type::Directional:
                    // Hacia donde APUNTA la luz. El shader la niega para su L.
                    gpu_light.position_or_direction = transform.Forward();
                    gpu_light.type = 0;
                    break;

                case ECS::Light_Component::Type::Point:
                    gpu_light.position_or_direction = transform.position;
                    gpu_light.type = 1;
                    break;

                case ECS::Light_Component::Type::Spot:
                    gpu_light.position_or_direction = transform.position;
                    gpu_light.spot_direction = transform.Forward();
                    gpu_light.type = 2;
                    break;
                }

                _out_packet.lights.push_back(gpu_light);
            });

        
        return true;
    }

} // namespace EngineCore