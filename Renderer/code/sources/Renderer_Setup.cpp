#include "Renderer_Impl.hpp"

#include <Descriptor_Layouts.hpp>
#include <Vulkan_Descriptor_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <Primitive_Builder.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

// Startup work of the Renderer: descriptor pool and sets, global tables,
// the procedural pass, the debug meshes and the debug names. Called once
// from the constructor (Renderer.cpp), in the order documented there.

namespace Renderer_System
{

    namespace
    {
        // Segments and rings of the unit sphere of the bounding volume view.
        constexpr uint16_t BOUNDS_SPHERE_SEGMENTS = 16;
        constexpr uint16_t BOUNDS_SPHERE_RINGS = 8;
    }

    // =========================================================
    // Init_descriptor_pool
    // =========================================================

    void Renderer::Impl::Init_descriptor_pool()
    {
        // The pool holds exactly the sets the Renderer allocates, sized from
        // the tables the layouts are created from (Descriptor_Layouts), so a
        // binding added to a layout is part of the pool without touching
        // this function:
        //   set 0, one per frame slot;
        //   set 1 of the compute contract (per_pass_set): the storage image
        //     of the procedural pass and the cluster boxes;
        //   set 1 of the graphics contract (composite_input_set): the two
        //     OIT targets as input attachments;
        //   set 2 (per_material_set): the material and mesh tables.
        // Sets 1 and 2 are shared by every frame slot. Set 3, the bindless
        // arrays, has a pool of its own (Bindless_Registry).
        Vulkan_Descriptor_Utils::Pool_Builder pool_builder;

        pool_builder.Add_sets(Descriptor_Layouts::Per_Frame, FRAMES_IN_FLIGHT)
                    .Add_sets(Descriptor_Layouts::Compute_Per_Pass)
                    .Add_sets(Descriptor_Layouts::Graphics_Per_Pass)
                    .Add_sets(Descriptor_Layouts::Per_Material);

        descriptor_pool = pool_builder.Create(device.Get_logical_device_handle(), 0, "Renderer: failed to create descriptor pool");
    }

    // =========================================================
    // Composite input set (graphics set 1)
    // =========================================================

    void Renderer::Impl::Init_composite_input_set()
    {
        composite_input_set = Vulkan_Descriptor_Utils::Allocate_set(device.Get_logical_device_handle(), descriptor_pool,
            descriptor_layouts.Get(Descriptor_Set::Per_Pass, Pipeline_Kind::Graphics),
            "Init_composite_input_set: failed to allocate the graphics set 1");

        Write_composite_input_set();
    }

