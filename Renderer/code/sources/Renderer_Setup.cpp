#include "Renderer_Impl.hpp"

#include <MathConstants.hpp>
#include <Vulkan_Utils.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

// Startup work of the Renderer: descriptor pool and sets, global tables,
// the procedural pass, the debug meshes and the debug names. Called once
// from the constructor (Renderer.cpp), in the order documented there.

namespace Renderer_System
{

    namespace
    {
        // Segments and rings of the unit sphere of the bounding volume view.
        constexpr uint32_t BOUNDS_SPHERE_SEGMENTS = 16;
        constexpr uint32_t BOUNDS_SPHERE_RINGS = 8;

        // Unit sphere (radius 1, centered at the origin) as a latitude /
        // longitude grid. Only positions matter to bounds.vert, which maps
        // it onto each object's bounding ellipsoid; the other attributes
        // get neutral values.
        CoreTypes::MeshData Build_unit_sphere(uint32_t _segments, uint32_t _rings)
        {
            CoreTypes::MeshData mesh;

            for (uint32_t ring = 0; ring <= _rings; ++ring)
            {
                const float phi = MathLib::Constants::PI * static_cast<float>(ring) / static_cast<float>(_rings);

                for (uint32_t segment = 0; segment <= _segments; ++segment)
                {
                    const float theta = MathLib::Constants::TWO_PI * static_cast<float>(segment) / static_cast<float>(_segments);

                    CoreTypes::Vertex_Static_Mesh_CPU vertex{};
                    vertex.position = { std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta) };
                    vertex.normal = vertex.position;
                    vertex.tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
                    vertex.uv = { static_cast<float>(segment) / static_cast<float>(_segments),
                                  static_cast<float>(ring) / static_cast<float>(_rings) };
                    vertex.color = { 1.0f, 1.0f, 1.0f, 1.0f };

                    mesh.vertices.push_back(vertex);
                }
            }

            const uint32_t stride = _segments + 1;

            for (uint32_t ring = 0; ring < _rings; ++ring)
            {
                for (uint32_t segment = 0; segment < _segments; ++segment)
                {
                    const uint32_t a = ring * stride + segment;
                    const uint32_t b = a + stride;

                    mesh.indices.insert(mesh.indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
                }
            }

            return mesh;
        }
    }

    // =========================================================
    // Init_descriptor_pool
    // =========================================================

    void Renderer::Impl::Init_descriptor_pool()
    {
        // Set 0: one uniform buffer and seven storage buffers (lights,
        // objects, cluster grid, cluster light indices, cluster counters,
        // draw commands, draw count) per frame-in-flight. Set 1 of the
        // compute contract: one storage image for the procedural pass and
        // one storage buffer for the cluster boxes (per_pass_set). Set 1 of
        // the graphics contract: two input attachments, the OIT targets
        // (composite_input_set). Set 2: two storage buffers, the material
        // and mesh tables (per_material_set). Sets 1 and 2 are shared by
        // every frame slot.
        constexpr uint32_t PER_FRAME_STORAGE_BUFFERS = 7;
        constexpr uint32_t PER_PASS_STORAGE_BUFFERS = 1;
        constexpr uint32_t PER_MATERIAL_STORAGE_BUFFERS = 2;
        constexpr uint32_t COMPOSITE_INPUT_ATTACHMENTS = 2;

        std::array<VkDescriptorPoolSize, 4> pool_sizes{};

        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_sizes[0].descriptorCount = FRAMES_IN_FLIGHT;

        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[1].descriptorCount = PER_FRAME_STORAGE_BUFFERS * FRAMES_IN_FLIGHT + PER_PASS_STORAGE_BUFFERS + PER_MATERIAL_STORAGE_BUFFERS;

        pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pool_sizes[2].descriptorCount = 1;

        pool_sizes[3].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        pool_sizes[3].descriptorCount = COMPOSITE_INPUT_ATTACHMENTS;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        // One set 0 per frame slot + the compute set 1 + the graphics set 1
        // + set 2.
        pool_info.maxSets = FRAMES_IN_FLIGHT + 3;

        VK_CHECK(vkCreateDescriptorPool(device.Get_logical_device_handle(), &pool_info, nullptr, &descriptor_pool),
            "Renderer: failed to create descriptor pool");
    }

    // =========================================================
    // Composite input set (graphics set 1)
    // =========================================================

    void Renderer::Impl::Init_composite_input_set()
    {
        const VkDescriptorSetLayout composite_layout = descriptor_layouts.Get(Descriptor_Set::Per_Pass, Pipeline_Kind::Graphics);

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &composite_layout;

        VK_CHECK(vkAllocateDescriptorSets(device.Get_logical_device_handle(), &alloc_info, &composite_input_set),
            "Init_composite_input_set: failed to allocate the graphics set 1");

        Write_composite_input_set();
    }

