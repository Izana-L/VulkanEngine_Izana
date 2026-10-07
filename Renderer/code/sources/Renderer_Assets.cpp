#include "Renderer_Impl.hpp"

#include <Mesh_Upload.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

// Asset side of the Renderer: uploads, mesh release and materials.

namespace Renderer_System
{

    // =========================================================
    // Upload
    // =========================================================

    Upload_Batch_Result Renderer::Impl::Upload_batch(const Upload_Batch& _batch)
    {
        // An upload waits for the frames in flight and for its own transfer:
        // on a lost device it fails at once instead.
        Require_not_lost("Upload_batch");

        Upload_Batch_Result result;

        const size_t mesh_count = _batch.meshes.size();
        const size_t texture_count = _batch.textures.size();

        // Nothing to do: don't allocate a command buffer for an empty batch.
        if (mesh_count == 0 && texture_count == 0)
            return result;

        // Validate the whole batch before touching the GPU, so a bad
        // element cannot leave half a batch uploaded. A mesh is checked down
        // to its indices: one past the end of its vertices would read the
        // geometry of its neighbor in the pool.
        for (size_t i = 0; i < mesh_count; ++i)
            Validate_mesh(_batch.meshes[i], i);

        for (const Texture_Upload& upload : _batch.textures)
        {
            if (upload.data == nullptr)
                throw std::invalid_argument("Upload_batch: null ImageData pointer");
            if (upload.data->pixels.empty() || upload.data->width == 0 || upload.data->height == 0)
                throw std::invalid_argument("Upload_batch: ImageData has no pixel data or zero dimensions");
        }

        // Every mesh of the batch needs an entry in the mesh table: its gpu
        // id is the entry index.
        if (mesh_count > mesh_registry.Get_free_slot_count())
        {
            throw std::runtime_error("Upload_batch: the batch holds " + std::to_string(mesh_count) + " mesh(es) but the mesh table has " +
                                     std::to_string(mesh_registry.Get_free_slot_count()) + " free entries; raise MAX_MESHES in Renderer_Limits.hpp");
        }

        // Every texture of the batch needs a bindless slot after the
        // submit. Checked here, before recording, so a full array rejects
        // the batch while nothing has been created yet.
        const uint32_t free_texture_slots = bindless_registry.Get_free_texture_count();

        if (texture_count > free_texture_slots)
        {
            throw std::runtime_error("Upload_batch: the batch holds " + std::to_string(texture_count) +
                                     " texture(s) but the bindless texture array has only " + std::to_string(free_texture_slots) +
                                     " free slot(s); release unused slots or raise BINDLESS_DESIRED_TEXTURES, "
                                     "within the device limits logged at startup");
        }

        // Reserve before recording. Otherwise emplace_back can reallocate
        // mid-batch and move every Texture_GPU already recorded. Those
        // moves are safe (they null out the source), but reserving avoids
        // the churn, keeps the registries stable while the batch is built,
        // and makes the push_backs below unable to throw.
        textures.reserve(textures.size() + texture_count);

        result.mesh_gpu_ids.reserve(mesh_count);
        result.texture_bindless_indices.reserve(texture_count);

        // Where this batch starts, so the post-submit pass below only
        // touches what this call added, and the failure path can undo it.
        const size_t first_texture = textures.size();

        uint32_t first_mesh_id = 0;
        bool     meshes_registered = false;

        try
        {
            // -- Frames in flight --
            // They read the geometry pool and the mesh table while this
            // batch writes them. The ranges written are new, so no frame
            // reads them, but each buffer is a single resource: an indexed
            // or indirect draw is modeled by validation tools as reading the
            // whole bound vertex buffer, and a descriptor as reading its
            // whole range. Waiting for the frames in flight first orders the
            // copies after every earlier read without relying on the ranges
            // being disjoint. Uploads are synchronous anyway, and the copies
            // would queue behind those frames.
            //
            // Before the ranges are reserved, not after: the wait also frees
            // the geometry that the frames just completed were holding
            // (Wait_for_serial). A pool whose free space is still held by
            // meshes released a frame or two ago must hand it to this batch,
            // not reject it for want of a range that the wait would free.
            if (mesh_count > 0)
                Wait_for_frames_in_flight();

            // -- Geometry ranges --
            // Allocated before anything is recorded: a full pool rejects
            // the batch with only the ranges of this batch to give back
            // (Mesh_Registry::Add_batch returns them itself).
            if (mesh_count > 0)
            {
                first_mesh_id = mesh_registry.Add_batch(_batch.meshes);
                meshes_registered = true;
            }

            // -- One transfer for the whole batch --
            // One command buffer, one submit and one wait, and the release of
            // every staging buffer at the end: the textures' and the
            // meshes' alike, which the GPU has consumed once the wait
            // returned. If anything throws, Run waits for the device and
            // releases them before the exception reaches the handler below.
            upload_context.Run([&](VkCommandBuffer _transfer_cmd)
                {
                    if (mesh_count > 0)
                    {
                        Record_mesh_copies(upload_context, _transfer_cmd, _batch.meshes, mesh_registry, first_mesh_id,
                                           geometry_pool, mesh_table_buffer.buffer);
                    }

                    // -- Textures --
                    // No barriers are needed BETWEEN assets: each Texture_GPU
                    // barriers its own image, so the recordings touch
                    // disjoint resources. The barriers that do exist (layout
                    // transitions, mip generation) are internal to each
                    // Texture_GPU. Their staging buffers come from the upload
                    // context, like the one of the meshes.
                    for (const Texture_Upload& upload : _batch.textures)
                    {
                        textures.emplace_back(device, allocator.Get_handle(), upload_context, _transfer_cmd, *upload.data,
                                              Vulkan_Image_Utils::To_vk_format(upload.format));
                    }
                });
        }
        catch (...)
        {
            // A lost device is final for the whole Renderer.
            Note_current_failure();

            // Nothing of this batch reached the GPU in a usable state, and
            // when the failure came from the transfer the device is idle
            // (Run waited for it): give back the ranges and drop the
            // registry entries added above (Texture_GPU destructors free
            // their images; the staging buffers were freed by the upload
            // context). A failure before the transfer holds no range that
            // any frame could read: they were just allocated.
            //
            // Unless Run could not wait for the device: the transfer may
            // still be writing those ranges and images. Settle_failed_
            // transfer says so and declares the Renderer lost; they are
            // released with it, by its destructor.
            if (Settle_failed_transfer())
            {
                if (meshes_registered)
                    mesh_registry.Discard_batch(first_mesh_id);

                textures.erase(textures.begin() + static_cast<std::ptrdiff_t>(first_texture), textures.end());
            }

            throw;
        }

        // -- Post-upload --
        // The transfer has completed (Run returned), so the GPU has consumed
        // every staging buffer in the batch and the upload context released
        // them.
        for (size_t i = 0; i < mesh_count; ++i)
            result.mesh_gpu_ids.push_back(first_mesh_id + static_cast<uint32_t>(i));

        for (size_t i = first_texture; i < textures.size(); ++i)
        {
            // Register in the global bindless texture array. Only the view
            // is registered: the sampler is chosen per draw, from the
            // sampler array, by the material's preset. The bindless index,
            // not the registry index i, is what callers store and what
            // shaders use.
            //
            // Cannot throw, which keeps the strong guarantee although it
            // runs after the submit: the free slots were checked before
            // recording, the registry reserves storage for every slot at
            // construction and writes the descriptor without allocating,
            // the view of a constructed Texture_GPU is never null, and
            // result.texture_bindless_indices was reserved above.
            result.texture_bindless_indices.push_back(bindless_registry.Register_texture(textures[i].Get_image_view()));
        }

        size_t batch_vertices = 0;
        size_t batch_indices = 0;

        for (const CoreTypes::MeshData* mesh_data : _batch.meshes)
        {
            batch_vertices += mesh_data->vertices.size();
            batch_indices += mesh_data->indices.size();
        }

        std::cout << "[Renderer] Batch uploaded: "
            << mesh_count << " mesh(es) (" << batch_vertices << " vertices, " << batch_indices << " indices into the geometry pool), "
            << texture_count << " texture(s) - 1 command buffer, 1 submit.\n";

        return result;
    }

