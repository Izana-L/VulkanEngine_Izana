#pragma once

#include <cstdint>

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
    namespace Descriptor_Set
    {
        inline constexpr uint32_t Per_Frame = 0;
        inline constexpr uint32_t Per_Pass = 1;   // resources written by a compute pass
        inline constexpr uint32_t Per_Material = 2;   // global material table, written on registration
        inline constexpr uint32_t Bindless = 3;
        inline constexpr uint32_t Count = 4;
    }

    // Bindings dentro del set 0.
    //
    // Bindings 3-7 hold buffers written on the GPU every frame. Each frame
    // slot has its own copy, so a frame never writes what the previous
    // frame, possibly still executing, reads.
    namespace Binding_Per_Frame
    {
        inline constexpr uint32_t Frame_UBO = 0;   // camara, inversas, tiempo
        inline constexpr uint32_t Lights = 1;   // SSBO con el array de luces
        inline constexpr uint32_t Objects = 2;     // SSBO of Object_GPU, one entry per draw, indexed by gl_InstanceIndex
        inline constexpr uint32_t Cluster_Grid = 3;            // SSBO of Cluster_Range_GPU, one per cluster: offset and count in the light index list
        inline constexpr uint32_t Cluster_Light_Indices = 4;   // SSBO of uint, compacted light indices of every cluster
        inline constexpr uint32_t Cluster_Counters = 5;        // SSBO of Cluster_Counters_GPU, atomic allocation counter of the list
        inline constexpr uint32_t Draw_Commands = 6;           // SSBO of VkDrawIndexedIndirectCommand written by the culling pass
        inline constexpr uint32_t Draw_Count = 7;              // SSBO of Draw_Count_GPU, atomic draw counter of the culling pass
    }

    // Bindings inside set 1. Visible to the compute stage only: graphics
    // pipelines read the outputs of a pass through the bindless set, never
    // through this one.
    namespace Binding_Per_Pass
    {
        inline constexpr uint32_t Procedural_Output = 0;   // STORAGE_IMAGE written by procedural.comp (layout GENERAL)
        inline constexpr uint32_t Cluster_AABBs = 1;       // SSBO of Cluster_AABB_GPU, view space, rebuilt when the projection changes
    }

    // Bindings inside set 2. One set for every frame: both tables are
    // append-only, so a frame in flight never reads a slot being written.
    namespace Binding_Per_Material
    {
        inline constexpr uint32_t Materials = 0;   // SSBO of Material_GPU, indexed by Object_GPU::material_index
        inline constexpr uint32_t Meshes = 1;      // SSBO of Mesh_Info_GPU, indexed by Object_GPU::mesh_index
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
        inline constexpr uint32_t Textures = 0;   // array de SAMPLED_IMAGE, una ranura por textura
        inline constexpr uint32_t Samplers = 1;   // array de SAMPLER, ranura = valor de CoreTypes::Sampler_Preset
    }
}