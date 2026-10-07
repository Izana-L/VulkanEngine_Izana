
#include <Pipeline_Cache.hpp>
#include <Vulkan_Utils.hpp>
#include <Filesystem.hpp>

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace Renderer_System
{
    namespace
    {
        constexpr const char* CACHE_FILENAME = "pipeline_cache.bin";

        // The file on disk is not the bare blob of the driver: a header that
        // makes it checkable comes first. The blob of vkGetPipelineCacheData
        // starts with a header of its own (vendor, device, cache UUID), but
        // it says nothing about how long the blob is or whether its bytes
        // are intact, and a file cut short or damaged keeps a perfectly
        // valid driver header. Handing such a blob to the driver can crash
        // it, so the container records the size and a hash of the payload,
        // and both are checked before the driver sees a single byte.
        constexpr uint32_t FILE_MAGIC = 0x43505256;   // "VRPC" in a little-endian dump
        constexpr uint32_t FILE_VERSION = 1;

        struct File_Header
        {
            uint32_t magic;
            uint32_t version;
            uint64_t payload_size;
            uint64_t payload_hash;
        };

        static_assert(sizeof(File_Header) == 24, "File_Header is written to disk as it is: no padding");

        // 64-bit FNV-1a: detects damage, it is not meant to resist anyone.
        // One byte per step is slow in absolute terms (about a gigabyte per
        // second) and irrelevant here: the blob of a handful of pipelines
        // is megabytes at most, hashed once when it is loaded and once when
        // it is saved, against pipelines that take milliseconds each to
        // compile. A word-at-a-time variant would be faster and also
        // weaker (flips of the same bit in two consecutive words cancel),
        // which is a bad trade for an integrity check.
        uint64_t Hash_bytes(const uint8_t* _data, size_t _size)
        {
            uint64_t hash = 0xcbf29ce484222325ull;

            for (size_t i = 0; i < _size; ++i)
            {
                hash ^= _data[i];
                hash *= 0x100000001b3ull;
            }

            return hash;
        }

        // The bytes of the file for the driver blob _payload.
        std::vector<uint8_t> Wrap_payload(const std::vector<uint8_t>& _payload)
        {
            const File_Header header{ FILE_MAGIC, FILE_VERSION, _payload.size(), Hash_bytes(_payload.data(), _payload.size()) };

            std::vector<uint8_t> file(sizeof(header) + _payload.size());
            std::memcpy(file.data(), &header, sizeof(header));

            if (!_payload.empty())
                std::memcpy(file.data() + sizeof(header), _payload.data(), _payload.size());

            return file;
        }

        enum class File_Status
        {
            Ok,
            Too_short,        // not even a header
            Bad_magic,        // not a file of this class (a bare blob of an older version, or something else)
            Bad_version,      // a layout this code does not know
            Size_mismatch,    // cut short, or with something appended
            Hash_mismatch     // the right size, but the bytes changed
        };

        // Checks the container of _file and, when it is intact, copies the
        // driver blob it holds into _payload.
        File_Status Unwrap_payload(const std::vector<uint8_t>& _file, std::vector<uint8_t>& _payload)
        {
            if (_file.size() < sizeof(File_Header))
                return File_Status::Too_short;

            // memcpy, not a reinterpret_cast: the file has no alignment guarantee.
            File_Header header{};
            std::memcpy(&header, _file.data(), sizeof(header));

            if (header.magic != FILE_MAGIC)
                return File_Status::Bad_magic;

            if (header.version != FILE_VERSION)
                return File_Status::Bad_version;

            if (header.payload_size != _file.size() - sizeof(header))
                return File_Status::Size_mismatch;

            const uint8_t* const payload_begin = _file.data() + sizeof(header);

            if (header.payload_hash != Hash_bytes(payload_begin, _file.size() - sizeof(header)))
                return File_Status::Hash_mismatch;

            _payload.assign(payload_begin, _file.data() + _file.size());
            return File_Status::Ok;
        }

        const char* Describe(File_Status _status)
        {
            switch (_status)
            {
            case File_Status::Ok:            return "intact";
            case File_Status::Too_short:     return "too short to hold a header";
            case File_Status::Bad_magic:     return "not a pipeline cache file of this engine (or one of an older format)";
            case File_Status::Bad_version:   return "written by another version of the format";
            case File_Status::Size_mismatch: return "truncated or with extra bytes";
            case File_Status::Hash_mismatch: return "corrupted (checksum mismatch)";
            }

            return "unknown";
        }
    }

    // ---------- Constructor ----------
    Pipeline_Cache::Pipeline_Cache(const Vulkan_Device& _device)
        : device_handle(_device.Get_logical_device_handle()),
        physical_device(_device.Get_physical_device_handle()),
        cache(VK_NULL_HANDLE),
        file_path(Platform::Filesystem::Combine_path(
            Platform::Filesystem::Get_executable_directory(), CACHE_FILENAME))
    {
        std::vector<uint8_t> blob;

        // A missing file is the normal first run; a file that exists but
        // cannot be read is worth saying so, since the cache then starts
        // empty and is rebuilt (and overwritten) from scratch.
        if (Platform::Filesystem::Exists(file_path))
        {
            std::optional<std::vector<uint8_t>> on_disk = Platform::Filesystem::Read_binary_file(file_path);

            if (!on_disk)
            {
                std::cerr << "[Pipeline_Cache] Could not read " << file_path
                    << " — starting empty.\n";
            }
            else
            {
                // First the container: size and checksum, so a damaged file
                // never reaches the driver. Then the driver's own header.
                const File_Status status = Unwrap_payload(*on_disk, blob);

                if (status != File_Status::Ok)
                {
                    std::cout << "[Pipeline_Cache] On-disk cache rejected ("
                        << Describe(status) << ") — starting empty.\n";
                    blob.clear();
                }
                else if (!Is_blob_usable(blob))
                {
                    std::cout << "[Pipeline_Cache] On-disk cache rejected "
                        "(different GPU or driver) — starting empty.\n";
                    blob.clear();
                }
            }
        }

        VkPipelineCacheCreateInfo cache_info{};
        cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        cache_info.initialDataSize = blob.size();
        cache_info.pInitialData = blob.empty() ? nullptr : blob.data();

        VK_CHECK(vkCreatePipelineCache(device_handle, &cache_info, nullptr, &cache),
            "Pipeline_Cache: failed to create pipeline cache");

        std::cout << "[Pipeline_Cache] Ready (" << blob.size()
            << " bytes loaded from disk).\n";
    }

    // ---------- Destructor ----------
    Pipeline_Cache::~Pipeline_Cache()
    {
        if (cache == VK_NULL_HANDLE) return;

        Save();

        vkDestroyPipelineCache(device_handle, cache, nullptr);
        cache = VK_NULL_HANDLE;
    }

    // ---------- Save ----------
    bool Pipeline_Cache::Save() const
    {
        if (cache == VK_NULL_HANDLE) return false;

        // Save() runs from the destructor, so a failure is reported through
        // the log rather than an exception.
        size_t size = 0;
        const VkResult size_result = vkGetPipelineCacheData(device_handle, cache, &size, nullptr);

        if (size_result != VK_SUCCESS)
        {
            std::cerr << "[Pipeline_Cache] Could not query cache size: "
                << Vulkan_Utils::Vk_result_to_string(size_result) << "\n";
            return false;
        }

        if (size == 0) return false;

        std::vector<uint8_t> blob(size);

        // VK_INCOMPLETE is not a failure here: it means the driver wrote
        // fewer bytes than the first query reported, and updated `size`.
        const VkResult result =
            vkGetPipelineCacheData(device_handle, cache, &size, blob.data());

        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            std::cerr << "[Pipeline_Cache] Could not read cache data: "
                << Vulkan_Utils::Vk_result_to_string(result) << "\n";
            return false;
        }

        blob.resize(size);

        // Atomic: the file is replaced in one step once complete, so a crash
        // while saving leaves the previous cache instead of a truncated one.
        const bool written =
            Platform::Filesystem::Write_binary_file_atomic(file_path, Wrap_payload(blob));

        std::cout << "[Pipeline_Cache] "
            << (written ? "Saved " : "FAILED to save ")
            << blob.size() << " bytes to " << file_path << "\n";

        return written;
    }

    // ---------- Is_blob_usable ----------
    bool Pipeline_Cache::Is_blob_usable(const std::vector<uint8_t>& _blob) const
    {
        if (_blob.size() < sizeof(VkPipelineCacheHeaderVersionOne))
            return false;

        // memcpy, not a reinterpret_cast: the blob has no alignment guarantee.
        VkPipelineCacheHeaderVersionOne header{};
        std::memcpy(&header, _blob.data(), sizeof(header));

        if (header.headerSize > _blob.size())                             return false;
        if (header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) return false;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);

        if (header.vendorID != properties.vendorID) return false;
        if (header.deviceID != properties.deviceID) return false;

        return std::memcmp(header.pipelineCacheUUID,
            properties.pipelineCacheUUID,
            VK_UUID_SIZE) == 0;
    }

} // namespace Renderer_System