    uint32_t Renderer::Impl::Upload_mesh(const CoreTypes::MeshData& _mesh_data)
    {
        Upload_Batch batch;
        batch.meshes.push_back(&_mesh_data);

        const Upload_Batch_Result result = Upload_batch(batch);

        if (result.mesh_gpu_ids.size() != 1)
            throw std::logic_error("Upload_mesh: single-mesh batch returned the wrong number of ids");

        return result.mesh_gpu_ids[0];
    }

    uint32_t Renderer::Impl::Upload_texture(const CoreTypes::ImageData& _image_data, CoreTypes::Pixel_Format _format)
    {
        Upload_Batch batch;
        batch.textures.push_back({ &_image_data, _format });

        const Upload_Batch_Result result = Upload_batch(batch);

        if (result.texture_bindless_indices.size() != 1)
            throw std::logic_error("Upload_texture: single-texture batch returned the wrong number of indices");

        return result.texture_bindless_indices[0];
    }

    void Renderer::Impl::Release_mesh(uint32_t _gpu_id)
    {
        // The unit sphere of the bounds view is internal: releasing it
        // would leave that view drawing a freed range.
        if (_gpu_id == bounds_sphere_mesh_id && mesh_registry.Is_drawable(_gpu_id))
        {
            std::cerr << "[Renderer] Release_mesh: mesh " << _gpu_id << " is internal to the Renderer and is not released.\n";
            return;
        }

        // Releasing is idempotent: an id that was never handed out, or that
        // was released before, changes nothing. It is reported all the same:
        // a caller that releases an id twice, or one it never got, holds a
        // stale id, and would otherwise never learn it.
        switch (mesh_registry.Release(_gpu_id, timeline))
        {
        case Mesh_Registry::Release_Result::Released:
            break;

        case Mesh_Registry::Release_Result::Unknown_Id:
            std::cerr << "[Renderer] Release_mesh: mesh " << _gpu_id << " was never uploaded (" << mesh_registry.Get_count()
                << " ids handed out); nothing was released.\n";
            break;

        case Mesh_Registry::Release_Result::Already_Released:
            std::cerr << "[Renderer] Release_mesh: mesh " << _gpu_id << " was already released; nothing was released.\n";
            break;
        }
    }

