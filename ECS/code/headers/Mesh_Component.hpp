#pragma once

#include <Asset_Handle.hpp>

namespace ECS
{

    // Mesh_Component: references a mesh asset loaded by the ResourceManager.
    //
    // The Asset_Handle is resolved to a gpu_id by the Extractor during the
    // extract phase. The Renderer never sees Asset_Handle — it only sees
    // gpu_ids from the RenderPacket.
    //
    // Note: in Fase 2 this component will be wrapped by Model_Component
    // alongside Material_Component. For now, a mesh renders with a default
    // material (flat white) until the material system is in place.
    struct Mesh_Component
    {
        // Handle to the mesh in the ResourceManager.
        // INVALID_ASSET_HANDLE means "no mesh assigned".
        // Set via ResourceManager::Load_mesh() or Create_primitive().
        CoreTypes::Asset_Handle mesh = CoreTypes::INVALID_ASSET_HANDLE;

        // Convenience constructor.
        explicit Mesh_Component(CoreTypes::Asset_Handle _mesh)
            : mesh(_mesh)
        {}

        // Default constructor — no mesh assigned.
        Mesh_Component() = default;
    };

} // namespace ECS