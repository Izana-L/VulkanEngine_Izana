#pragma once

#include <Material_Desc.hpp>
#include <Render_Debug_Settings.hpp>
#include <Validation_Mode.hpp>

#include <ImageData.hpp>
#include <MeshData.hpp>
#include <RenderPacket.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace Platform { class Window; }

namespace Renderer_System
{
    // Public interface of the Renderer, the only part of the Vulkan
    // rendering stack that EngineCore knows about. Nothing in this header
    // depends on Vulkan, VMA or GLFW: every handle and every Vulkan type
    // lives behind the implementation pointer of Renderer, and the types
    // below are expressed in CoreTypes and plain values.

    // One texture in an upload batch. The format travels per-texture
    // because only the caller knows a texture's role: *_SRGB for color
    // data (albedo, emissive), *_UNORM for data maps (normal,
    // metallic-roughness, AO). Upload_texture(const ImageData&) uses
    // ImageData::format.
    struct Texture_Upload
    {
        const CoreTypes::ImageData* data = nullptr;
        CoreTypes::Pixel_Format     format = CoreTypes::Pixel_Format::RGBA8_SRGB;
    };

    // A set of assets to upload together in one command buffer and one
    // submit. Holds NON-OWNING pointers: the caller (ResourceManager) owns
    // the data and only has to keep it alive across the Upload_batch call.
    struct Upload_Batch
    {
        std::vector<const CoreTypes::MeshData*> meshes;
        std::vector<Texture_Upload>             textures;
    };

    // Result of Upload_batch. Each vector is parallel to its input list.
    struct Upload_Batch_Result
    {
        // Index into the Renderer's internal mesh registry: the value
        // ResourceManager stores through Register_gpu_id().
        std::vector<uint32_t> mesh_gpu_ids;

        // Index into the global bindless texture array (set 3, binding 0):
        // what shaders use, NOT the internal texture registry index.
        std::vector<uint32_t> texture_bindless_indices;
    };

    // Renderer: owns the entire Vulkan stack (instance -> device ->
    // swapchain -> pipelines) and all per-frame resources. Exposes
    // operations to EngineCore:
    //
    //   Upload_mesh()    - uploads geometry to the GPU once at load time.
    //   Upload_texture() - uploads a texture (with mipmaps) once at load time.
    //   Render()         - consumes a RenderPacket and produces one frame.
    //
    // Swapchain recreation happens for two reasons, both handled inside:
    //   - the window reported a resize (Notify_framebuffer_resized, called
    //     by the engine loop from Window::Consume_resized_flag);
    //   - the driver reported OUT_OF_DATE / SUBOPTIMAL from an acquire or a
    //     present.
    // In both cases Render only records that a recreation is pending and
    // skips or finishes its frame; the swapchain is rebuilt by
    // Recreate_swapchain_if_needed(), which the engine loop calls before it
    // extracts the frame, so the aspect ratio of the projection and the
    // viewport of the frame come from the same extent. Rebuilding never
    // waits for window events: a minimized window is reported by
    // Recreate_swapchain_if_needed() returning false.
    //
    // A frame is a transaction. If Render throws, the acquired image and
    // the semaphore signal of the failed frame have been given back, and
    // the next Render call works. The exception is for the caller to log;
    // it is only final when Is_lost() is true.
    //
    // Frame structure, everything GPU-driven recorded before the render
    // pass:
    //   compute  - procedural texture, only in the frames where its content
    //              changes: once at startup today;
    //   transfer - cluster boxes (when the projection changed) and counter
    //              resets;
    //   compute  - light assignment to clusters, frustum culling of the
    //              opaque objects;
    //   barrier  - compute writes to the fragment stage, the indirect
    //              command read and the statistics copy;
    //   render   - one geometry bind, then three subpasses:
    //                0 opaque      - opaque draws (direct, CPU indirect or
    //                                GPU indirect count), depth written;
    //                1 transparent - transparent draws accumulated into the
    //                                OIT targets (weighted blended
    //                                order-independent transparency), in
    //                                any order, depth tested only;
    //                2 composite   - the accumulated transparency blended
    //                                over the opaque color, then the debug
    //                                bounding volumes;
    //   transfer - statistics copies into the readback buffer.
    //
    // Every block is a named GPU scope: the same name labels it in
    // captures and times it in the statistics
    // (Render_Debug_Settings::print_stats).
    //
    // The work is split across subsystems, each with its own state and
    // invariants, owned by the implementation: Frame_Timeline, Mesh_Registry,
    // Upload_Context, Material_Table, Present_Sync, Light_Clusters,
    // Draw_List_Builder and Frame_Statistics. The Renderer builds them, and
    // sequences the passes.
    //
    // Not copyable or movable: owns the entire Vulkan lifetime.
    class Renderer
    {
    public:

