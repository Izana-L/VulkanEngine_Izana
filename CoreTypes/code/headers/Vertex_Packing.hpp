#pragma once

#include <Vertex.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace CoreTypes::Vertex_Packing
{

    // Vertex_Packing: converts the full-precision CPU vertices
    // (Vertex_*_CPU) into the packed layouts the GPU reads
    // (Vertex_Static_Mesh, Vertex_Skinned_Mesh). Each function encodes
    // exactly what the matching Vulkan format decodes (see the tables in
    // Vertex.hpp), so no shader has to unpack anything by hand.

    namespace Detail
    {
        // std::clamp lets NaN through (every comparison with it is
        // false), and converting a NaN float to an integer is undefined
        // behavior. Every quantizer goes through this first.
        inline float Clamp_not_nan(float _value, float _min, float _max)
        {
            if (std::isnan(_value)) return 0.0f;
            return std::clamp(_value, _min, _max);
        }

        // One 10-bit SNORM component, two's complement, in the low 10 bits.
        // Vulkan decodes it as max(c / 511, -1), so encoding with 511 keeps
        // 0 and +-1 exact and the error below 1/1022.
        inline uint32_t Snorm10(float _value)
        {
            const float value = Clamp_not_nan(_value, -1.0f, 1.0f);
            const int32_t quantized = static_cast<int32_t>(std::lround(value * 511.0f));   // [-511, 511]
            return static_cast<uint32_t>(quantized) & 0x3FFu;
        }

        inline uint8_t Unorm8(float _value)
        {
            const float value = Clamp_not_nan(_value, 0.0f, 1.0f);
            return static_cast<uint8_t>(std::lround(value * 255.0f));
        }

        // Unit length before quantizing, so no component gets clamped and
        // all of them use the full [-1, 1] range. A zero or non-finite
        // vector is passed through: it was already invalid as a float.
        inline MathLib::Vector3 Normalize_if_possible(const MathLib::Vector3& _vector)
        {
            const float length = glm::length(_vector);
            return (std::isfinite(length) && length > 1e-8f) ? _vector / length : _vector;
        }

        // A2B10G10R10_*_PACK32 word: the first component in the name (A)
        // is in the most significant bits, so x = bits 0-9, y = 10-19,
        // z = 20-29, w = 30-31.
        inline uint32_t Pack_2_10_10_10(const MathLib::Vector3& _xyz, uint32_t _w_bits)
        {
            return  Snorm10(_xyz.x)
                | (Snorm10(_xyz.y) << 10)
                | (Snorm10(_xyz.z) << 20)
                | ((_w_bits & 0x3u) << 30);
        }
    }

    // A2B10G10R10_SNORM_PACK32, w = 0 (the shader reads only xyz).
    inline uint32_t Pack_normal(const MathLib::Vector3& _normal)
    {
        return Detail::Pack_2_10_10_10(Detail::Normalize_if_possible(_normal), 0u);
    }

    // A2B10G10R10_SNORM_PACK32, w = bitangent sign. As a 2-bit SNORM,
    // 0b01 decodes to +1.0 and 0b11 to -1.0, both exactly.
    inline uint32_t Pack_tangent(const MathLib::Vector4& _tangent)
    {
        const uint32_t sign_bits = (_tangent.w < 0.0f) ? 0x3u : 0x1u;
        return Detail::Pack_2_10_10_10(Detail::Normalize_if_possible(MathLib::Vector3(_tangent)), sign_bits);
    }

    // R8G8B8A8_UNORM. Values outside [0, 1] are clamped.
    inline std::array<uint8_t, 4> Pack_color(const MathLib::Vector4& _color)
    {
        return { Detail::Unorm8(_color.x), Detail::Unorm8(_color.y),
                 Detail::Unorm8(_color.z), Detail::Unorm8(_color.w) };
    }

    // R16G16B16A16_UINT. Throws instead of wrapping around: a truncated
    // index would silently bind the vertex to the wrong bone.
    inline std::array<uint16_t, 4> Pack_bone_indices(const std::array<uint32_t, 4>& _indices)
    {
        std::array<uint16_t, 4> packed{};

        for (size_t i = 0; i < 4; ++i)
        {
            if (_indices[i] > UINT16_MAX)
                throw std::out_of_range("Vertex_Packing: bone index " + std::to_string(_indices[i]) +
                    " does not fit in 16 bits");

            packed[i] = static_cast<uint16_t>(_indices[i]);
        }

        return packed;
    }

    // R8G8B8A8_UNORM, renormalized so the four bytes add up to exactly
    // 255. Rounding each weight on its own can leave the sum at 253..257,
    // and a sum other than 1.0 in the shader scales the skinned vertex.
    inline std::array<uint8_t, 4> Pack_bone_weights(const MathLib::Vector4& _weights)
    {
        float weights[4];
        float sum = 0.0f;

        for (int i = 0; i < 4; ++i)
        {
            // Negative and NaN weights contribute nothing.
            weights[i] = (_weights[i] > 0.0f) ? _weights[i] : 0.0f;
            sum += weights[i];
        }

        std::array<uint8_t, 4> packed{};

        // No usable weight at all: bind the vertex fully to its first
        // bone instead of collapsing it to the origin.
        if (!std::isfinite(sum) || sum <= 0.0f)
        {
            packed[0] = 255;
            return packed;
        }

        int total = 0;
        int largest = 0;

        for (int i = 0; i < 4; ++i)
        {
            // weights[i] <= sum, so this stays in [0, 255].
            const int quantized = static_cast<int>(std::lround(weights[i] / sum * 255.0f));
            packed[i] = static_cast<uint8_t>(quantized);
            total += quantized;

            if (weights[i] > weights[largest]) largest = i;
        }

        // The rounding error (at most +-2) goes to the largest weight,
        // which is at least 64 and at most 255 minus the other three, so
        // the result always stays in [0, 255].
        packed[largest] = static_cast<uint8_t>(packed[largest] + (255 - total));

        return packed;
    }

    inline Vertex_Static_Mesh Pack(const Vertex_Static_Mesh_CPU& _vertex)
    {
        Vertex_Static_Mesh packed;
        packed.position = _vertex.position;
        packed.normal = Pack_normal(_vertex.normal);
        packed.tangent = Pack_tangent(_vertex.tangent);
        packed.uv = _vertex.uv;
        packed.color = Pack_color(_vertex.color);
        return packed;
    }

    inline Vertex_Skinned_Mesh Pack(const Vertex_Skinned_Mesh_CPU& _vertex)
    {
        Vertex_Skinned_Mesh packed;
        packed.position = _vertex.position;
        packed.normal = Pack_normal(_vertex.normal);
        packed.tangent = Pack_tangent(_vertex.tangent);
        packed.uv = _vertex.uv;
        packed.color = Pack_color(_vertex.color);
        packed.bone_indices = Pack_bone_indices(_vertex.bone_indices);
        packed.bone_weights = Pack_bone_weights(_vertex.bone_weights);
        return packed;
    }

} // namespace CoreTypes::Vertex_Packing