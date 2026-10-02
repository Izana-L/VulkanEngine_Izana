#pragma once

#include <cstdint>

// The set and binding numbers live in this shared header as macros, and the
// shaders use the same ones in their layout(set = ..., binding = ...).
#include "../../shaders/common/gpu_shared.h"

namespace Renderer_System
{
    // =========================================================
    // Numeracion congelada de conjuntos de descriptores
    // =========================================================
    //
    // Ordenados por FRECUENCIA DE CAMBIO del dato: el 0 se reescribe cada
    // fotograma, el 3 se escribe al cargar y ya no se toca.
    //
    // Por que el orden importa, en terminos de la spec: enlazar el
    // conjunto N nunca perturba los conjuntos MENORES que N, y solo
    // perturba los mayores si el pipeline layout es incompatible para N.
    // Es decir: lo estable va abajo, lo volatil arriba, y reenlazar un
    // material (set 2) por draw jamas obliga a reenlazar la camara.
    //
    // Hoy hay un solo VkPipelineLayout y nada se perturba nunca, asi que
    // esto no compra rendimiento todavia. Compra que el dia que exista un
    // segundo layout (pasada de sombras) el orden ya sea el correcto.
    //
    // ESTOS NUMEROS SON UN CONTRATO C++/GLSL. Cambiarlos invalida todos
    // los VkPipelineLayout y todos los .spv a la vez. Se congelan aqui,
    // ahora, con los huecos reservados, para no volver a tocarlos.
    //
    // The contract has ONE definition: the GPU_SET_* and GPU_BINDING_*
    // macros of Renderer/shaders/common/gpu_shared.h. The constants below
    // are those macros with a name and a type; the shaders write the macros
    // directly. Changing a number there changes both sides.
    namespace Descriptor_Set
    {
        inline constexpr uint32_t Per_Frame = GPU_SET_PER_FRAME;
        inline constexpr uint32_t Per_Pass = GPU_SET_PER_PASS;   // resources of the passes; one layout per Pipeline_Kind (see below)
        inline constexpr uint32_t Per_Material = GPU_SET_PER_MATERIAL;   // global material table, written on registration
        inline constexpr uint32_t Bindless = GPU_SET_BINDLESS;
        inline constexpr uint32_t Count = 4;
    }

    static_assert(Descriptor_Set::Bindless + 1 == Descriptor_Set::Count, "Descriptor_Set::Count must follow the last set of gpu_shared.h");

    // The two pipeline layout contracts of the engine. Sets 0, 2 and 3 use
    // the same set layouts in both; set 1 does not:
    //   Compute  - set 1 = Binding_Per_Pass: what the compute passes read
    //              and write (procedural output, cluster boxes).
    //   Graphics - set 1 = Binding_Graphics_Pass: the attachments the
    //              composite subpass reads.
    // Descriptor sets bound at VK_PIPELINE_BIND_POINT_GRAPHICS and at
    // VK_PIPELINE_BIND_POINT_COMPUTE are independent, so the two set 1
    // never disturb each other, and every graphics pipeline shares one
    // layout, so binding sets for one never disturbs another.
    enum class Pipeline_Kind : uint32_t
    {
        Graphics = 0,
        Compute = 1
    };

