#pragma once

#include <Image_Loader.hpp>
#include <Mesh_Loader.hpp>
#include <Mesh_Optimizer.hpp>
#include <Primitive_Builder.hpp>
#include <Primitive_Desc.hpp>
#include <Asset_Handle.hpp>
#include <Id_Provider.hpp>
#include <ImageData.hpp>
#include <MeshData.hpp>

#include <cstdint>
#include <deque>
#include <limits>
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
        Resource_Manager(Resource_Manager&&) = default;
        Resource_Manager& operator=(Resource_Manager&&) = default;


        static constexpr uint32_t INVALID_GPU_ID = std::numeric_limits<uint32_t>::max();

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
        // The reference stays valid for the lifetime of the manager: mesh
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

    private:

        // =========================================================
        // Internal types
        // =========================================================
        struct Mesh_Entry
        {
            CoreTypes::MeshData data;
            uint32_t            gpu_id = INVALID_GPU_ID;
            uint32_t            generation = 0;
            std::string         source;            // path or primitive description
        };

        struct Image_Entry
        {
            CoreTypes::ImageData data;
            uint32_t             gpu_id = INVALID_GPU_ID;   // bindless texture index
            uint32_t             generation = 0;
            std::string          source;
        };

        // =========================================================
        // Internal helpers
        // =========================================================

        // nullptr for an invalid or stale handle.
        const Mesh_Entry* Find_mesh_entry(CoreTypes::Asset_Handle _handle) const;
        Mesh_Entry* Find_mesh_entry(CoreTypes::Asset_Handle _handle);
        const Image_Entry* Find_image_entry(CoreTypes::Asset_Handle _handle) const;
        Image_Entry* Find_image_entry(CoreTypes::Asset_Handle _handle);

        // Throwing variants, for accessors that must return a reference.
        const Mesh_Entry& Get_mesh_entry(CoreTypes::Asset_Handle _handle, const char* _operation) const;
        Mesh_Entry& Get_mesh_entry(CoreTypes::Asset_Handle _handle, const char* _operation);
        const Image_Entry& Get_image_entry(CoreTypes::Asset_Handle _handle, const char* _operation) const;
        Image_Entry& Get_image_entry(CoreTypes::Asset_Handle _handle, const char* _operation);

        // Creates a new mesh entry from ready MeshData, returns its handle.
        CoreTypes::Asset_Handle Register_mesh(CoreTypes::MeshData&& _data, const std::string& _source);

        CoreTypes::Asset_Handle Register_image(CoreTypes::ImageData&& _data, const std::string& _source);

        static std::string Make_image_key(const std::string& _path, CoreTypes::Pixel_Format _format);

        // =========================================================
        // Data
        // =========================================================

        // std::deque, not std::vector: Get_mesh_data() hands out references
        // into this container, and a deque never relocates existing
        // elements when it grows at the back.
        CoreTypes::Id_Provider  mesh_id_provider;
        std::deque<Mesh_Entry>  meshes;

        CoreTypes::Id_Provider  image_id_provider;
        std::deque<Image_Entry> images;

        std::unordered_map<std::string, std::vector<CoreTypes::Asset_Handle>> file_mesh_cache;
        std::unordered_map<uint64_t, CoreTypes::Asset_Handle>                 primitive_cache;
        std::unordered_map<std::string, CoreTypes::Asset_Handle>              image_cache;
    };

} // namespace ResourceManager
