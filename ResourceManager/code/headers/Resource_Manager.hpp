#pragma once

#include <Asset_Table.hpp>
#include <Image_Loader.hpp>
#include <Mesh_Loader.hpp>
#include <Mesh_Optimizer.hpp>
#include <Primitive_Builder.hpp>
#include <Primitive_Desc.hpp>
#include <Asset_Handle.hpp>
#include <ImageData.hpp>
#include <MeshData.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ResourceManager
{

    // Resource_Manager: loads, generates, deduplicates and tracks assets.
    //
    // Sits in Layer 1: knows nothing about Vulkan or the Renderer.
    // EngineCore (Layer 3) bridges to the Renderer for GPU upload and
    // stores the resulting gpu ids here through Register_gpu_id /
    // Register_image_gpu_id, so every asset is uploaded at most once:
    // a cache hit hands back a handle whose gpu id is already set, and
    // Get_gpu_id() is what callers must check before uploading again.
    //
    // Deduplication: loading the same file twice, or requesting the same
    // primitive twice, returns the same Asset_Handle(s) without reloading
    // or regenerating. This mirrors Unity/Unreal: many entities share one
    // GPU mesh, differing only by their Transform.
    //
    // Cache keys:
    //   - Mesh files:  the path string itself.
    //   - Images:      the path string plus the requested pixel format.
    //   - Primitives:  the canonical Primitive_Desc packed into a uint64_t.
    //   Strings are compared in full, so two different paths can never
    //   share an entry, whatever their hashes are; the primitive key is
    //   one-to-one with the geometry by construction.
    //
    // Handles are generational: every accessor checks the handle's
    // generation against the slot's, in every build configuration. A
    // stale handle yields INVALID_GPU_ID from the id queries and throws
    // std::invalid_argument from the data accessors.
    class Resource_Manager
    {
    public:

        Resource_Manager() = default;
        ~Resource_Manager() = default;

        Resource_Manager(const Resource_Manager&) = delete;
        Resource_Manager& operator=(const Resource_Manager&) = delete;

        // Moving transfers every asset, handle validity and cache entry.
        // The defaulted moves are sound because nothing here holds an
        // address of its own storage (Id_Provider links its free list by
        // ID); the moved-from manager is empty and can be reused, and it
        // shares no state with the destination.
        Resource_Manager(Resource_Manager&&) = default;
        Resource_Manager& operator=(Resource_Manager&&) = default;


        static constexpr uint32_t INVALID_GPU_ID = ResourceManager::INVALID_GPU_ID;

        // =========================================================
        // Mesh: file
        // =========================================================

        // Loads all primitives from a glTF/GLB file, or returns the
        // cached handles if this file was already loaded. One handle
        // per primitive. gpu_id must still be registered after upload.
        std::vector<CoreTypes::Asset_Handle> Load_mesh(const std::string& _path);

        // =========================================================
        // Mesh: primitive
        // =========================================================

        // Generates a procedural primitive, or returns the cached handle
        // if an identical primitive (same type + topology params) already
        // exists. Returns a single handle (primitives are one mesh each).
        CoreTypes::Asset_Handle Create_primitive(const Primitive_Desc& _desc);

        // =========================================================
        // Mesh: shared queries
        // =========================================================

        // Stores the Renderer's gpu id for a mesh. Throws
        // std::invalid_argument for a stale/invalid handle and
        // std::logic_error if the mesh already has a gpu id: a second
        // upload of the same mesh would leak the first GPU buffer.
        void     Register_gpu_id(CoreTypes::Asset_Handle _handle, uint32_t _gpu_id);

        // INVALID_GPU_ID when the mesh was never uploaded, or when the
        // handle is invalid or stale.
        uint32_t Get_gpu_id(CoreTypes::Asset_Handle _handle) const;

        // True if the handle addresses a live mesh entry (id in range and
        // generation matching).
        bool     Is_mesh_handle_valid(CoreTypes::Asset_Handle _handle) const;

        // Throws std::invalid_argument for an invalid or stale handle.
        // The reference stays valid for the lifetime of the manager: the
        // entries live in a std::deque, which never relocates its elements.
        const CoreTypes::MeshData& Get_mesh_data(CoreTypes::Asset_Handle _handle) const;

        // =========================================================
        // Image
        // =========================================================

        // Loads an image, or returns the cached handle if the same path
        // was already loaded with the same format. Format is part of the
        // key: the same file as SRGB vs UNORM are distinct GPU resources.
        CoreTypes::Asset_Handle Load_image(const std::string& _path, CoreTypes::Pixel_Format _format = CoreTypes::Pixel_Format::RGBA8_SRGB);

        // Same contract as the mesh counterparts. The image gpu id is the
        // Renderer's bindless texture index.
        void     Register_image_gpu_id(CoreTypes::Asset_Handle _handle, uint32_t _gpu_id);
        uint32_t Get_image_gpu_id(CoreTypes::Asset_Handle _handle) const;
        bool     Is_image_handle_valid(CoreTypes::Asset_Handle _handle) const;

        const CoreTypes::ImageData& Get_image_data(CoreTypes::Asset_Handle _handle) const;

        // Registers an image that already lives on the GPU and has no CPU
        // pixels: a texture generated by a compute pass, whose bindless
        // index _gpu_id comes from the Renderer (e.g.
        // Renderer::Get_procedural_texture_index). Returns a handle usable
        // like any other image handle (Material_Component slots,
        // Get_image_gpu_id), with the gpu id already registered.
        //
        // _name identifies the image in logs and error messages; it is not
        // a path and is not deduplicated. Get_image_data returns an empty
        // ImageData for such an image: there is nothing to upload.
        //
        // Throws std::invalid_argument if _gpu_id is INVALID_GPU_ID.
        CoreTypes::Asset_Handle Register_external_image(const std::string& _name, uint32_t _gpu_id);


    private:

        // =========================================================
        // Internal helpers
        // =========================================================

        static std::string Make_image_key(const std::string& _path, CoreTypes::Pixel_Format _format);

        // =========================================================
        // Data
        // =========================================================

        // One table per asset kind: slot storage, handle validation and
        // gpu id bookkeeping live in Asset_Table. Get_mesh_data() and
        // Get_image_data() hand out references into these tables, which
        // stay valid because the tables never relocate their entries.
        Asset_Table<CoreTypes::MeshData>  meshes{ "mesh" };
        Asset_Table<CoreTypes::ImageData> images{ "image" };

        std::unordered_map<std::string, std::vector<CoreTypes::Asset_Handle>> file_mesh_cache;
        std::unordered_map<uint64_t, CoreTypes::Asset_Handle>                 primitive_cache;
        std::unordered_map<std::string, CoreTypes::Asset_Handle>              image_cache;
    };

} // namespace ResourceManager
