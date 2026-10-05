
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

        if (Platform::Filesystem::Exists(file_path))
        {
            blob = Platform::Filesystem::Read_binary_file(file_path);

            if (!Is_blob_usable(blob))
            {
                std::cout << "[Pipeline_Cache] On-disk cache rejected "
                    "(different GPU or driver) — starting empty.\n";
                blob.clear();
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

        const bool written =
            Platform::Filesystem::Write_binary_file(file_path, blob);

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