    void Renderer::Impl::Write_composite_input_set()
    {
        assert(composite_input_set != VK_NULL_HANDLE && "Write_composite_input_set: the set is not allocated");

        // SHADER_READ_ONLY_OPTIMAL: the layout the composite subpass reads
        // them in (the input attachment references of Vulkan_Render_Pass).
        // No sampler: subpassLoad reads the texel of the current pixel.
        Vulkan_Descriptor_Utils::Descriptor_Writer writer(Descriptor_Layouts::Graphics_Per_Pass);

        writer.Write_image(composite_input_set, Binding_Graphics_Pass::Oit_Accumulation,
                           targets.oit.Get_accumulation_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
              .Write_image(composite_input_set, Binding_Graphics_Pass::Oit_Revealage,
                           targets.oit.Get_revealage_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        writer.Update(device.Get_logical_device_handle());
    }

    // =========================================================
    // Init_descriptor_sets
    // =========================================================

    void Renderer::Impl::Init_descriptor_sets()
    {
        const VkDevice dev = device.Get_logical_device_handle();

        // Set 0 is shared by both contracts.
        Vulkan_Descriptor_Utils::Allocate_sets(dev, descriptor_pool,
            descriptor_layouts.Get(Descriptor_Set::Per_Frame, Pipeline_Kind::Graphics),
            descriptor_sets, "Renderer: failed to allocate descriptor sets");

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            const Frame_Data& frame = frames[i];

            // Every descriptor covers the whole buffer, that is, its
            // CAPACITY, not this frame's live content: the descriptors are
            // written once at startup and the contents change by memcpy or
            // by the GPU. Frame_UBO::light_count says how many lights are
            // valid, and every draw indexes its own object entry through
            // firstInstance. The sizes are the ones the buffers were created
            // with (Frame_Data), not restated here.
            Vulkan_Descriptor_Utils::Descriptor_Writer writer(Descriptor_Layouts::Per_Frame);

            writer.Write_buffer(descriptor_sets[i], Binding_Per_Frame::Frame_UBO,             frame.uniform_buffer)
                  .Write_buffer(descriptor_sets[i], Binding_Per_Frame::Lights,                frame.light_buffer)
                  .Write_buffer(descriptor_sets[i], Binding_Per_Frame::Objects,               frame.object_buffer)
                  .Write_buffer(descriptor_sets[i], Binding_Per_Frame::Cluster_Grid,          frame.cluster_grid_buffer)
                  .Write_buffer(descriptor_sets[i], Binding_Per_Frame::Cluster_Light_Indices, frame.cluster_light_index_buffer)
                  .Write_buffer(descriptor_sets[i], Binding_Per_Frame::Cluster_Counters,      frame.cluster_counter_buffer)
                  .Write_buffer(descriptor_sets[i], Binding_Per_Frame::Draw_Commands,         frame.gpu_draw_command_buffer)
                  .Write_buffer(descriptor_sets[i], Binding_Per_Frame::Draw_Count,            frame.gpu_draw_count_buffer);

            writer.Update(dev);
        }
    }

    // =========================================================
    // Init_procedural_pass
    // =========================================================

    void Renderer::Impl::Init_procedural_pass()
    {
        // Fixed size: independent of the swapchain, so the image is never
        // recreated on resize and its bindless slot never changes.
        constexpr uint32_t PROCEDURAL_TEXTURE_SIZE = 256;

        // Storage image support for R8G8B8A8_UNORM with optimal tiling is
        // mandatory in Vulkan. Must match the rgba8 qualifier of
        // procedural.comp. SRGB formats rarely support storage.
        constexpr VkFormat PROCEDURAL_TEXTURE_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

        VkDevice dev = device.Get_logical_device_handle();

        // -- Image and initial clear --
        // The constructor records the clear that leaves every texel at zero
        // in SHADER_READ_ONLY_OPTIMAL. It is submitted and waited for here,
        // before the first frame, so the slot is valid from its first read.
        try
        {
            upload_context.Run([&](VkCommandBuffer _transfer_cmd)
                {
                    procedural_image.emplace(device, allocator.Get_handle(), _transfer_cmd,
                        PROCEDURAL_TEXTURE_SIZE, PROCEDURAL_TEXTURE_SIZE, PROCEDURAL_TEXTURE_FORMAT);
                });
        }
        catch (...)
        {
            // Same recovery as Upload_batch: Run waited for the device and
            // released the transfer, so the clear either never ran or was
            // waited for, and the image can be destroyed right away. When
            // Run could not wait for the device (Settle_failed_transfer),
            // the clear may still be running: the image stays, and the
            // constructor's failure path destroys it after its own wait.
            if (Settle_failed_transfer())
                procedural_image.reset();

            throw;
        }

        // -- Bindless slot (set 3) --
        // Read by the draws as a sampled image. The declared layout of the
        // slot (SHADER_READ_ONLY_OPTIMAL) holds after the initial clear and
        // after every Storage_Image::End_write.
        procedural_texture_index = bindless_registry.Register_texture(procedural_image->Get_image_view());

        // -- Set 1: storage image descriptor --
        // Set 1 of the compute contract (Binding_Per_Pass).
        per_pass_set = Vulkan_Descriptor_Utils::Allocate_set(dev, descriptor_pool,
            descriptor_layouts.Get(Descriptor_Set::Per_Pass, Pipeline_Kind::Compute),
            "Init_procedural_pass: failed to allocate the set 1 descriptor set");

        // GENERAL: the layout the image is in while the dispatch writes it
        // (between Begin_write and End_write), not the layout it rests in.
        // Written once, before any frame is recorded: the only moment the
        // set can be updated without racing a frame in flight.
        Vulkan_Descriptor_Utils::Descriptor_Writer writer(Descriptor_Layouts::Compute_Per_Pass);

        writer.Write_image(per_pass_set, Binding_Per_Pass::Procedural_Output, procedural_image->Get_image_view(), VK_IMAGE_LAYOUT_GENERAL);
        writer.Update(dev);

        // The image holds the zeros of its initial clear: the first frame
        // generates its content.
        procedural_dirty = true;

        std::cout << "[Renderer] Procedural texture " << PROCEDURAL_TEXTURE_SIZE << "x" << PROCEDURAL_TEXTURE_SIZE
            << " (" << Vulkan_Utils::Vk_format_to_string(PROCEDURAL_TEXTURE_FORMAT)
            << ") in bindless slot " << procedural_texture_index << ", generated when its content changes.\n";
    }

    // =========================================================
    // Material and mesh tables
    // =========================================================

    void Renderer::Impl::Init_global_tables()
    {
        VkDevice dev = device.Get_logical_device_handle();

        // -- Mesh table --
        // Device-local, written by the transfer of the upload that creates
        // each mesh (Upload_batch), never by the CPU directly. The material
        // table owns its own buffer.
        mesh_table_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator.Get_handle(), sizeof(Mesh_Info_GPU) * MAX_MESHES,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Vulkan_Buffer_Utils::Buffer_Access::Gpu_Only);

        // -- Set 2: material table descriptor --
        // Set 2 is shared by both contracts.
        per_material_set = Vulkan_Descriptor_Utils::Allocate_set(dev, descriptor_pool,
            descriptor_layouts.Get(Descriptor_Set::Per_Material, Pipeline_Kind::Graphics),
            "Init_global_tables: failed to allocate the set 2 descriptor set");

        // Both ranges are the whole CAPACITY: the descriptors are written
        // once and entries are added in place afterwards.
        Vulkan_Descriptor_Utils::Descriptor_Writer writer(Descriptor_Layouts::Per_Material);

        writer.Write_buffer(per_material_set, Binding_Per_Material::Materials, material_table.Get_buffer(), material_table.Get_buffer_size())
              .Write_buffer(per_material_set, Binding_Per_Material::Meshes, mesh_table_buffer);

        writer.Update(dev);

        // -- Default material --
        // A default-constructed Material_Desc is the default material.
        // Slot 0 is a contract with every Draw_Item: a mismatch means
        // something was registered before this call.
        const uint32_t default_slot = Register_material(Material_Desc{});

        if (default_slot != Default_Material)
            throw std::logic_error("Renderer: the default material did not land in slot "
                + std::to_string(Default_Material) + "; something was registered before it");

        std::cout << "[Renderer] Material table: " << MAX_MATERIALS << " slots, default material in slot "
            << Default_Material << ". Mesh table: " << MAX_MESHES << " entries.\n";
    }