    // Bindings dentro del set 0.
    //
    // Bindings 3-7 hold buffers written on the GPU every frame. Each frame
    // slot has its own copy, so a frame never writes what the previous
    // frame, possibly still executing, reads.
    namespace Binding_Per_Frame
    {
        inline constexpr uint32_t Frame_UBO = GPU_BINDING_FRAME_UBO;   // camara, inversas, tiempo
        inline constexpr uint32_t Lights = GPU_BINDING_LIGHTS;   // SSBO con el array de luces
        inline constexpr uint32_t Objects = GPU_BINDING_OBJECTS;     // SSBO of Object_GPU, one entry per draw, indexed by gl_InstanceIndex
        inline constexpr uint32_t Cluster_Grid = GPU_BINDING_CLUSTER_GRID;            // SSBO of Cluster_Range_GPU, one per cluster: offset and count in the light index list
        inline constexpr uint32_t Cluster_Light_Indices = GPU_BINDING_CLUSTER_LIGHT_INDICES;   // SSBO of uint, compacted light indices of every cluster
        inline constexpr uint32_t Cluster_Counters = GPU_BINDING_CLUSTER_COUNTERS;        // SSBO of Cluster_Counters_GPU, atomic allocation counter of the list
        inline constexpr uint32_t Draw_Commands = GPU_BINDING_DRAW_COMMANDS;           // SSBO of VkDrawIndexedIndirectCommand written by the culling pass
        inline constexpr uint32_t Draw_Count = GPU_BINDING_DRAW_COUNT;              // SSBO of Draw_Count_GPU, atomic draw counter of the culling pass
    }

    // Bindings inside set 1 of the compute pipeline layout. Visible to the
    // compute stage only: graphics pipelines read the outputs of a compute
    // pass through the bindless set, never through this one.
    namespace Binding_Per_Pass
    {
        inline constexpr uint32_t Procedural_Output = GPU_BINDING_PROCEDURAL_OUTPUT;   // STORAGE_IMAGE written by procedural.comp (layout GENERAL)
        inline constexpr uint32_t Cluster_AABBs = GPU_BINDING_CLUSTER_AABBS;       // SSBO of Cluster_AABB_GPU, view space, rebuilt when the projection changes
    }

    // Bindings inside set 1 of the graphics pipeline layout: the OIT
    // targets (Vulkan_OIT_Resources) as input attachments of the composite
    // subpass, fragment stage only. One set for every frame in flight,
    // rewritten whenever the targets are recreated with the swapchain.
    // Mirrored by oit_composite.frag.
    namespace Binding_Graphics_Pass
    {
        inline constexpr uint32_t Oit_Accumulation = GPU_BINDING_OIT_ACCUMULATION;   // INPUT_ATTACHMENT, input_attachment_index 0 (layout SHADER_READ_ONLY_OPTIMAL)
        inline constexpr uint32_t Oit_Revealage = GPU_BINDING_OIT_REVEALAGE;      // INPUT_ATTACHMENT, input_attachment_index 1 (layout SHADER_READ_ONLY_OPTIMAL)
    }

    // Bindings inside set 2. One set for every frame: both tables are
    // append-only, so a frame in flight never reads a slot being written.
    namespace Binding_Per_Material
    {
        inline constexpr uint32_t Materials = GPU_BINDING_MATERIALS;   // SSBO of Material_GPU, indexed by Object_GPU::material_index
        inline constexpr uint32_t Meshes = GPU_BINDING_MESHES;      // SSBO of Mesh_Info_GPU, indexed by Object_GPU::mesh_index
    }

    // Storage buffer descriptors the device must allow one shader stage to
    // access across sets 0-2 (maxPerStageDescriptorStorageBuffers), and a
    // pipeline layout to hold in total (maxDescriptorSetStorageBuffers).
    // The compute stage sees the most: seven buffers of set 0, one of set
    // 1 and two of set 2. The specification only guarantees 4 per stage,
    // so Vulkan_Device requires this value explicitly, and
    // Descriptor_Layout_Cache refuses layouts that exceed it.
    inline constexpr uint32_t Required_Storage_Buffers = 10;



    // Bindings dentro del set 3. Imagen y muestreador van en arrays
    // separados y el shader los combina en el punto de uso: asi el
    // material elige el filtrado sin depender de la textura.
    namespace Binding_Bindless
    {
        inline constexpr uint32_t Textures = GPU_BINDING_BINDLESS_TEXTURES;   // array de SAMPLED_IMAGE, una ranura por textura
        inline constexpr uint32_t Samplers = GPU_BINDING_BINDLESS_SAMPLERS;   // array de SAMPLER, ranura = valor de CoreTypes::Sampler_Preset
    }
}