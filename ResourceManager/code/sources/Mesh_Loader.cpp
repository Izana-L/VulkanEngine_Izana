#include <Mesh_Loader.hpp>
#include <Mesh_Tangents.hpp>

#include <Tinygltf_Include.hpp>

#include <Matrix4.hpp>
#include <Quaternion.hpp>
#include <Vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ResourceManager::Mesh_Loader
{

    // =========================================================
    // Internal helpers
    // =========================================================

    namespace
    {
        using Vertex = CoreTypes::Vertex_Static_Mesh_CPU;
        using Mesh = CoreTypes::MeshData;
        using Vec3 = MathLib::Vector3;
        using Matrix4 = MathLib::Matrix4;

        // Image loader registered with tinygltf. Tinygltf_Include.hpp builds
        // the library without a default one, and loading fails with "No
        // LoadImageData callback specified" unless a callback is set.
        // Textures are loaded through Image_Loader, not through glTF images,
        // so this callback only has to satisfy the API.
        bool Dummy_load_image(tinygltf::Image* /*_image*/,
            const int            /*_image_idx*/,
            std::string* /*_err*/,
            std::string* /*_warn*/,
            int                  /*_req_width*/,
            int                  /*_req_height*/,
            const unsigned char* /*_bytes*/,
            int                  /*_size*/,
            void* /*_user_data*/)
        {
            return true;
        }

        // =========================================================
        // Checked arithmetic
        // =========================================================

        // Every size computed from a file field goes through these: the
        // fields are attacker-controlled 64-bit integers, and an unchecked
        // sum or product wraps to a small number that passes a bounds check
        // and then addresses memory far outside the buffer.

        bool Add_fits(size_t _a, size_t _b, size_t& _out)
        {
            if (_a > std::numeric_limits<size_t>::max() - _b) return false;
            _out = _a + _b;
            return true;
        }

        bool Mul_fits(size_t _a, size_t _b, size_t& _out)
        {
            if (_a != 0 && _b > std::numeric_limits<size_t>::max() / _a) return false;
            _out = _a * _b;
            return true;
        }

        // Vertex and index values are 32-bit in MeshData, so no accessor may
        // describe more elements than that.
        constexpr size_t MAX_ELEMENTS = std::numeric_limits<uint32_t>::max();

        const char* Type_name(int _type)
        {
            switch (_type)
            {
            case TINYGLTF_TYPE_SCALAR: return "SCALAR";
            case TINYGLTF_TYPE_VEC2:   return "VEC2";
            case TINYGLTF_TYPE_VEC3:   return "VEC3";
            case TINYGLTF_TYPE_VEC4:   return "VEC4";
            case TINYGLTF_TYPE_MAT2:   return "MAT2";
            case TINYGLTF_TYPE_MAT3:   return "MAT3";
            case TINYGLTF_TYPE_MAT4:   return "MAT4";
            default:                   return "an unknown type";
            }
        }

        const char* Component_name(int _component_type)
        {
            switch (_component_type)
            {
            case TINYGLTF_COMPONENT_TYPE_BYTE:           return "BYTE";
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return "UNSIGNED_BYTE";
            case TINYGLTF_COMPONENT_TYPE_SHORT:          return "SHORT";
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return "UNSIGNED_SHORT";
            case TINYGLTF_COMPONENT_TYPE_INT:            return "INT";
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return "UNSIGNED_INT";
            case TINYGLTF_COMPONENT_TYPE_FLOAT:          return "FLOAT";
            case TINYGLTF_COMPONENT_TYPE_DOUBLE:         return "DOUBLE";
            default:                                     return "an unknown component type";
            }
        }

        // =========================================================
        // Accessor layout
        // =========================================================

        // Where the elements of an accessor live, with the layout resolved:
        // a base pointer, the stride between elements (which honors
        // bufferView.byteStride for interleaved vertex data) and the size
        // of one component. Resolve_layout proves, before the struct is
        // handed out, that all `count` elements lie inside the buffer view
        // and the buffer, so reading element i < count at base + i * stride
        // can never leave the buffer.
        struct Accessor_Layout
        {
            const uint8_t* base = nullptr;   // nullptr when the accessor has no buffer view
            size_t         stride = 0;       // bytes between consecutive elements
            int            components = 0;   // components per element (VEC3 -> 3)
            int            component_size = 0;
            uint32_t       component_type = 0;
            bool           normalized = false;
            size_t         count = 0;
        };

        Accessor_Layout Resolve_layout(const tinygltf::Model& _model,
            const tinygltf::Accessor& _accessor,
            const char* _what)
        {
            const std::string what = _what;

            if (_accessor.sparse.isSparse)
                throw std::runtime_error("Mesh_Loader: " + what + " uses a sparse accessor, which is not supported");

            Accessor_Layout layout;
            layout.components = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(_accessor.type));
            layout.component_size = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(_accessor.componentType));
            layout.component_type = static_cast<uint32_t>(_accessor.componentType);
            layout.normalized = _accessor.normalized;
            layout.count = _accessor.count;

            if (layout.components <= 0 || layout.component_size <= 0)
                throw std::runtime_error("Mesh_Loader: " + what + " has an unknown accessor type or component type");

            // The specification requires at least one element, and every
            // size below is derived from count - 1.
            if (layout.count == 0)
                throw std::runtime_error("Mesh_Loader: " + what + " has no elements");

            if (layout.count > MAX_ELEMENTS)
                throw std::runtime_error("Mesh_Loader: " + what + " has " + std::to_string(layout.count) +
                    " elements, more than the 32-bit limit");

            // Per the glTF specification an accessor without a buffer view
            // reads as all zeros. Represented by a null base.
            if (_accessor.bufferView < 0)
                return layout;

            if (static_cast<size_t>(_accessor.bufferView) >= _model.bufferViews.size())
                throw std::runtime_error("Mesh_Loader: " + what + " references a buffer view out of range");

            const tinygltf::BufferView& view = _model.bufferViews[static_cast<size_t>(_accessor.bufferView)];

            if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= _model.buffers.size())
                throw std::runtime_error("Mesh_Loader: " + what + " references a buffer out of range");

            const tinygltf::Buffer& buffer = _model.buffers[static_cast<size_t>(view.buffer)];

            // ByteStride() returns the explicit view stride when the view is
            // interleaved, or the packed element size otherwise (-1 on error).
            const int stride = _accessor.ByteStride(view);
            if (stride <= 0)
                throw std::runtime_error("Mesh_Loader: " + what + " has an invalid byte stride");

            layout.stride = static_cast<size_t>(stride);

            const size_t element_size = static_cast<size_t>(layout.components) * static_cast<size_t>(layout.component_size);

            // Elements that overlap each other are not a layout glTF allows.
            if (layout.stride < element_size)
                throw std::runtime_error("Mesh_Loader: " + what + " has a byte stride smaller than one element");

            // 1. The buffer view must lie inside its buffer.
            size_t view_end = 0;
            if (!Add_fits(view.byteOffset, view.byteLength, view_end) || view_end > buffer.data.size())
                throw std::runtime_error("Mesh_Loader: " + what + " reads past the end of its buffer");

            // 2. The accessor must lie inside its buffer view: from its own
            //    offset to the end of its last element.
            size_t span = 0;   // bytes from the first element's start to the last element's end
            size_t accessor_end = 0;
            if (!Mul_fits(layout.count - 1, layout.stride, span) ||
                !Add_fits(span, element_size, span) ||
                !Add_fits(_accessor.byteOffset, span, accessor_end) ||
                accessor_end > view.byteLength)
            {
                throw std::runtime_error("Mesh_Loader: " + what + " reads past the end of its buffer view");
            }

            // Both offsets are bounded by the checks above, so this sum is
            // at most the buffer size and cannot wrap.
            layout.base = buffer.data.data() + view.byteOffset + _accessor.byteOffset;

            return layout;
        }

        // =========================================================
        // Vertex attributes
        // =========================================================

        // Converts one component to float, applying the normalization rule
        // the glTF specification defines for each integer type.
        float Read_component(const uint8_t* _ptr, uint32_t _component_type, bool _normalized)
        {
            switch (_component_type)
            {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
            {
                float value;
                std::memcpy(&value, _ptr, sizeof(float));
                return value;
            }
            case TINYGLTF_COMPONENT_TYPE_BYTE:
            {
                int8_t value;
                std::memcpy(&value, _ptr, sizeof(value));
                return _normalized ? std::max(static_cast<float>(value) / 127.0f, -1.0f)
                    : static_cast<float>(value);
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            {
                uint8_t value;
                std::memcpy(&value, _ptr, sizeof(value));
                return _normalized ? static_cast<float>(value) / 255.0f
                    : static_cast<float>(value);
            }
            case TINYGLTF_COMPONENT_TYPE_SHORT:
            {
                int16_t value;
                std::memcpy(&value, _ptr, sizeof(value));
                return _normalized ? std::max(static_cast<float>(value) / 32767.0f, -1.0f)
                    : static_cast<float>(value);
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            {
                uint16_t value;
                std::memcpy(&value, _ptr, sizeof(value));
                return _normalized ? static_cast<float>(value) / 65535.0f
                    : static_cast<float>(value);
            }
            default:
                throw std::runtime_error("Mesh_Loader: unsupported attribute component type " +
                    std::to_string(_component_type));
            }
        }

        // What an attribute means decides which accessor types it may have.
        enum class Semantic { Position, Normal, Tangent, Uv, Color };

        // One supported vertex attribute: its glTF name, what it means, and
        // how it is stored once read (components per vertex, and the value
        // that fills the components the file does not provide: a VEC3 color
        // gets alpha = 1).
        struct Attribute_Info
        {
            const char* name;
            Semantic    semantic;
            int         components;
            float       pad_value;
        };

        constexpr Attribute_Info ATTRIBUTE_POSITION = { "POSITION",   Semantic::Position, 3, 0.0f };
        constexpr Attribute_Info ATTRIBUTE_NORMAL = { "NORMAL",     Semantic::Normal,   3, 0.0f };
        constexpr Attribute_Info ATTRIBUTE_TANGENT = { "TANGENT",    Semantic::Tangent,  4, 1.0f };
        constexpr Attribute_Info ATTRIBUTE_TEXCOORD_0 = { "TEXCOORD_0", Semantic::Uv,       2, 0.0f };
        constexpr Attribute_Info ATTRIBUTE_COLOR_0 = { "COLOR_0",    Semantic::Color,    4, 1.0f };

        bool Has_extension(const std::vector<std::string>& _list, const char* _name)
        {
            return std::find(_list.begin(), _list.end(), _name) != _list.end();
        }

        // The accessor type the specification fixes for each attribute.
        bool Type_allowed(Semantic _semantic, int _type)
        {
            switch (_semantic)
            {
            case Semantic::Position:
            case Semantic::Normal:   return _type == TINYGLTF_TYPE_VEC3;
            case Semantic::Tangent:  return _type == TINYGLTF_TYPE_VEC4;
            case Semantic::Uv:       return _type == TINYGLTF_TYPE_VEC2;
            case Semantic::Color:    return _type == TINYGLTF_TYPE_VEC3 || _type == TINYGLTF_TYPE_VEC4;
            }
            return false;
        }

        // The component types the specification allows for each attribute.
        //   core:   POSITION / NORMAL / TANGENT are FLOAT; TEXCOORD and COLOR
        //           may also be normalized UNSIGNED_BYTE / UNSIGNED_SHORT.
        //   KHR_mesh_quantization widens POSITION and TEXCOORD to any 8/16-bit
        //   integer, and NORMAL / TANGENT to normalized BYTE / SHORT. Colors
        //   are not touched by it.
        // UNSIGNED_INT, INT and DOUBLE are never valid for an attribute.
        bool Component_allowed(Semantic _semantic, uint32_t _component_type, bool _normalized, bool _quantization)
        {
            if (_component_type == TINYGLTF_COMPONENT_TYPE_FLOAT)
                return true;

            const bool is_signed = _component_type == TINYGLTF_COMPONENT_TYPE_BYTE ||
                _component_type == TINYGLTF_COMPONENT_TYPE_SHORT;
            const bool is_small_int = is_signed ||
                _component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
                _component_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;

            if (!is_small_int)
                return false;

            switch (_semantic)
            {
            case Semantic::Color:    return !is_signed && _normalized;
            case Semantic::Uv:       return (!is_signed && _normalized) || _quantization;
            case Semantic::Position: return _quantization;
            case Semantic::Normal:
            case Semantic::Tangent:  return _quantization && is_signed && _normalized;
            }
            return false;
        }

        // Rejects an accessor whose type cannot hold the attribute it is
        // used for. Without this a POSITION stored as SCALAR or MAT4 reads
        // as plausible garbage (the missing components are padded, the extra
        // ones dropped) and nothing reports it.
        void Validate_attribute(const tinygltf::Model& _model,
            const tinygltf::Accessor& _accessor,
            const Attribute_Info& _info)
        {
            if (!Type_allowed(_info.semantic, _accessor.type))
            {
                const char* expected = _info.semantic == Semantic::Color ? "VEC3 or VEC4"
                    : _info.components == 2 ? "VEC2"
                    : _info.components == 4 ? "VEC4" : "VEC3";

                throw std::runtime_error(std::string("Mesh_Loader: attribute ") + _info.name + " must be " +
                    expected + ", but its accessor is " + Type_name(_accessor.type));
            }

            const bool quantization = Has_extension(_model.extensionsUsed, "KHR_mesh_quantization");

            if (!Component_allowed(_info.semantic, static_cast<uint32_t>(_accessor.componentType),
                _accessor.normalized, quantization))
            {
                // Only offer the extension as a way out when it could help:
                // 32-bit and double components are never valid.
                const bool small_int = _accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
                    _accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
                    _accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT ||
                    _accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;

                throw std::runtime_error(std::string("Mesh_Loader: attribute ") + _info.name + " cannot be stored as " +
                    (_accessor.normalized ? "normalized " : "") + Component_name(_accessor.componentType) +
                    ((small_int && !quantization) ? " (that needs the KHR_mesh_quantization extension)" : ""));
            }
        }

        // Reads a validated attribute as `_info.components` floats per
        // element. Elements with fewer components (a VEC3 color) are padded
        // with _info.pad_value.
        std::vector<float> Read_float_attribute(const tinygltf::Model& _model,
            const tinygltf::Accessor& _accessor,
            const Attribute_Info& _info)
        {
            Validate_attribute(_model, _accessor, _info);

            const Accessor_Layout layout = Resolve_layout(_model, _accessor, _info.name);
            const size_t wanted = static_cast<size_t>(_info.components);

            // A position stream without data is not "all zeros" in any useful
            // sense: it is compressed data this loader cannot decode (Draco,
            // meshopt) or a file that forgot its geometry.
            if (layout.base == nullptr && _info.semantic == Semantic::Position)
                throw std::runtime_error("Mesh_Loader: POSITION has no buffer view (geometry compressed with an "
                    "unsupported extension, or missing)");

            size_t total = 0;
            if (!Mul_fits(layout.count, wanted, total))
                throw std::runtime_error(std::string("Mesh_Loader: attribute ") + _info.name + " is too large");

            std::vector<float> out(total, _info.pad_value);

            const size_t copied = std::min(wanted, static_cast<size_t>(layout.components));

            for (size_t i = 0; i < layout.count; ++i)
            {
                // No buffer view: the specification defines the components as zeros.
                if (layout.base == nullptr)
                {
                    for (size_t c = 0; c < copied; ++c)
                        out[i * wanted + c] = 0.0f;
                    continue;
                }

                const uint8_t* element = layout.base + i * layout.stride;

                for (size_t c = 0; c < copied; ++c)
                {
                    out[i * wanted + c] = Read_component(
                        element + c * static_cast<size_t>(layout.component_size),
                        layout.component_type,
                        layout.normalized);
                }
            }

            return out;
        }

        // Fetches the accessor of a named attribute, or nullptr if absent.
        const tinygltf::Accessor* Find_attribute(const tinygltf::Model& _model,
            const tinygltf::Primitive& _primitive,
            const char* _name)
        {
            auto it = _primitive.attributes.find(_name);
            if (it == _primitive.attributes.end()) return nullptr;

            if (it->second < 0 || static_cast<size_t>(it->second) >= _model.accessors.size())
                throw std::runtime_error(std::string("Mesh_Loader: attribute ") + _name + " references an accessor out of range");

            return &_model.accessors[static_cast<size_t>(it->second)];
        }

        // =========================================================
        // Indices and primitive modes
        // =========================================================

        // Reads the index accessor, widening every index to uint32_t and
        // validating that each one addresses an existing vertex.
        std::vector<uint32_t> Read_indices(const tinygltf::Model& _model,
            const tinygltf::Accessor& _accessor,
            uint32_t                  _vertex_count)
        {
            if (_accessor.type != TINYGLTF_TYPE_SCALAR)
                throw std::runtime_error(std::string("Mesh_Loader: index accessor must be SCALAR, but it is ") +
                    Type_name(_accessor.type));

            switch (_accessor.componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                break;
            default:
                throw std::runtime_error(std::string("Mesh_Loader: unsupported index component type: ") +
                    Component_name(_accessor.componentType));
            }

            const Accessor_Layout layout = Resolve_layout(_model, _accessor, "index buffer");

            if (layout.base == nullptr)
                throw std::runtime_error("Mesh_Loader: index accessor has no buffer view");

            std::vector<uint32_t> indices(layout.count);

            for (size_t i = 0; i < layout.count; ++i)
            {
                const uint8_t* element = layout.base + i * layout.stride;
                uint32_t value = 0;

                switch (layout.component_type)
                {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                {
                    uint8_t v; std::memcpy(&v, element, sizeof(v)); value = v; break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                {
                    uint16_t v; std::memcpy(&v, element, sizeof(v)); value = v; break;
                }
                default:
                {
                    std::memcpy(&value, element, sizeof(value)); break;
                }
                }

                if (value >= _vertex_count)
                {
                    throw std::runtime_error("Mesh_Loader: index " + std::to_string(value) +
                        " is out of range for " + std::to_string(_vertex_count) + " vertices");
                }

                indices[i] = value;
            }

            return indices;
        }

        // Turns the index stream of a TRIANGLES / TRIANGLE_STRIP /
        // TRIANGLE_FAN primitive into a plain triangle list, which is the
        // only topology MeshData holds. Strips and fans are expanded with
        // the winding the specification defines, so every triangle stays
        // counter-clockwise; triangles that repeat an index (how strips are
        // stitched together) are dropped.
        std::vector<uint32_t> To_triangle_list(int _mode, std::vector<uint32_t> _stream)
        {
            if (_mode == TINYGLTF_MODE_TRIANGLES)
            {
                if (_stream.size() % 3 != 0)
                    throw std::runtime_error("Mesh_Loader: index count " + std::to_string(_stream.size()) +
                        " is not a multiple of 3 for a triangle primitive");

                return _stream;
            }

            if (_stream.size() < 3)
                throw std::runtime_error("Mesh_Loader: a triangle strip or fan needs at least 3 indices, found " +
                    std::to_string(_stream.size()));

            std::vector<uint32_t> triangles;
            triangles.reserve((_stream.size() - 2) * 3);

            const auto emit = [&](uint32_t _a, uint32_t _b, uint32_t _c)
                {
                    if (_a == _b || _b == _c || _a == _c) return;
                    triangles.push_back(_a);
                    triangles.push_back(_b);
                    triangles.push_back(_c);
                };

            for (size_t i = 0; i + 2 < _stream.size(); ++i)
            {
                if (_mode == TINYGLTF_MODE_TRIANGLE_STRIP)
                {
                    // Odd triangles swap their first two vertices to keep the winding.
                    if (i % 2 == 0) emit(_stream[i], _stream[i + 1], _stream[i + 2]);
                    else            emit(_stream[i + 1], _stream[i], _stream[i + 2]);
                }
                else
                {
                    emit(_stream[0], _stream[i + 1], _stream[i + 2]);
                }
            }

            return triangles;
        }

        // =========================================================
        // Geometry derived from what the file left out
        // =========================================================

        // glTF: "When normals are not specified, client implementations
        // MUST calculate flat normals". A flat normal belongs to a face, not
        // to a vertex, so the vertices cannot stay shared between triangles:
        // every triangle gets its own three, carrying the face normal.
        // Mesh_Optimizer merges the copies that turn out identical (coplanar
        // neighbors) afterwards.
        void Unweld_with_flat_normals(Mesh& _mesh)
        {
            std::vector<Vertex> unwelded;
            unwelded.reserve(_mesh.indices.size());

            for (size_t i = 0; i < _mesh.indices.size(); i += 3)
            {
                const Vertex corners[3] = { _mesh.vertices[_mesh.indices[i + 0]],
                                            _mesh.vertices[_mesh.indices[i + 1]],
                                            _mesh.vertices[_mesh.indices[i + 2]] };

                const Vec3 cross = MathLib::Vec3::Cross(corners[1].position - corners[0].position,
                    corners[2].position - corners[0].position);

                // A zero-area triangle has no normal and covers no pixels;
                // any unit vector keeps the data finite.
                const Vec3 normal = (MathLib::Vec3::Length_squared(cross) > 0.0f)
                    ? MathLib::Vec3::Normalize(cross) : Vec3(0.0f, 1.0f, 0.0f);

                for (Vertex corner : corners)
                {
                    corner.normal = normal;
                    unwelded.push_back(corner);
                }
            }

            _mesh.vertices = std::move(unwelded);

            for (size_t i = 0; i < _mesh.indices.size(); ++i)
                _mesh.indices[i] = static_cast<uint32_t>(i);
        }

        // =========================================================
        // Primitive extraction
        // =========================================================

        // Attributes this loader reads; anything else on a primitive is
        // reported as ignored.
        bool Is_supported_attribute(const std::string& _name)
        {
            return _name == ATTRIBUTE_POSITION.name || _name == ATTRIBUTE_NORMAL.name || _name == ATTRIBUTE_TANGENT.name ||
                _name == ATTRIBUTE_TEXCOORD_0.name || _name == ATTRIBUTE_COLOR_0.name;
        }

        // Appends the primitive as a MeshData in the mesh's own space (no node
        // transform), or nothing if it is not a triangle primitive.
        void Extract_primitive(const tinygltf::Model& _model,
            const tinygltf::Primitive& _primitive,
            std::vector<Mesh>& _out)
        {
            const int mode = _primitive.mode;

            if (mode != TINYGLTF_MODE_TRIANGLES &&
                mode != TINYGLTF_MODE_TRIANGLE_STRIP &&
                mode != TINYGLTF_MODE_TRIANGLE_FAN)
            {
                std::cout << "[Mesh_Loader] Skipping non-triangle primitive (glTF mode " << mode << ").\n";
                return;
            }

            // POSITION (required) ---------------------------------------
            const tinygltf::Accessor* pos_accessor = Find_attribute(_model, _primitive, ATTRIBUTE_POSITION.name);
            if (!pos_accessor)
                throw std::runtime_error("Mesh_Loader: primitive has no POSITION attribute");

            const std::vector<float> positions = Read_float_attribute(_model, *pos_accessor, ATTRIBUTE_POSITION);

            // Read_float_attribute proved the count is non-zero and fits 32 bits.
            const uint32_t vertex_count = static_cast<uint32_t>(pos_accessor->count);

            // Optional attributes ---------------------------------------
            // Each one must have exactly one element per vertex; a mismatch
            // means the file is malformed and the read would run past the
            // shorter array.
            const auto read_optional = [&](const Attribute_Info& info) -> std::vector<float>
                {
                    const tinygltf::Accessor* accessor = Find_attribute(_model, _primitive, info.name);
                    if (!accessor) return {};

                    if (accessor->count != pos_accessor->count)
                    {
                        throw std::runtime_error(std::string("Mesh_Loader: attribute ") + info.name + " has " +
                            std::to_string(accessor->count) + " elements but POSITION has " +
                            std::to_string(pos_accessor->count));
                    }

                    return Read_float_attribute(_model, *accessor, info);
                };

            const std::vector<float> normals = read_optional(ATTRIBUTE_NORMAL);
            const std::vector<float> uvs = read_optional(ATTRIBUTE_TEXCOORD_0);
            const std::vector<float> tangents = read_optional(ATTRIBUTE_TANGENT);
            const std::vector<float> colors = read_optional(ATTRIBUTE_COLOR_0);

            // Build vertices --------------------------------------------
            Mesh mesh_data;
            mesh_data.vertices.resize(vertex_count);

            for (uint32_t i = 0; i < vertex_count; ++i)
            {
                Vertex& v = mesh_data.vertices[i];

                v.position = { positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2] };

                // Missing normals are generated below, once the triangles are known.
                v.normal = !normals.empty()
                    ? Vec3{ normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2] }
                    : Vec3{ 0.0f, 1.0f, 0.0f };

                v.uv = !uvs.empty()
                    ? MathLib::Vector2{ uvs[i * 2 + 0], uvs[i * 2 + 1] }
                    : MathLib::Vector2{ 0.0f, 0.0f };

                v.tangent = !tangents.empty()
                    ? MathLib::Vector4{ tangents[i * 4 + 0], tangents[i * 4 + 1], tangents[i * 4 + 2], tangents[i * 4 + 3] }
                    : MathLib::Vector4{ 1.0f, 0.0f, 0.0f, 1.0f };

                v.color = !colors.empty()
                    ? MathLib::Vector4{ colors[i * 4 + 0], colors[i * 4 + 1], colors[i * 4 + 2], colors[i * 4 + 3] }
                    : MathLib::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
            }

            // Indices ---------------------------------------------------
            // A primitive without an index accessor is drawn straight from
            // the vertex array: vertex i is index i.
            std::vector<uint32_t> stream;

            if (_primitive.indices >= 0)
            {
                if (static_cast<size_t>(_primitive.indices) >= _model.accessors.size())
                    throw std::runtime_error("Mesh_Loader: index accessor out of range");

                stream = Read_indices(_model, _model.accessors[static_cast<size_t>(_primitive.indices)], vertex_count);
            }
            else
            {
                stream.resize(vertex_count);
                for (uint32_t i = 0; i < vertex_count; ++i) stream[i] = i;
            }

            mesh_data.indices = To_triangle_list(mode, std::move(stream));

            if (mesh_data.indices.empty())
            {
                std::cout << "[Mesh_Loader] Skipping primitive without any non-degenerate triangle.\n";
                return;
            }

            // Derived attributes ----------------------------------------
            if (normals.empty())
                Unweld_with_flat_normals(mesh_data);

            // Tangents come from the UVs and normals just set; a mesh with
            // no UVs gets a finite, normal-perpendicular frame.
            if (tangents.empty())
                Mesh_Tangents::Compute(mesh_data);

            _out.push_back(std::move(mesh_data));
        }

        // =========================================================
        // Scene graph
        // =========================================================

        // glTF stores a node's transform either as a 4x4 matrix (column
        // major) or as translation / rotation (x, y, z, w) / scale.
        Matrix4 Local_matrix(const tinygltf::Node& _node, int _index)
        {
            const std::string where = "Mesh_Loader: node " + std::to_string(_index);

            const auto require_finite = [&](const std::vector<double>& values)
                {
                    for (double value : values)
                        if (!std::isfinite(value))
                            throw std::runtime_error(where + " has a non-finite transform value");
                };

            if (!_node.matrix.empty())
            {
                if (_node.matrix.size() != 16)
                    throw std::runtime_error(where + " has a matrix with " + std::to_string(_node.matrix.size()) +
                        " values instead of 16");

                require_finite(_node.matrix);

                Matrix4 matrix(1.0f);
                for (int i = 0; i < 16; ++i)
                    matrix[i / 4][i % 4] = static_cast<float>(_node.matrix[static_cast<size_t>(i)]);

                return matrix;
            }

            if ((!_node.translation.empty() && _node.translation.size() != 3) ||
                (!_node.rotation.empty() && _node.rotation.size() != 4) ||
                (!_node.scale.empty() && _node.scale.size() != 3))
            {
                throw std::runtime_error(where + " has a translation, rotation or scale of the wrong length");
            }

            require_finite(_node.translation);
            require_finite(_node.rotation);
            require_finite(_node.scale);

            Vec3 translation(0.0f);
            Vec3 scale(1.0f);
            MathLib::Quat::Quaternion rotation = MathLib::Quat::Identity();

            if (!_node.translation.empty())
                translation = { static_cast<float>(_node.translation[0]), static_cast<float>(_node.translation[1]),
                                static_cast<float>(_node.translation[2]) };

            if (!_node.scale.empty())
                scale = { static_cast<float>(_node.scale[0]), static_cast<float>(_node.scale[1]),
                          static_cast<float>(_node.scale[2]) };

            if (!_node.rotation.empty())
            {
                // glTF orders the quaternion x, y, z, w; the math library's constructor w, x, y, z.
                rotation = MathLib::Quat::Quaternion(static_cast<float>(_node.rotation[3]), static_cast<float>(_node.rotation[0]),
                    static_cast<float>(_node.rotation[1]), static_cast<float>(_node.rotation[2]));

                if (!(MathLib::Quat::Length(rotation) > 0.0f))
                    throw std::runtime_error(where + " has a zero-length rotation quaternion");

                rotation = MathLib::Quat::Normalize(rotation);
            }

            return MathLib::Mat4::TRS(translation, rotation, scale);
        }

        // One placement of a glTF mesh in the world.
        struct Mesh_Instance
        {
            size_t mesh = 0;
            Matrix4   world = Matrix4(1.0f);
        };

        // Resolves what the file actually shows: the meshes of the nodes
        // reachable from the default scene (scene 0 when none is marked), with
        // the product of the transforms down the node hierarchy. A mesh used
        // by several nodes yields several instances.
        //
        // Files without a node graph (meshes only) have nothing to place, so
        // each mesh appears once, untransformed.
        std::vector<Mesh_Instance> Collect_instances(const tinygltf::Model& _model)
        {
            std::vector<Mesh_Instance> instances;

            if (_model.nodes.empty())
            {
                for (size_t i = 0; i < _model.meshes.size(); ++i)
                    instances.push_back({ i, Matrix4(1.0f) });

                return instances;
            }

            // Roots: the nodes of the scene, or, for a file with no scenes,
            // every node that is nobody's child.
            std::vector<int> roots;

            if (!_model.scenes.empty())
            {
                const int scene = _model.defaultScene >= 0 ? _model.defaultScene : 0;

                if (static_cast<size_t>(scene) >= _model.scenes.size())
                    throw std::runtime_error("Mesh_Loader: default scene " + std::to_string(scene) + " does not exist");

                roots = _model.scenes[static_cast<size_t>(scene)].nodes;
            }
            else
            {
                std::vector<char> is_child(_model.nodes.size(), 0);

                for (const tinygltf::Node& node : _model.nodes)
                    for (int child : node.children)
                        if (child >= 0 && static_cast<size_t>(child) < is_child.size())
                            is_child[static_cast<size_t>(child)] = 1;

                for (size_t i = 0; i < is_child.size(); ++i)
                    if (!is_child[i]) roots.push_back(static_cast<int>(i));
            }

            // Depth-first, children in file order. Iterative: a hand-made
            // file can chain hundreds of thousands of nodes, which would
            // overflow the call stack of a recursive walk.
            struct Frame { int node; Matrix4 parent_world; };

            std::vector<Frame> pending;
            for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                pending.push_back({ *it, Matrix4(1.0f) });

            // glTF nodes form a forest: each has at most one parent and there
            // are no cycles. Reaching one twice means the file breaks that,
            // and a cycle would otherwise never terminate.
            std::vector<char> visited(_model.nodes.size(), 0);

            while (!pending.empty())
            {
                const Frame frame = pending.back();
                pending.pop_back();

                if (frame.node < 0 || static_cast<size_t>(frame.node) >= _model.nodes.size())
                    throw std::runtime_error("Mesh_Loader: node index " + std::to_string(frame.node) + " is out of range");

                if (visited[static_cast<size_t>(frame.node)])
                    throw std::runtime_error("Mesh_Loader: node " + std::to_string(frame.node) +
                        " is reached twice (a cycle, or a node with two parents)");

                visited[static_cast<size_t>(frame.node)] = 1;

                const tinygltf::Node& node = _model.nodes[static_cast<size_t>(frame.node)];
                const Matrix4 world = frame.parent_world * Local_matrix(node, frame.node);

                if (node.mesh >= 0)
                {
                    if (static_cast<size_t>(node.mesh) >= _model.meshes.size())
                        throw std::runtime_error("Mesh_Loader: node " + std::to_string(frame.node) +
                            " references a mesh out of range");

                    instances.push_back({ static_cast<size_t>(node.mesh), world });
                }

                for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
                    pending.push_back({ *it, world });
            }

            return instances;
        }

        // Bakes a world transform into the vertices. Positions take the full
        // matrix; normals the inverse-transpose of its 3x3, which keeps them
        // perpendicular under non-uniform scale; tangents the 3x3 itself,
        // since they are directions along the surface.
        //
        // A matrix with a negative determinant mirrors the mesh, and a
        // mirrored triangle appears clockwise: the winding is flipped back so
        // it stays counter-clockwise from outside, and the bitangent sign
        // flips with it (the frame cross(N, T) * w must keep matching the
        // transformed bitangent).
        void Apply_transform(Mesh& _mesh, const Matrix4& _world)
        {
            if (_world == Matrix4(1.0f))
                return;

            const MathLib::Matrix3 linear = MathLib::Mat4::To_matrix3(_world);
            const MathLib::Matrix3 normal_matrix = MathLib::Mat4::Normal_matrix(_world);
            const bool mirrored = MathLib::Mat4::Determinant(_world) < 0.0f;

            for (Vertex& v : _mesh.vertices)
            {
                v.position = MathLib::Mat4::Transform_point(_world, v.position);
                v.normal = MathLib::Vec3::Normalize(normal_matrix * v.normal);

                const Vec3 tangent = MathLib::Vec3::Normalize(linear * Vec3(v.tangent));
                v.tangent = MathLib::Vector4(tangent, mirrored ? -v.tangent.w : v.tangent.w);
            }

            if (mirrored)
            {
                for (size_t i = 0; i + 2 < _mesh.indices.size(); i += 3)
                    std::swap(_mesh.indices[i + 1], _mesh.indices[i + 2]);
            }
        }

        // =========================================================
        // File handling and diagnostics
        // =========================================================

        // glTF content MeshData has no place for. It is dropped on purpose,
        // but never silently: a model that loads as untextured geometry
        // should say why.
        void Report_ignored_content(const tinygltf::Model& _model, const std::string& _path,
            size_t _unreferenced_meshes)
        {
            std::string ignored;

            const auto add = [&](size_t count, const char* label)
                {
                    if (count == 0) return;
                    if (!ignored.empty()) ignored += ", ";
                    ignored += std::to_string(count) + " " + label;
                };

            add(_model.materials.size(), "material(s)");
            add(_model.textures.size(), "texture(s)");
            add(_model.images.size(), "image(s)");
            add(_model.animations.size(), "animation(s)");
            add(_model.skins.size(), "skin(s)");

            if (!ignored.empty())
                std::cout << "[Mesh_Loader] Warning: '" << _path << "' defines " << ignored
                << ", which are not imported (only geometry is). Assign materials and load textures separately.\n";

            std::set<std::string> unused_attributes;
            for (const tinygltf::Mesh& mesh : _model.meshes)
                for (const tinygltf::Primitive& primitive : mesh.primitives)
                    for (const auto& attribute : primitive.attributes)
                        if (!Is_supported_attribute(attribute.first))
                            unused_attributes.insert(attribute.first);

            if (!unused_attributes.empty())
            {
                std::string list;
                for (const std::string& name : unused_attributes)
                    list += (list.empty() ? "" : ", ") + name;

                std::cout << "[Mesh_Loader] Warning: '" << _path << "' has vertex attributes that are not imported: " << list << ".\n";
            }

            if (_unreferenced_meshes > 0)
                std::cout << "[Mesh_Loader] Warning: '" << _path << "' has " << _unreferenced_meshes
                << " mesh(es) that no node of the loaded scene uses; they are skipped.\n";
        }

        // The directory part of a path, including the trailing separator, in
        // the form tinygltf wants for resolving external buffers.
        std::string Directory_of(const std::string& _path)
        {
            const size_t separator = _path.find_last_of("/\\");
            return separator == std::string::npos ? std::string() : _path.substr(0, separator + 1);
        }

        // Reads and parses a .gltf or .glb. Which of the two it is comes from
        // the file's own first four bytes ("glTF" starts every binary glTF),
        // not from its name, so "Model.GLB" and extension-less files work.
        tinygltf::Model Read_model(const std::string& _path)
        {
            // tinygltf has no default image loader (see Tinygltf_Include.hpp);
            // textures are handled by Image_Loader.
            tinygltf::TinyGLTF loader;
            loader.SetImageLoader(Dummy_load_image, nullptr);

            std::vector<unsigned char> bytes;
            std::string                read_error;

            if (!tinygltf::ReadWholeFile(&bytes, &read_error, _path, nullptr))
                throw std::runtime_error("Mesh_Loader: failed to read '" + _path + "': " + read_error);

            if (bytes.empty())
                throw std::runtime_error("Mesh_Loader: '" + _path + "' is empty");

            // tinygltf takes the size as an unsigned int.
            if (bytes.size() > std::numeric_limits<unsigned int>::max())
                throw std::runtime_error("Mesh_Loader: '" + _path + "' is larger than 4 GiB");

            const unsigned int size = static_cast<unsigned int>(bytes.size());
            const std::string  base_dir = Directory_of(_path);
            const bool         is_glb = bytes.size() >= 4 && std::memcmp(bytes.data(), "glTF", 4) == 0;

            tinygltf::Model model;
            std::string     error;
            std::string     warning;

            const bool success = is_glb
                ? loader.LoadBinaryFromMemory(&model, &error, &warning, bytes.data(), size, base_dir)
                : loader.LoadASCIIFromString(&model, &error, &warning, reinterpret_cast<const char*>(bytes.data()), size, base_dir);

            if (!warning.empty())
                std::cout << "[Mesh_Loader] Warning: " << warning << "\n";

            if (!success)
                throw std::runtime_error("Mesh_Loader: failed to load '" + _path + "': " + error);

            return model;
        }

    } // anonymous namespace

    // =========================================================
    // Load
    // =========================================================

    std::vector<CoreTypes::MeshData> Load(const std::string& _path)
    {
        const tinygltf::Model model = Read_model(_path);

        const std::vector<Mesh_Instance> instances = Collect_instances(model);

        // How many instances use each mesh: a mesh is extracted once, and the
        // last instance takes the extracted data instead of copying it.
        std::vector<size_t> uses_left(model.meshes.size(), 0);
        for (const Mesh_Instance& instance : instances)
            ++uses_left[instance.mesh];

        size_t unreferenced = 0;
        for (size_t uses : uses_left)
            if (uses == 0) ++unreferenced;

        Report_ignored_content(model, _path, unreferenced);

        std::vector<std::vector<Mesh>> extracted(model.meshes.size());
        std::vector<char>              is_extracted(model.meshes.size(), 0);

        std::vector<Mesh> result;

        for (const Mesh_Instance& instance : instances)
        {
            // A zero scale hides an object on purpose; there is nothing to
            // place and no inverse to build its normals with.
            if (MathLib::Mat4::Is_singular(instance.world, MathLib::Constants::EPSILON))
            {
                std::cout << "[Mesh_Loader] Skipping an instance of mesh " << instance.mesh
                    << ": its node transform is singular.\n";
                --uses_left[instance.mesh];
                continue;
            }

            if (!is_extracted[instance.mesh])
            {
                for (const tinygltf::Primitive& primitive : model.meshes[instance.mesh].primitives)
                    Extract_primitive(model, primitive, extracted[instance.mesh]);

                is_extracted[instance.mesh] = 1;
            }

            const bool last_use = --uses_left[instance.mesh] == 0;

            for (Mesh& local : extracted[instance.mesh])
            {
                Mesh placed = last_use ? Mesh(std::move(local)) : Mesh(local);
                Apply_transform(placed, instance.world);
                result.push_back(std::move(placed));
            }
        }

        if (result.empty())
            throw std::runtime_error(
                "Mesh_Loader: no valid triangle primitives found in '" + _path + "'");

        std::cout << "[Mesh_Loader] Loaded " << result.size()
            << " primitive(s) from '" << _path << "'.\n";

        return result;
    }

} // namespace ResourceManager::Mesh_Loader
