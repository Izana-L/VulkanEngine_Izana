#pragma once

#include <Image_Loader.hpp>
#include <Mesh_Loader.hpp>
#include <Asset_Handle.hpp>
#include <Id_Provider.hpp>
#include <ImageData.hpp>
#include <MeshData.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ResourceManager
{

    // Resource_Manager: loads assets from disk and maintains the mapping
    // from Asset_Handle to CPU data and GPU ids.
    //
    // Sits in Layer 1 — knows nothing about Vulkan or the Renderer.
    // EngineCore (Layer 3) bridges the two:
    //
    //   auto handles = resource_manager.Load_mesh("model.glb");
    //   for (auto handle : handles)
    //   {
    //       uint32_t gpu_id = renderer.Upload_mesh(
    //           resource_manager.Get_mesh_data(handle));
    //       resource_manager.Register_gpu_id(handle, gpu_id);
    //   }
    //
    // After that, the extract can resolve handles to gpu_ids:
    //   uint32_t gpu_id = resource_manager.Get_gpu_id(handle);
    class Resource_Manager
    {
    public:

        Resource_Manager() = default;
        ~Resource_Manager() = default;

        Resource_Manager(const Resource_Manager&) = delete;
        Resource_Manager& operator=(const Resource_Manager&) = delete;
        Resource_Manager(Resource_Manager&&) = default;
        Resource_Manager& operator=(Resource_Manager&&) = default;

        // =========================================================
        // Mesh
        // =========================================================

        // Loads all triangle primitives from a glTF or GLB file.
        // Returns one Asset_Handle per primitive found.
        // gpu_id is not set yet — call Register_gpu_id() after uploading
        // each mesh to the Renderer.
        std::vector<CoreTypes::Asset_Handle>
            Load_mesh(const std::string& _path);

        // Registers the gpu_id returned by Renderer::Upload_mesh() for
        // the given handle. Must be called before Get_gpu_id().
        void Register_gpu_id(CoreTypes::Asset_Handle _handle, uint32_t _gpu_id);

        // Resolves a mesh handle to its GPU id.
        // Asserts in debug if the handle is invalid or gpu_id has not
        // been registered yet.
        uint32_t Get_gpu_id(CoreTypes::Asset_Handle _handle) const;

        // Returns the CPU-side mesh data for a given handle.
        // Useful for debug, reimport, or physics (collision mesh).
        const CoreTypes::MeshData& Get_mesh_data(CoreTypes::Asset_Handle _handle) const;

        // =========================================================
        // Image
        // =========================================================

        // Loads an image from disk and returns a handle to it.
        // The image is kept in CPU memory — GPU upload happens in Fase 2
        // when Renderer::Upload_texture() is implemented.
        CoreTypes::Asset_Handle
            Load_image(const std::string& _path,
                CoreTypes::Pixel_Format  _format =
                CoreTypes::Pixel_Format::RGBA8_SRGB);

        // Returns the CPU-side image data for a given handle.
        const CoreTypes::ImageData& Get_image_data(CoreTypes::Asset_Handle _handle) const;

    private:

        // =========================================================
        // Internal types
        // =========================================================

        static constexpr uint32_t INVALID_GPU_ID = std::numeric_limits<uint32_t>::max();

        struct Mesh_Entry
        {
            CoreTypes::MeshData data;
            uint32_t            gpu_id = INVALID_GPU_ID;
            uint32_t            generation = 0;
        };

        struct Image_Entry
        {
            CoreTypes::ImageData data;
            uint32_t             generation = 0;
            // gpu_id added in Fase 2
        };

        // =========================================================
        // Internal helpers
        // =========================================================

        // Validates a mesh handle and returns the corresponding entry.
        // Asserts in debug on invalid handle or generation mismatch.
        const Mesh_Entry& Get_mesh_entry(CoreTypes::Asset_Handle _handle) const;
        const Image_Entry& Get_image_entry(CoreTypes::Asset_Handle _handle) const;

        // =========================================================
        // Data
        // =========================================================

        CoreTypes::Id_Provider   mesh_id_provider;
        std::vector<Mesh_Entry>  meshes;

        CoreTypes::Id_Provider   image_id_provider;
        std::vector<Image_Entry> images;
    };

} // namespace ResourceManager