        // The window is needed after construction to know whether the
        // framebuffer has a size at all (a minimized window cannot have a
        // swapchain). Must outlive the Renderer.
        //
        // _validation_mode is forwarded to the Vulkan instance; see
        // Validation_Mode for what each level checks and what it costs.
        Renderer(const Platform::Window& _window, Validation_Mode _validation_mode);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;
        Renderer(Renderer&&) = delete;
        Renderer& operator=(Renderer&&) = delete;

        // =========================================================
        // Asset upload
        // =========================================================

        // Uploads a mesh into the Geometry_Pool (vertices, 32-bit indices
        // and its mesh table entry, bounding sphere included) and returns
        // its gpu_id: the index into the Renderer's internal mesh registry,
        // which ResourceManager stores through Register_gpu_id() and
        // Draw_Item::mesh_gpu_id references.
        //
        // Throws std::invalid_argument if _mesh_data has no vertices or no
        // indices, and std::runtime_error if the mesh table (MAX_MESHES)
        // or the Geometry_Pool is full.
        uint32_t Upload_mesh(const CoreTypes::MeshData& _mesh_data);

        // Stops drawing mesh _gpu_id and returns its geometry to the pool
        // once no frame in flight can read it any more. The id is not
        // reused: Draw_Items that still reference it are skipped with a
        // warning. Releasing an id twice, or an id that was never handed
        // out, does nothing.
        void Release_mesh(uint32_t _gpu_id);

        // Uploads a texture (with a full mip chain) to the GPU, registers
        // it in the global bindless texture array, and returns its
        // BINDLESS INDEX: the value shaders use to index into the texture
        // array (set 3, binding 0). The texture carries no sampler: the
        // shader pairs it with the slot of the sampler array (set 3,
        // binding 1) that the material selects, e.g.:
        //   Sample_bindless(material.albedo_texture_index, material.albedo_sampler_index, uv)
        // where the index returned here reaches the shader as
        // Material_Desc::albedo_texture_index (see Register_material).
        //
        // This is distinct from the internal texture registry index (used
        // only to keep the GPU texture alive); callers (ResourceManager,
        // Material loading) should only ever store and use the bindless
        // index returned here.
        //
        // _format: RGBA8_SRGB for color textures (albedo, emissive),
        //   RGBA8_UNORM for data textures (normal, metallic-roughness, AO).
        //   Caller decides based on texture role.
        uint32_t Upload_texture(const CoreTypes::ImageData& _image_data, CoreTypes::Pixel_Format _format);

        // Same, with the format taken from ImageData::format.
        uint32_t Upload_texture(const CoreTypes::ImageData& _image_data);

        // Uploads every mesh and texture in _batch using ONE command buffer
        // and ONE queue submission, instead of one of each per asset.
        // Meshes are copied into the Geometry_Pool from one staging buffer
        // (one copy with a region per mesh for the vertices, another for
        // the indices, a third for their mesh table entries); textures only
        // record into the command buffer they are handed and never submit,
        // which is what makes this possible.
        //
        // Ids come back in the same order as the input lists:
        //   result.mesh_gpu_ids[i]             <- _batch.meshes[i]
        //   result.texture_bindless_indices[i] <- _batch.textures[i]
        //
        // Upload_mesh and Upload_texture above are thin wrappers over this
        // function with a single-element batch: there is one upload path.
        //
        // Cost to be aware of: every staging buffer in the batch stays alive
        // until the submit completes, so peak host-visible memory is the SUM
        // of the batch, not the largest item. Split very large scenes into
        // several batches if that ever matters.
        //
        // Strong exception guarantee: if any upload fails, nothing of the
        // batch stays registered. Every check that can reject the batch
        // (null or empty data, a full bindless texture array, a full mesh
        // table or geometry pool) runs before anything is recorded; once
        // the submit has completed, registering the textures in the
        // bindless array cannot fail.
        //
        // Throws std::invalid_argument for a null or empty element, and
        // std::runtime_error if the bindless texture array does not have a
        // free slot for every texture of the batch, or the mesh table or
        // the geometry pool cannot hold its meshes.
        Upload_Batch_Result Upload_batch(const Upload_Batch& _batch);