    // =========================================================
    // Clustered lighting resources
    // =========================================================

    void Renderer::Impl::Init_light_clusters()
    {
        light_clusters.Write_descriptor(per_pass_set);

        std::cout << "[Renderer] Clustered lighting: " << CLUSTER_TILES_X << "x" << CLUSTER_TILES_Y << "x" << CLUSTER_SLICES
            << " clusters, slices up to " << CLUSTER_MAX_DISTANCE << " units, " << CLUSTER_LIGHT_INDEX_CAPACITY
            << " light index entries per frame, " << MAX_LIGHTS << " lights max.\n";
    }

    // =========================================================
    // Debug resources
    // =========================================================

    void Renderer::Impl::Init_debug_meshes()
    {
        // The unit sphere of the bounding volume view: radius 1, centered at
        // the origin. Only the positions matter to bounds.vert, which maps
        // it onto each object's bounding ellipsoid; the winding does not
        // either, since the view is drawn without culling.
        const CoreTypes::MeshData unit_sphere = ResourceManager::Primitive_Builder::Build_sphere(BOUNDS_SPHERE_SEGMENTS, BOUNDS_SPHERE_RINGS);

        bounds_sphere_mesh_id = Upload_mesh(unit_sphere);

        std::cout << "[Renderer] Bounding volume view: unit sphere uploaded as mesh " << bounds_sphere_mesh_id
            << (device.Is_fill_mode_non_solid_enabled() ? " (wireframe).\n" : " (filled, blended: fillModeNonSolid unavailable).\n");
    }