    void Renderer::Impl::Write_composite_input_set()
    {
        assert(composite_input_set != VK_NULL_HANDLE && "Write_composite_input_set: the set is not allocated");

        // SHADER_READ_ONLY_OPTIMAL: the layout the composite subpass reads
        // them in (the input attachment references of Vulkan_Render_Pass).
        // No sampler: subpassLoad reads the texel of the current pixel.
        std::array<VkDescriptorImageInfo, 2> image_infos{};

        image_infos[0].sampler = VK_NULL_HANDLE;
        image_infos[0].imageView = oit_resources.Get_accumulation_view();
        image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        image_infos[1].sampler = VK_NULL_HANDLE;
        image_infos[1].imageView = oit_resources.Get_revealage_view();
        image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        const std::array<uint32_t, 2> bindings = { Binding_Graphics_Pass::Oit_Accumulation, Binding_Graphics_Pass::Oit_Revealage };

        std::array<VkWriteDescriptorSet, 2> writes{};

        for (size_t i = 0; i < writes.size(); ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = composite_input_set;
            writes[i].dstBinding = bindings[i];
            writes[i].dstArrayElement = 0;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo = &image_infos[i];
        }

        vkUpdateDescriptorSets(device.Get_logical_device_handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // =========================================================
    // Init_descriptor_sets
    // =========================================================

    void Renderer::Impl::Init_descriptor_sets()
    {
        // Set 0 is shared by both contracts.
        std::array<VkDescriptorSetLayout, FRAMES_IN_FLIGHT> layouts;
        layouts.fill(descriptor_layouts.Get(Descriptor_Set::Per_Frame, Pipeline_Kind::Graphics));

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = FRAMES_IN_FLIGHT;
        alloc_info.pSetLayouts = layouts.data();

        VK_CHECK(vkAllocateDescriptorSets(device.Get_logical_device_handle(), &alloc_info, descriptor_sets.data()),
            "Renderer: failed to allocate descriptor sets");

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorBufferInfo ubo_info{};
            ubo_info.buffer = frames[i].uniform_buffer.buffer;
            ubo_info.offset = 0;
            ubo_info.range = sizeof(Frame_UBO);

            VkDescriptorBufferInfo light_info{};
            light_info.buffer = frames[i].light_buffer.buffer;
            light_info.offset = 0;
            // The range is the whole CAPACITY, not this frame's live lights:
            // the descriptor is written once at startup and the contents
            // change by memcpy. Frame_UBO::light_count says how many entries
            // are valid.
            light_info.range = sizeof(CoreTypes::GPU_Light) * MAX_LIGHTS;

            // Same rule as the lights: the whole capacity. Only the entries
            // written this frame are read, because every draw indexes its own
            // entry through firstInstance.
            VkDescriptorBufferInfo object_info{};
            object_info.buffer = frames[i].object_buffer.buffer;
            object_info.offset = 0;
            object_info.range = sizeof(Object_GPU) * MAX_OBJECTS;

            // Buffers written by the compute passes of this frame slot,
            // whole capacity as well.
            VkDescriptorBufferInfo cluster_grid_info{};
            cluster_grid_info.buffer = frames[i].cluster_grid_buffer.buffer;
            cluster_grid_info.offset = 0;
            cluster_grid_info.range = sizeof(Cluster_Range_GPU) * CLUSTER_COUNT;

            VkDescriptorBufferInfo cluster_indices_info{};
            cluster_indices_info.buffer = frames[i].cluster_light_index_buffer.buffer;
            cluster_indices_info.offset = 0;
            cluster_indices_info.range = sizeof(uint32_t) * CLUSTER_LIGHT_INDEX_CAPACITY;

            VkDescriptorBufferInfo cluster_counters_info{};
            cluster_counters_info.buffer = frames[i].cluster_counter_buffer.buffer;
            cluster_counters_info.offset = 0;
            cluster_counters_info.range = sizeof(Cluster_Counters_GPU);

            VkDescriptorBufferInfo draw_commands_info{};
            draw_commands_info.buffer = frames[i].gpu_draw_command_buffer.buffer;
            draw_commands_info.offset = 0;
            draw_commands_info.range = sizeof(VkDrawIndexedIndirectCommand) * MAX_OBJECTS;

            VkDescriptorBufferInfo draw_count_info{};
            draw_count_info.buffer = frames[i].gpu_draw_count_buffer.buffer;
            draw_count_info.offset = 0;
            draw_count_info.range = sizeof(Draw_Count_GPU);

            // Binding and buffer of every descriptor of set 0, in binding
            // order; binding 0 is the uniform buffer, the rest are storage
            // buffers.
            constexpr size_t PER_FRAME_BINDING_COUNT = 8;

            const std::array<std::pair<uint32_t, const VkDescriptorBufferInfo*>, PER_FRAME_BINDING_COUNT> bindings = { {
                { Binding_Per_Frame::Frame_UBO,             &ubo_info },
                { Binding_Per_Frame::Lights,                &light_info },
                { Binding_Per_Frame::Objects,               &object_info },
                { Binding_Per_Frame::Cluster_Grid,          &cluster_grid_info },
                { Binding_Per_Frame::Cluster_Light_Indices, &cluster_indices_info },
                { Binding_Per_Frame::Cluster_Counters,      &cluster_counters_info },
                { Binding_Per_Frame::Draw_Commands,         &draw_commands_info },
                { Binding_Per_Frame::Draw_Count,            &draw_count_info } } };

            std::array<VkWriteDescriptorSet, PER_FRAME_BINDING_COUNT> writes{};

            for (size_t b = 0; b < PER_FRAME_BINDING_COUNT; ++b)
            {
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet = descriptor_sets[i];
                writes[b].dstBinding = bindings[b].first;
                writes[b].dstArrayElement = 0;
                writes[b].descriptorType = (bindings[b].first == Binding_Per_Frame::Frame_UBO)
                                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[b].descriptorCount = 1;
                writes[b].pBufferInfo = bindings[b].second;
            }

            vkUpdateDescriptorSets(device.Get_logical_device_handle(),
                static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
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
        VkCommandBuffer transfer_cmd = VK_NULL_HANDLE;

        try
        {
            transfer_cmd = upload_context.Begin();

            procedural_image.emplace(device, allocator.Get_handle(), transfer_cmd,
                PROCEDURAL_TEXTURE_SIZE, PROCEDURAL_TEXTURE_SIZE, PROCEDURAL_TEXTURE_FORMAT);

            upload_context.Submit_and_wait(transfer_cmd);
        }
        catch (...)
        {
            // Same recovery as Upload_batch: the clear either never ran or
            // was waited for, so the image can be destroyed right away.
            upload_context.Abort(transfer_cmd);
            procedural_image.reset();
            throw;
        }

        upload_context.End(transfer_cmd);

        // -- Bindless slot (set 3) --
        // Read by the draws as a sampled image. The declared layout of the
        // slot (SHADER_READ_ONLY_OPTIMAL) holds after the initial clear and
        // after every Storage_Image::End_write.
        procedural_texture_index = bindless_registry.Register_texture(procedural_image->Get_image_view());

        // -- Set 1: storage image descriptor --
        // Set 1 of the compute contract (Binding_Per_Pass).
        const VkDescriptorSetLayout per_pass_layout = descriptor_layouts.Get(Descriptor_Set::Per_Pass, Pipeline_Kind::Compute);

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &per_pass_layout;

        VK_CHECK(vkAllocateDescriptorSets(dev, &alloc_info, &per_pass_set),
            "Init_procedural_pass: failed to allocate the set 1 descriptor set");

        // GENERAL: the layout the image is in while the dispatch writes it
        // (between Begin_write and End_write), not the layout it rests in.
        VkDescriptorImageInfo image_info{};
        image_info.sampler = VK_NULL_HANDLE;
        image_info.imageView = procedural_image->Get_image_view();
        image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = per_pass_set;
        write.dstBinding = Binding_Per_Pass::Procedural_Output;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;

        // Written once, before any frame is recorded: the only moment the
        // set can be updated without racing a frame in flight.
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

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
        const VkDescriptorSetLayout per_material_layout = descriptor_layouts.Get(Descriptor_Set::Per_Material, Pipeline_Kind::Graphics);

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &per_material_layout;

        VK_CHECK(vkAllocateDescriptorSets(dev, &alloc_info, &per_material_set),
            "Init_global_tables: failed to allocate the set 2 descriptor set");

        // Both ranges are the whole CAPACITY: the descriptors are written
        // once and entries are added in place afterwards.
        VkDescriptorBufferInfo material_info{};
        material_info.buffer = material_table.Get_buffer();
        material_info.offset = 0;
        material_info.range = material_table.Get_buffer_size();

        VkDescriptorBufferInfo mesh_info{};
        mesh_info.buffer = mesh_table_buffer.buffer;
        mesh_info.offset = 0;
        mesh_info.range = sizeof(Mesh_Info_GPU) * MAX_MESHES;

        std::array<VkWriteDescriptorSet, 2> writes{};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = per_material_set;
        writes[0].dstBinding = Binding_Per_Material::Materials;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &material_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = per_material_set;
        writes[1].dstBinding = Binding_Per_Material::Meshes;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &mesh_info;

        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        // -- Default material --
        // A default-constructed Material_Desc is the default material.
        // Slot 0 is a contract with every Draw_Item: a mismatch means
        // something was registered before this call.
        const uint32_t default_slot = Register_material(Material_Desc{});

        if (default_slot != CoreTypes::Default_Material)
            throw std::logic_error("Renderer: the default material did not land in slot "
                + std::to_string(CoreTypes::Default_Material) + "; something was registered before it");

        std::cout << "[Renderer] Material table: " << MAX_MATERIALS << " slots, default material in slot "
            << CoreTypes::Default_Material << ". Mesh table: " << MAX_MESHES << " entries.\n";
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
        const CoreTypes::MeshData unit_sphere = Build_unit_sphere(BOUNDS_SPHERE_SEGMENTS, BOUNDS_SPHERE_RINGS);

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

        debug_utils.Set_name(oit_resources.Get_accumulation_image(), VK_OBJECT_TYPE_IMAGE, "Oit_Accumulation");
        debug_utils.Set_name(oit_resources.Get_revealage_image(), VK_OBJECT_TYPE_IMAGE, "Oit_Revealage");
    }

} // namespace Renderer_System
