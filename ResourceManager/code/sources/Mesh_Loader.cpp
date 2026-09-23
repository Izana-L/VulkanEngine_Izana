#include <Mesh_Loader.hpp>

#include <tiny_gltf.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ResourceManager::Mesh_Loader
{

    // =========================================================
    // Internal helpers
    // =========================================================

    namespace
    {
        // Dummy image loader registered when TINYGLTF_NO_STB_IMAGE is defined.
        // tinygltf requires a custom LoadImageData callback in that case.
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

        // Where the elements of an accessor live, with the layout resolved:
        // a base pointer, the stride between elements (which honors
        // bufferView.byteStride for interleaved vertex data) and the size
        // of one component. Every field has been bounds-checked against the
        // buffer and the buffer view before the struct is handed out.
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
            const size_t first_byte = view.byteOffset + _accessor.byteOffset;
            const size_t last_byte_end = (layout.count == 0)
                ? first_byte
                : first_byte + (layout.count - 1) * layout.stride + element_size;

            // Bounds against the view (when it declares a length) and
            // against the buffer that backs it.
            if (view.byteLength > 0 && last_byte_end > view.byteOffset + view.byteLength)
                throw std::runtime_error("Mesh_Loader: " + what + " reads past the end of its buffer view");

            if (last_byte_end > buffer.data.size())
                throw std::runtime_error("Mesh_Loader: " + what + " reads past the end of its buffer");

            layout.base = buffer.data.data() + first_byte;

            return layout;
        }

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
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            {
                uint32_t value;
                std::memcpy(&value, _ptr, sizeof(value));
                return _normalized ? static_cast<float>(static_cast<double>(value) / 4294967295.0)
                    : static_cast<float>(value);
            }
            default:
                throw std::runtime_error("Mesh_Loader: unsupported attribute component type " +
                    std::to_string(_component_type));
            }
        }

        // Reads a vertex attribute as `_wanted_components` floats per
        // element. Elements with fewer components (a VEC3 color) are padded
        // with _pad_value; elements with more are truncated.
        std::vector<float> Read_float_attribute(const tinygltf::Model& _model,
            const tinygltf::Accessor& _accessor,
            int                       _wanted_components,
            float                     _pad_value,
            const char* _what)
        {
            const Accessor_Layout layout = Resolve_layout(_model, _accessor, _what);
            const size_t wanted = static_cast<size_t>(_wanted_components);

            std::vector<float> out(layout.count * wanted, _pad_value);

            if (layout.base == nullptr)
            {
                // No buffer view: zeros, with the padding rule for the
                // components the accessor does not even declare.
                for (size_t i = 0; i < layout.count; ++i)
                    for (size_t c = 0; c < std::min(wanted, static_cast<size_t>(layout.components)); ++c)
                        out[i * wanted + c] = 0.0f;
                return out;
            }

            const size_t copied = std::min(wanted, static_cast<size_t>(layout.components));

            for (size_t i = 0; i < layout.count; ++i)
            {
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

        // Reads the index accessor, widening every index to uint32_t and
        // validating that each one addresses an existing vertex.
        std::vector<uint32_t> Read_indices(const tinygltf::Model& _model,
            const tinygltf::Accessor& _accessor,
            uint32_t                  _vertex_count,
            CoreTypes::Index_Type& _out_index_type)
        {
            const Accessor_Layout layout = Resolve_layout(_model, _accessor, "index buffer");

            if (layout.components != 1)
                throw std::runtime_error("Mesh_Loader: index accessor must be SCALAR");

            if (layout.base == nullptr)
                throw std::runtime_error("Mesh_Loader: index accessor has no buffer view");

            if (layout.count % 3 != 0)
                throw std::runtime_error("Mesh_Loader: index count " + std::to_string(layout.count) +
                    " is not a multiple of 3 for a triangle primitive");

            switch (layout.component_type)
            {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                _out_index_type = CoreTypes::Index_Type::UINT16;
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                _out_index_type = CoreTypes::Index_Type::UINT32;
                break;
            default:
                throw std::runtime_error("Mesh_Loader: unsupported index component type: " +
                    std::to_string(layout.component_type));
            }

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

        void Extract_primitive(const tinygltf::Model& _model,
            const tinygltf::Primitive& _primitive,
            std::vector<CoreTypes::MeshData>& _out)
        {
            if (_primitive.mode != TINYGLTF_MODE_TRIANGLES)
            {
                std::cout << "[Mesh_Loader] Skipping non-triangle primitive.\n";
                return;
            }

            // POSITION (required) ---------------------------------------
            const tinygltf::Accessor* pos_accessor = Find_attribute(_model, _primitive, "POSITION");
            if (!pos_accessor)
                throw std::runtime_error("Mesh_Loader: primitive has no POSITION attribute");

            const uint32_t vertex_count = static_cast<uint32_t>(pos_accessor->count);
            if (vertex_count == 0)
                throw std::runtime_error("Mesh_Loader: primitive has zero vertices");

            const std::vector<float> positions = Read_float_attribute(_model, *pos_accessor, 3, 0.0f, "POSITION");

            // Optional attributes ---------------------------------------
            // Each one must have exactly one element per vertex; a mismatch
            // means the file is malformed and the read would run past the
            // shorter array.
            const auto read_optional = [&](const char* name, int components, float pad) -> std::vector<float>
                {
                    const tinygltf::Accessor* accessor = Find_attribute(_model, _primitive, name);
                    if (!accessor) return {};

                    if (accessor->count != pos_accessor->count)
                    {
                        throw std::runtime_error(std::string("Mesh_Loader: attribute ") + name + " has " +
                            std::to_string(accessor->count) + " elements but POSITION has " +
                            std::to_string(pos_accessor->count));
                    }

                    return Read_float_attribute(_model, *accessor, components, pad, name);
                };

            const std::vector<float> normals = read_optional("NORMAL", 3, 0.0f);
            const std::vector<float> uvs = read_optional("TEXCOORD_0", 2, 0.0f);
            const std::vector<float> tangents = read_optional("TANGENT", 4, 1.0f);
            const std::vector<float> colors = read_optional("COLOR_0", 4, 1.0f);   // VEC3 colors get alpha = 1

            // Build vertices --------------------------------------------
            CoreTypes::MeshData mesh_data;
            mesh_data.vertices.resize(vertex_count);

            for (uint32_t i = 0; i < vertex_count; ++i)
            {
                CoreTypes::Vertex_Static_Mesh_CPU& v = mesh_data.vertices[i];

                v.position = { positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2] };

                v.normal = !normals.empty()
                    ? MathLib::Vector3{ normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2] }
                    : MathLib::Vector3{ 0.0f, 1.0f, 0.0f };

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
            if (_primitive.indices < 0)
                throw std::runtime_error("Mesh_Loader: primitive has no index buffer");

            if (static_cast<size_t>(_primitive.indices) >= _model.accessors.size())
                throw std::runtime_error("Mesh_Loader: index accessor out of range");

            mesh_data.indices = Read_indices(_model, _model.accessors[static_cast<size_t>(_primitive.indices)],
                vertex_count, mesh_data.index_type);

            _out.push_back(std::move(mesh_data));
        }

    } // anonymous namespace

    // =========================================================
    // Load
    // =========================================================

    std::vector<CoreTypes::MeshData> Load(const std::string& _path)
    {
        tinygltf::TinyGLTF loader;
        tinygltf::Model    model;
        std::string        error;
        std::string        warning;

        // Register dummy image loader: required when TINYGLTF_NO_STB_IMAGE
        // is defined. Textures are handled by Image_Loader.
        loader.SetImageLoader(Dummy_load_image, nullptr);

        const bool is_glb = _path.size() >= 4 &&
            _path.substr(_path.size() - 4) == ".glb";

        bool success = is_glb
            ? loader.LoadBinaryFromFile(&model, &error, &warning, _path)
            : loader.LoadASCIIFromFile(&model, &error, &warning, _path);

        if (!warning.empty())
            std::cout << "[Mesh_Loader] Warning: " << warning << "\n";

        if (!success)
            throw std::runtime_error(
                "Mesh_Loader: failed to load '" + _path + "': " + error);

        std::vector<CoreTypes::MeshData> result;

        for (const tinygltf::Mesh& mesh : model.meshes)
            for (const tinygltf::Primitive& primitive : mesh.primitives)
                Extract_primitive(model, primitive, result);

        if (result.empty())
            throw std::runtime_error(
                "Mesh_Loader: no valid triangle primitives found in '" + _path + "'");

        std::cout << "[Mesh_Loader] Loaded " << result.size()
            << " primitive(s) from '" << _path << "'.\n";

        return result;
    }

} // namespace ResourceManager::Mesh_Loader
