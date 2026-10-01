#include <Extractor.hpp>

#include <World.hpp>
#include <Transform_Component.hpp>
#include <Mesh_Component.hpp>
#include <Material_Component.hpp>
#include <Light_Component.hpp>
#include <Camera_Component.hpp>
#include <Resource_Manager.hpp>
#include <Vector3.hpp>
#include <Matrix4.hpp>
#include <MathConstants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace EngineCore
{

    namespace
    {
        // Plane through _point with unit normal _normal pointing inside:
        // xyz = normal, w = -dot(normal, point), so dot(xyz, p) + w is the
        // signed distance of p (positive inside).
        MathLib::Vector4 Make_plane(const MathLib::Vector3& _normal, const MathLib::Vector3& _point)
        {
            return MathLib::Vector4(_normal, -MathLib::Vec3::Dot(_normal, _point));
        }

        // World space culling planes of a camera, built from its parameters
        // rather than extracted from view_projection: with reverse-Z and an
        // infinite far plane the classic extraction yields a degenerate far
        // plane and signs that differ from the usual derivation.
        //
        // _right, _up, _forward: orthonormal world space basis of the view,
        // the same one Look_at builds. Order of the planes: left, right,
        // bottom, top, near (CoreTypes::Frustum).
        CoreTypes::Frustum Make_camera_frustum(const ECS::Camera_Component& _camera,
            const MathLib::Vector3& _position,
            const MathLib::Vector3& _right,
            const MathLib::Vector3& _up,
            const MathLib::Vector3& _forward,
            float _aspect)
        {
            CoreTypes::Frustum frustum;

            if (_camera.projection == ECS::Camera_Component::Projection::Perspective)
            {
                // Side planes through the eye. A point at distance d along
                // the view direction is inside horizontally when
                // |x| <= d * tan_half_width: the plane normals lean towards
                // the view direction by that slope.
                const float tan_half_height = std::tan(_camera.fov * MathLib::Constants::DEG_TO_RAD * 0.5f);
                const float tan_half_width = tan_half_height * _aspect;

                frustum.planes[0] = Make_plane(MathLib::Vec3::Normalize(_right + _forward * tan_half_width), _position);   // left
                frustum.planes[1] = Make_plane(MathLib::Vec3::Normalize(-_right + _forward * tan_half_width), _position);   // right
                frustum.planes[2] = Make_plane(MathLib::Vec3::Normalize(_up + _forward * tan_half_height), _position);     // bottom
                frustum.planes[3] = Make_plane(MathLib::Vec3::Normalize(-_up + _forward * tan_half_height), _position);     // top
            }
            else
            {
                // Parallel side planes at the half extents of the box.
                const float half_height = _camera.ortho_size;
                const float half_width = half_height * _aspect;

                frustum.planes[0] = Make_plane( _right, _position - _right * half_width);    // left
                frustum.planes[1] = Make_plane(-_right, _position + _right * half_width);    // right
                frustum.planes[2] = Make_plane( _up, _position - _up * half_height);         // bottom
                frustum.planes[3] = Make_plane(-_up, _position + _up * half_height);         // top
            }

            // Near plane, facing the view direction.
            frustum.planes[4] = Make_plane(_forward, _position + _forward * _camera.near_plane);

            return frustum;
        }
    }

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
        _out_packet.directional_light_count = 0;
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
        _out_packet.view.near_plane = camera_comp->near_plane;
        _out_packet.clear_color = camera_comp->clear_color;

        // Culling planes, from the same basis Look_at builds: forward, the
        // right vector cross(forward, up) and the up vector re-orthogonalized
        // from both, so the planes match the view matrix exactly.
        {
            const MathLib::Vector3 view_forward = MathLib::Vec3::Normalize(cam_forward);
            const MathLib::Vector3 view_right = MathLib::Vec3::Normalize(MathLib::Vec3::Cross(view_forward, cam_up));
            const MathLib::Vector3 view_up = MathLib::Vec3::Cross(view_right, view_forward);

            _out_packet.view.frustum = Make_camera_frustum(*camera_comp, cam_pos, view_right, view_up, view_forward, aspect);
        }

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

                // Optional material, already registered in the Renderer's
                // material table by the Engine: the item only carries its
                // slot. Without a material, or with one not registered yet,
                // the item draws with the default material, which looks
                // exactly like an untextured, untinted draw. The textures
                // were resolved to bindless indices at registration, not
                // here every frame.
                CoreTypes::Draw_Item item{};
                item.mesh_gpu_id = gpu_id;
                item.material_index = CoreTypes::Default_Material;

                // Alpha of the material tint: what routes the item to the
                // opaque or the transparent pass below.
                float base_alpha = 1.0f;

                if (const ECS::Material_Component* material = _world.Try_get_component<ECS::Material_Component>(entity))
                {
                    if (material->gpu_material_id != ECS::Material_Component::INVALID_GPU_MATERIAL_ID)
                        item.material_index = material->gpu_material_id;

                    base_alpha = material->base_color_factor.a;
                }

                // Store the world matrix and record its index.
                item.transform_idx = static_cast<uint32_t>(transform_buffer.size());
                transform_buffer.push_back(transform.world_matrix);

                // Depth along the view direction, from the WORLD position:
                // the same space the matrix that draws the item lives in.
                const float depth = MathLib::Vec3::Dot(transform.World_position() - cam_pos, cam_forward);

                // Alpha below one routes the item to the transparent pass:
                // accumulated by the weighted blended OIT, depth tested
                // without writes. The composite does not depend on the
                // draw order, so the key groups the item like an opaque one
                // (pipeline, material, mesh, then front-to-back).
                const bool transparent = base_alpha < 1.0f;

                // Objects whose transform inverts the winding are drawn with
                // the opposite front face: the key keeps them together, right
                // below their pipeline, so the front face changes as rarely
                // as possible. Only an ordering hint; the Renderer decides
                // the winding from the transform itself.
                const uint8_t winding_bits = CoreTypes::Inverts_winding(transform.world_matrix)
                    ? CoreTypes::Sort_Key_Mirrored_Bit
                    : uint8_t{ 0 };

                if (transparent)
                {
                    item.pass_mask = CoreTypes::Render_Pass_Bit::Transparent;
                    item.sort_key = CoreTypes::Make_sort_key(_params.transparent_pipeline_id, winding_bits,
                        static_cast<uint16_t>(gpu_id & 0xFFFF), CoreTypes::Depth_to_sortable_bits(depth));

                    _out_packet.transparent_items.push_back(item);
                }
                else
                {
                    item.pass_mask = CoreTypes::Render_Pass_Bit::Opaque;
                    item.sort_key = CoreTypes::Make_sort_key(_params.opaque_pipeline_id, winding_bits,
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

        // Both lists: grouped by pipeline/winding/mesh, front-to-back
        // inside each group. The transparent list needs no back-to-front
        // order: the Renderer composites it order-independently, and the
        // depth bits only keep its order deterministic between frames.
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

        // Directional lights first, each group keeping the query order.
        // They reach every fragment and are never clustered, so the
        // Renderer cuts the array at MAX_LIGHTS knowing it drops local
        // lights and never a sun.
        const auto first_local = std::stable_partition(
            _out_packet.lights.begin(), _out_packet.lights.end(),
            [](const CoreTypes::GPU_Light& _light) { return _light.type == 0; });

        _out_packet.directional_light_count =
            static_cast<uint32_t>(first_local - _out_packet.lights.begin());

        return true;
    }

} // namespace EngineCore
