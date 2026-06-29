#include <Mesh_Loader.hpp>

#include <tiny_gltf.h>

#include <stdexcept>
#include <iostream>
#include <string>

namespace ResourceManager::Mesh_Loader
{

    // =========================================================
    // Internal helpers
    // =========================================================

    namespace
    {
        // Dummy image loader registered when TINYGLTF_NO_STB_IMAGE is defined.
        // tinygltf requires a custom LoadImageData callback in that case.
        // We don't load textures in Fase 1 — return true to satisfy the API.
        bool Dummy_load_image(tinygltf::Image* _image,
            const int            _image_idx,
            std::string* _err,
            std::string* _warn,
            int                  _req_width,
            int                  _req_height,
            const unsigned char* _bytes,
            int                  _size,
            void* _user_data)
        {
            // No-op: image loading handled by our own Image_Loader in Fase 2.
            return true;
        }

        // Returns a typed pointer into a tinygltf buffer view's raw data.
        template< typename T >
        const T* Get_buffer_data(const tinygltf::Model& _model,
            const tinygltf::Accessor& _accessor)
        {
            const tinygltf::BufferView& view = _model.bufferViews[_accessor.bufferView];
            const tinygltf::Buffer& buffer = _model.buffers[view.buffer];

            return reinterpret_cast<const T*>(
                buffer.data.data() + view.byteOffset + _accessor.byteOffset);
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

            // ── POSITION (required) ───────────────────────────────
            auto pos_it = _primitive.attributes.find("POSITION");
            if (pos_it == _primitive.attributes.end())
                throw std::runtime_error("Mesh_Loader: primitive has no POSITION attribute");

            const tinygltf::Accessor& pos_accessor = _model.accessors[pos_it->second];
            const uint32_t vertex_count = static_cast<uint32_t>(pos_accessor.count);
            const float* positions = Get_buffer_data<float>(_model, pos_accessor);

            // ── Optional attributes ───────────────────────────────
            auto normal_it = _primitive.attributes.find("NORMAL");
            auto uv_it = _primitive.attributes.find("TEXCOORD_0");
            auto tangent_it = _primitive.attributes.find("TANGENT");
            auto color_it = _primitive.attributes.find("COLOR_0");

            const float* normals = nullptr;
            const float* uvs = nullptr;
            const float* tangents = nullptr;
            const float* colors = nullptr;

            if (normal_it != _primitive.attributes.end())
                normals = Get_buffer_data<float>(_model, _model.accessors[normal_it->second]);
            if (uv_it != _primitive.attributes.end())
                uvs = Get_buffer_data<float>(_model, _model.accessors[uv_it->second]);
            if (tangent_it != _primitive.attributes.end())
                tangents = Get_buffer_data<float>(_model, _model.accessors[tangent_it->second]);
            if (color_it != _primitive.attributes.end())
                colors = Get_buffer_data<float>(_model, _model.accessors[color_it->second]);

            // ── Build vertices ────────────────────────────────────
            CoreTypes::MeshData mesh_data;
            mesh_data.vertices.resize(vertex_count);

            for (uint32_t i = 0; i < vertex_count; ++i)
            {
                CoreTypes::Vertex_Static_Mesh& v = mesh_data.vertices[i];

                v.position = { positions[i * 3 + 0],
                               positions[i * 3 + 1],
                               positions[i * 3 + 2] };

                v.normal = normals
                    ? MathLib::Vector3{ normals[i * 3 + 0],
                                        normals[i * 3 + 1],
                                        normals[i * 3 + 2] }
                : MathLib::Vector3{ 0.0f, 1.0f, 0.0f };

                v.uv = uvs
                    ? MathLib::Vector2{ uvs[i * 2 + 0], uvs[i * 2 + 1] }
                : MathLib::Vector2{ 0.0f, 0.0f };

                v.tangent = tangents
                    ? MathLib::Vector4{ tangents[i * 4 + 0],
                                        tangents[i * 4 + 1],
                                        tangents[i * 4 + 2],
                                        tangents[i * 4 + 3] }
                : MathLib::Vector4{ 1.0f, 0.0f, 0.0f, 1.0f };

                v.color = colors
                    ? MathLib::Vector4{ colors[i * 4 + 0],
                                        colors[i * 4 + 1],
                                        colors[i * 4 + 2],
                                        colors[i * 4 + 3] }
                : MathLib::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
            }

            // ── Indices ───────────────────────────────────────────
            if (_primitive.indices < 0)
                throw std::runtime_error("Mesh_Loader: primitive has no index buffer");

            const tinygltf::Accessor& idx_accessor = _model.accessors[_primitive.indices];
            const uint32_t index_count = static_cast<uint32_t>(idx_accessor.count);
            mesh_data.indices.resize(index_count);

            switch (idx_accessor.componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            {
                mesh_data.index_type = CoreTypes::Index_Type::UINT16;
                const uint16_t* src = Get_buffer_data<uint16_t>(_model, idx_accessor);
                for (uint32_t i = 0; i < index_count; ++i)
                    mesh_data.indices[i] = static_cast<uint32_t>(src[i]);
                break;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            {
                mesh_data.index_type = CoreTypes::Index_Type::UINT32;
                const uint32_t* src = Get_buffer_data<uint32_t>(_model, idx_accessor);
                for (uint32_t i = 0; i < index_count; ++i)
                    mesh_data.indices[i] = src[i];
                break;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            {
                mesh_data.index_type = CoreTypes::Index_Type::UINT16;
                const uint8_t* src = Get_buffer_data<uint8_t>(_model, idx_accessor);
                for (uint32_t i = 0; i < index_count; ++i)
                    mesh_data.indices[i] = static_cast<uint32_t>(src[i]);
                break;
            }
            default:
                throw std::runtime_error(
                    "Mesh_Loader: unsupported index component type: " +
                    std::to_string(idx_accessor.componentType));
            }

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

        // Register dummy image loader — required when TINYGLTF_NO_STB_IMAGE
        // is defined. We handle textures ourselves in Fase 2.
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