    void Renderer::Impl::Name_debug_objects()
    {
        if (!debug_utils.Is_enabled())
            return;

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            const Frame_Data&  frame = frames[i];
            const std::string suffix = "_Frame" + std::to_string(i);

            const auto name_buffer = [&](const Vulkan_Buffer_Utils::Buffer_Allocation& _buffer, const char* _name)
                {
                    debug_utils.Set_name(_buffer.buffer, VK_OBJECT_TYPE_BUFFER, (std::string(_name) + suffix).c_str());
                };

            name_buffer(frame.uniform_buffer, "Frame_UBO");
            name_buffer(frame.light_buffer, "Lights");
            name_buffer(frame.object_buffer, "Objects");
            name_buffer(frame.cpu_draw_command_buffer, "Cpu_Draw_Commands");
            name_buffer(frame.cluster_grid_buffer, "Cluster_Grid");
            name_buffer(frame.cluster_light_index_buffer, "Cluster_Light_Indices");
            name_buffer(frame.cluster_counter_buffer, "Cluster_Counters");
            name_buffer(frame.gpu_draw_command_buffer, "Gpu_Draw_Commands");
            name_buffer(frame.gpu_draw_count_buffer, "Gpu_Draw_Count");
            name_buffer(frame.stats_readback_buffer, "Stats_Readback");

            debug_utils.Set_name(frame.Get_command_buffer(), VK_OBJECT_TYPE_COMMAND_BUFFER, ("Frame_Commands" + suffix).c_str());
            debug_utils.Set_name(descriptor_sets[i], VK_OBJECT_TYPE_DESCRIPTOR_SET, ("Per_Frame_Set" + suffix).c_str());
        }

        debug_utils.Set_name(geometry_pool.Get_vertex_buffer(), VK_OBJECT_TYPE_BUFFER, "Geometry_Pool_Vertices");
        debug_utils.Set_name(geometry_pool.Get_index_buffer(), VK_OBJECT_TYPE_BUFFER, "Geometry_Pool_Indices");
        debug_utils.Set_name(material_table.Get_buffer(), VK_OBJECT_TYPE_BUFFER, "Material_Table");
        debug_utils.Set_name(mesh_table_buffer.buffer, VK_OBJECT_TYPE_BUFFER, "Mesh_Table");
        debug_utils.Set_name(light_clusters.Get_aabb_buffer(), VK_OBJECT_TYPE_BUFFER, "Cluster_AABBs");

        debug_utils.Set_name(per_pass_set, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Per_Pass_Set");
        debug_utils.Set_name(composite_input_set, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Composite_Input_Set");
        debug_utils.Set_name(per_material_set, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Per_Material_Set");

        debug_utils.Set_name(procedural_pipeline.Get_handle(), VK_OBJECT_TYPE_PIPELINE, "Procedural_Compute");
        debug_utils.Set_name(light_clusters.Get_pipeline(), VK_OBJECT_TYPE_PIPELINE, "Cluster_Lights_Compute");
        debug_utils.Set_name(cull_pipeline.Get_handle(), VK_OBJECT_TYPE_PIPELINE, "Cull_Objects_Compute");
        debug_utils.Set_name(pipeline_registry.Get_by_id(opaque_pipeline_id), VK_OBJECT_TYPE_PIPELINE, "Mesh_Opaque");
        debug_utils.Set_name(pipeline_registry.Get_by_id(transparent_pipeline_id), VK_OBJECT_TYPE_PIPELINE, "Mesh_Transparent");
        debug_utils.Set_name(pipeline_registry.Get_by_id(bounds_pipeline_id), VK_OBJECT_TYPE_PIPELINE, "Bounds_Debug");
        debug_utils.Set_name(pipeline_registry.Get_by_id(composite_pipeline_id), VK_OBJECT_TYPE_PIPELINE, "Oit_Composite");

        if (procedural_image.has_value())
            debug_utils.Set_name(procedural_image->Get_image(), VK_OBJECT_TYPE_IMAGE, "Procedural_Texture");

        if (gpu_timer.Is_supported())
            debug_utils.Set_name(gpu_timer.Get_query_pool(), VK_OBJECT_TYPE_QUERY_POOL, "Gpu_Timer_Queries");

        Name_oit_targets();
    }

    void Renderer::Impl::Name_oit_targets()
    {
        if (!debug_utils.Is_enabled())
            return;

        debug_utils.Set_name(targets.oit.Get_accumulation_image(), VK_OBJECT_TYPE_IMAGE, "Oit_Accumulation");
        debug_utils.Set_name(targets.oit.Get_revealage_image(), VK_OBJECT_TYPE_IMAGE, "Oit_Revealage");
    }

} // namespace Renderer_System