    void Renderer::Impl::Wait_for_frames_in_flight()
    {
        if (frames.empty())
            return;

        // The last submission completing means all of them did: the serials
        // of the timeline semaphore are monotonic. Every submitted frame has
        // completed afterwards, and the geometry released before them goes
        // back to the pool in the same call (Wait_for_serial).
        Wait_for_serial(timeline.Get_submitted_serial(), "Wait_for_frames_in_flight: wait for the last frame submission");
    }

    uint32_t Renderer::Impl::Register_material(const Material_Desc& _desc)
    {
        return material_table.Register(_desc, bindless_registry);
    }

    void Renderer::Impl::Upload_default_textures()
    {
        namespace Default = Default_Texture;

        // One opaque texel. Mip generation is skipped for 1x1 images
        // (Compute_mip_levels(1, 1) == 1).
        const auto make_texel = [](uint8_t _r, uint8_t _g, uint8_t _b, CoreTypes::Pixel_Format _format)
            {
                CoreTypes::ImageData image;
                image.pixels = { _r, _g, _b, 255 };
                image.width = 1;
                image.height = 1;
                image.mip_levels = 1;
                image.format = _format;
                return image;
            };

        // Indexed by slot, so each texel stays tied to its constant even
        // if the Default_Texture values are ever reordered.
        std::array<CoreTypes::ImageData, Default::Count> defaults;
        defaults[Default::Error] = make_texel(255, 0, 255, CoreTypes::Pixel_Format::RGBA8_SRGB);
        defaults[Default::White] = make_texel(255, 255, 255, CoreTypes::Pixel_Format::RGBA8_SRGB);
        defaults[Default::Black] = make_texel(0, 0, 0, CoreTypes::Pixel_Format::RGBA8_SRGB);
        // A normal is data, not color: UNORM, so 128 stays 0.5 when sampled.
        defaults[Default::Flat_Normal] = make_texel(128, 128, 255, CoreTypes::Pixel_Format::RGBA8_UNORM);

        Upload_Batch batch;
        for (const CoreTypes::ImageData& image : defaults)
            batch.textures.push_back({ &image, image.format });

        const Upload_Batch_Result result = Upload_batch(batch);

        // The slots are a contract with every Draw_Item. A mismatch means
        // something was registered before this call.
        bool in_place = result.texture_bindless_indices.size() == Default::Count;
        for (uint32_t i = 0; in_place && i < Default::Count; ++i)
            in_place = result.texture_bindless_indices[i] == i;

        if (!in_place)
            throw std::logic_error("Renderer: the default textures did not land in bindless slots 0.."
                + std::to_string(Default::Count - 1) + "; something was registered before them");

        std::cout << "[Renderer] Default textures in bindless slots 0-" << (Default::Count - 1)
                  << " (Error, White, Black, Flat_Normal).\n";
    }

} // namespace Renderer_System
