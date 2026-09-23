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
        inline constexpr uint32_t Per_Frame    = 0;
        inline constexpr uint32_t Per_Pass     = 1;   // reservado, vacio
        inline constexpr uint32_t Per_Material = 2;   // reservado, vacio
        inline constexpr uint32_t Bindless     = 3;
        inline constexpr uint32_t Count        = 4;
    }

    // Bindings dentro del set 0.
    namespace Binding_Per_Frame
    {
        inline constexpr uint32_t Frame_UBO = 0;   // camara, inversas, tiempo
        inline constexpr uint32_t Lights = 1;   // SSBO con el array de luces
    }

    // Bindings dentro del set 3. Imagen y muestreador van en arrays
    // separados y el shader los combina en el punto de uso: asi el
    // material elige el filtrado sin depender de la textura.
    namespace Binding_Bindless
    {
        inline constexpr uint32_t Textures = 0;   // array de SAMPLED_IMAGE, una ranura por textura
        inline constexpr uint32_t Samplers = 1;   // array de SAMPLER, ranura = valor de CoreTypes::Sampler_Preset
    }
}