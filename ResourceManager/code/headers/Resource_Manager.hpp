#pragma once

#include <Image_Loader.hpp>
#include <Mesh_Loader.hpp>
#include <Primitive_Builder.hpp>
#include <Primitive_Desc.hpp>
#include <Asset_Handle.hpp>
#include <Id_Provider.hpp>
#include <Fnv.hpp>
#include <ImageData.hpp>
#include <MeshData.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace ResourceManager
{

    // Resource_Manager: loads, generates, deduplicates and tracks assets.
    //
    // Sits in Layer 1 — knows nothing about Vulkan or the Renderer.
    // EngineCore (Layer 3) bridges to the Renderer for GPU upload.
    //
    // Deduplication: loading the same file twice, or requesting the same
    // primitive twice, returns the same Asset_Handle(s) without reloading
    // or regenerating. This mirrors Unity/Unreal: many entities share one
    // GPU mesh, differing only by their Transform.
    //
    // Cache keys (uint64_t):
    //   - File assets:  FNV64(path) combined with format, bit 63 = 0.
    //   - Primitives:   structured packing from Primitive_Desc, bit 63 = 1.
    //   The discriminator bit keeps the two key spaces from colliding.
    //   A stored source string allows a full comparison on hash hit to
    //   rule out the (rare) FNV collision.
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
        // Mesh — file
        // =========================================================

        // Loads all primitives from a glTF/GLB file, or returns the
        // cached handles if this file was already loaded. One handle
        // per primitive. gpu_id must still be registered after upload.
        std::vector<CoreTypes::Asset_Handle>
            Load_mesh(const std::string& _path);

        // =========================================================
        // Mesh — primitive
        // =========================================================

        // Generates a procedural primitive, or returns the cached handle
        // if an identical primitive (same type + topology params) already
        // exists. Returns a single handle (primitives are one mesh each).
        CoreTypes::Asset_Handle
            Create_primitive(const Primitive_Desc& _desc);

        // =========================================================
        // Mesh — shared queries
        // =========================================================

        void     Register_gpu_id(CoreTypes::Asset_Handle _handle, uint32_t _gpu_id);
        uint32_t Get_gpu_id(CoreTypes::Asset_Handle _handle) const;

        const CoreTypes::MeshData&
            Get_mesh_data(CoreTypes::Asset_Handle _handle) const;

        // =========================================================
        // Image
        // =========================================================

        // Loads an image, or returns the cached handle if the same path
        // was already loaded with the same format. Format is part of the
        // key: the same file as SRGB vs UNORM are distinct GPU resources.
        CoreTypes::Asset_Handle
            Load_image(const std::string& _path,
                CoreTypes::Pixel_Format  _format =
                CoreTypes::Pixel_Format::RGBA8_SRGB);

        const CoreTypes::ImageData&
            Get_image_data(CoreTypes::Asset_Handle _handle) const;

    private:

        // =========================================================
        // Internal types
        // =========================================================

        static constexpr uint32_t INVALID_GPU_ID =
            std::numeric_limits<uint32_t>::max();

        struct Mesh_Entry
        {
            CoreTypes::MeshData data;
            uint32_t            gpu_id = INVALID_GPU_ID;
            uint32_t            generation = 0;
            uint32_t            ref_count = 0;    // reserved — no unload logic yet
            std::string         source;            // path or primitive description
        };

        struct Image_Entry
        {
            CoreTypes::ImageData data;
            uint32_t             generation = 0;
            uint32_t             ref_count = 0;    // reserved
            std::string          source;
            // gpu_id added in Fase 2
        };

        // =========================================================
        // Internal helpers
        // =========================================================

        const Mesh_Entry& Get_mesh_entry(CoreTypes::Asset_Handle _handle) const;
        const Image_Entry& Get_image_entry(CoreTypes::Asset_Handle _handle) const;

        // Creates a new mesh entry from ready MeshData, returns its handle.
        CoreTypes::Asset_Handle
            Register_mesh(CoreTypes::MeshData&& _data, const std::string& _source);

        // Builds a file-asset cache key: FNV64(path) folded with format,
        // bit 63 forced to 0 to stay in the file key space.
        static uint64_t Make_file_key(const std::string& _path);
        static uint64_t Make_image_key(const std::string& _path,
            CoreTypes::Pixel_Format  _format);

        // =========================================================
        // Data
        // =========================================================

        CoreTypes::Id_Provider   mesh_id_provider;
        std::vector<Mesh_Entry>  meshes;

        CoreTypes::Id_Provider   image_id_provider;
        std::vector<Image_Entry> images;

        // key -> handles. Mesh files yield several handles (one per
        // primitive); primitives yield exactly one (stored as a 1-element
        // vector for uniformity).
        std::unordered_map<uint64_t, std::vector<CoreTypes::Asset_Handle>> mesh_cache;
        std::unordered_map<uint64_t, CoreTypes::Asset_Handle>              image_cache;
    };

} // namespace ResourceManager