        // =========================================================
        // Materials
        // =========================================================

        // Registers a material in the material table (set 2) and returns
        // its slot: the value Draw_Item::material_index carries and the
        // shaders index the table with. A description equal to one already
        // registered returns that slot and writes nothing.
        //
        // Slots are never released or rewritten, so the returned index is
        // valid for the whole lifetime of the Renderer. Meant for load
        // time; calling it between frames is also valid, because a new
        // slot is never referenced by a command already submitted.
        //
        // Throws std::invalid_argument if albedo_texture_index is not a
        // registered bindless slot or sampler is not a Sampler_Preset
        // value, and std::runtime_error if the table is full
        // (MAX_MATERIALS).
        uint32_t Register_material(const Material_Desc& _desc);

        // =========================================================
        // Compute pass outputs
        // =========================================================

        // Bindless index of the texture generated by procedural.comp, which
        // is regenerated only when its content changes (once, in the first
        // frame, today). Used like the index returned by Upload_texture:
        // stored as a material's albedo texture index and sampled with any
        // sampler preset. The image is UNORM, so it is read as linear color.
        // Valid for the whole lifetime of the Renderer.
        uint32_t Get_procedural_texture_index() const;

        // =========================================================
        // Pipelines
        // =========================================================

        // Ids that go into the sort key of a Draw_Item (CoreTypes::
        // Get_pipeline_id).
        uint8_t Get_opaque_pipeline_id() const;
        uint8_t Get_transparent_pipeline_id() const;

        // =========================================================
        // Debug switches
        // =========================================================

        const Render_Debug_Settings& Get_debug_settings() const;

        // Applies new switches from the next frame on and logs every value
        // that changed. Out-of-range enumerators are replaced by their
        // defaults. Enabling freeze_culling freezes the frustum of the next
        // frame.
        void Set_debug_settings(const Render_Debug_Settings& _settings);

        // =========================================================
        // Surface size
        // =========================================================

        // Marks the swapchain as needing recreation. Called by the engine
        // loop when Window::Consume_resized_flag() reports a resize.
        void Notify_framebuffer_resized();

        // Rebuilds the swapchain if a recreation is pending (a resize was
        // notified, or the driver reported the swapchain out of date) and
        // the window currently has a non-zero framebuffer. Returns true if
        // the swapchain is usable afterwards (false while minimized).
        // The engine loop calls it before extracting the frame, so the
        // aspect ratio and the viewport come from the same extent.
        //
        // The rebuild is all or nothing. If it throws (for example, out of
        // memory), the recreation stays pending, Render acquires nothing,
        // and the next call tries again. Throws at once when the Renderer
        // is lost and a recreation is pending.
        bool Recreate_swapchain_if_needed();

        // Size of the images being rendered into: the swapchain extent.
        // This, not the live window size, is what the projection's aspect
        // ratio must be derived from, because it is what the viewport uses.
        void Get_render_size(uint32_t& _out_width, uint32_t& _out_height) const;

        // =========================================================
        // Render
        // =========================================================

        // Draws one frame from the given RenderPacket.
        // Handles frame-in-flight synchronization, command recording,
        // submission, and presentation internally.
        // On swapchain out-of-date (resize), marks the swapchain for
        // recreation and skips the frame. While a recreation is pending
        // (or the window is minimized) it draws nothing.
        //
        // Throws when the frame fails. Everything the failed frame did to
        // the GPU and to the presentation is undone before the exception
        // leaves, so the next call can succeed, unless Is_lost() is true:
        // then it throws at once, every time.
        void Render(const CoreTypes::RenderPacket& _packet);

        // =========================================================
        // Device loss
        // =========================================================

        // True when the device was lost (VK_ERROR_DEVICE_LOST) or a failed
        // frame could not be undone. Final: Render, the uploads and the
        // swapchain recreation throw at once, without waiting for the GPU,
        // and the Renderer can only be destroyed. Checked by the engine
        // loop after a failure to decide whether to go on.
        bool Is_lost() const;

    private:

        // The Vulkan stack and the subsystems; defined in Renderer.cpp.
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

} // namespace Renderer_System
