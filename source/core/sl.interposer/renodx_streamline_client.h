#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <Windows.h>

#include "source/core/sl.interposer/renodx_streamline_bridge.h"

namespace renodx::streamline_client {

struct ImageInfo {
    uint32_t width{};
    uint32_t height{};
    uint32_t format{};
    bool active{};
};

struct SwapchainInfo {
    uint32_t width{};
    uint32_t height{};
    uint32_t format{};
    std::vector<VkImage> images;
    std::chrono::steady_clock::time_point registration_ready_at{};
};

inline std::mutex mutex;
inline std::unordered_map<uint64_t, SwapchainInfo> swapchains;
inline std::unordered_map<uint64_t, ImageInfo> images;
inline bool initial_registration_complete{};
inline bool initial_registration_in_progress{};
inline PFN_vkGetDeviceProcAddr native_get_device_proc_addr{};
inline std::atomic<renodx::streamline_bridge::ManageVulkanClientImageV1>
    manage_client_image{};
inline std::atomic<
    renodx::streamline_bridge::SetVulkanDisplayReadyPQPresentV1>
    set_display_ready_pq_present{};
inline thread_local uint32_t outer_present_hook_depth{};

template<typename T>
uint64_t Handle(T value)
{
    if constexpr (std::is_pointer_v<T>)
    {
        return reinterpret_cast<uint64_t>(value);
    }
    else
    {
        return static_cast<uint64_t>(value);
    }
}

inline renodx::streamline_bridge::ManageVulkanClientImageV1 GetManager()
{
    auto manager = manage_client_image.load(std::memory_order_acquire);
    if (!manager)
    {
        if (const HMODULE addon =
                ::GetModuleHandleW(L"renodx-endfield-vk.addon64"))
        {
            manager = reinterpret_cast<
                renodx::streamline_bridge::ManageVulkanClientImageV1>(
                    ::GetProcAddress(
                        addon,
                        "RenoDX_Streamline_ManageVulkanClientImageV1"));
            if (manager)
            {
                manage_client_image.store(manager, std::memory_order_release);
            }
        }
    }
    return manager;
}

inline bool Manage(
    uint32_t operation,
    VkCommandBuffer command_buffer,
    VkImage image,
    const ImageInfo& info)
{
    const auto manager = GetManager();
    return manager && manager(
        renodx::streamline_bridge::kAbiVersion,
        operation,
        Handle(command_buffer),
        Handle(image),
        info.width,
        info.height,
        info.format) != 0u;
}

inline bool RegisterClientImages(
    const SwapchainInfo& swapchain_info,
    const std::vector<VkImage>& swapchain_images)
{
    bool registered_all = true;
    for (const VkImage image : swapchain_images)
    {
        const ImageInfo image_info{
            swapchain_info.width,
            swapchain_info.height,
            swapchain_info.format,
            true,
        };
        if (!Manage(
                renodx::streamline_bridge::kClientImageOperationRegister,
                nullptr,
                image,
                image_info))
        {
            SL_LOG_ERROR("[RenoDX][client-fp16] Failed to register client image 0x%llx",
                static_cast<unsigned long long>(Handle(image)));
            registered_all = false;
            continue;
        }
        static std::once_flag logged;
        std::call_once(logged, [] {
            SL_LOG_INFO("[RenoDX][client-fp16] Registered SDR/HDR10 Streamline client images with FP16 clones");
        });
        std::lock_guard<std::mutex> lock(mutex);
        images[Handle(image)] = image_info;
    }
    return registered_all;
}

inline bool SetDisplayReadyPQPresent(bool active)
{
    auto setter = set_display_ready_pq_present.load(std::memory_order_acquire);
    if (!setter)
    {
        if (const HMODULE addon =
                ::GetModuleHandleW(L"renodx-endfield-vk.addon64"))
        {
            setter = reinterpret_cast<
                renodx::streamline_bridge::SetVulkanDisplayReadyPQPresentV1>(
                    ::GetProcAddress(
                        addon,
                        "RenoDX_Streamline_SetVulkanDisplayReadyPQPresentV1"));
            if (setter)
            {
                set_display_ready_pq_present.store(
                    setter,
                    std::memory_order_release);
            }
        }
    }
    return setter && setter(
        renodx::streamline_bridge::kAbiVersion,
        active ? 1u : 0u) != 0u;
}

inline void BeginOuterPresentHook()
{
    ++outer_present_hook_depth;
}

inline void EndOuterPresentHook()
{
    if (outer_present_hook_depth != 0u)
    {
        --outer_present_hook_depth;
    }
}

inline void OnCreateSwapchain(
    VkSwapchainKHR swapchain,
    const VkSwapchainCreateInfoKHR& create_info)
{
    if (!swapchain
        || (create_info.imageFormat != VK_FORMAT_R8G8B8A8_UNORM
            && create_info.imageFormat
                != VK_FORMAT_A2B10G10R10_UNORM_PACK32))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    swapchains[Handle(swapchain)] = {
        create_info.imageExtent.width,
        create_info.imageExtent.height,
        static_cast<uint32_t>(create_info.imageFormat),
        {},
    };
}

inline void OnDestroySwapchain(VkSwapchainKHR swapchain)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = swapchains.find(Handle(swapchain));
    if (found == swapchains.end())
    {
        return;
    }
    for (const VkImage image : found->second.images)
    {
        images.erase(Handle(image));
    }
    swapchains.erase(found);
}

inline void OnGetSwapchainImages(
    VkSwapchainKHR swapchain,
    uint32_t image_count,
    const VkImage* swapchain_images)
{
    if (!swapchain_images || image_count == 0u)
    {
        return;
    }

    SwapchainInfo info{};
    bool defer_registration{};
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = swapchains.find(Handle(swapchain));
        if (found == swapchains.end())
        {
            return;
        }
        const std::vector<VkImage> queried_images(
            swapchain_images, swapchain_images + image_count);
        if (found->second.images != queried_images)
        {
            found->second.images = queried_images;
            if (!initial_registration_complete)
            {
                found->second.registration_ready_at =
                    std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(500);
            }
        }
        info = found->second;
        defer_registration = !initial_registration_complete;
    }

    if (!defer_registration)
    {
        RegisterClientImages(info, info.images);
    }
}

inline void OnAcquire(VkSwapchainKHR swapchain, uint32_t image_index)
{
    VkImage image{};
    ImageInfo info{};
    SwapchainInfo pending_swapchain{};
    std::vector<VkImage> pending_images;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = swapchains.find(Handle(swapchain));
        if (found == swapchains.end() || image_index >= found->second.images.size())
        {
            return;
        }
        image = found->second.images[image_index];
        const auto image_found = images.find(Handle(image));
        if (image_found == images.end())
        {
            if (initial_registration_complete
                || initial_registration_in_progress
                || std::chrono::steady_clock::now()
                    < found->second.registration_ready_at)
            {
                return;
            }
            for (const VkImage candidate : found->second.images)
            {
                if (images.find(Handle(candidate)) == images.end())
                {
                    pending_images.push_back(candidate);
                }
            }
            pending_swapchain = found->second;
            initial_registration_in_progress = true;
        }
        else
        {
            info = image_found->second;
        }
    }

    if (!pending_images.empty())
    {
        const bool registered =
            RegisterClientImages(pending_swapchain, pending_images);
        std::lock_guard<std::mutex> lock(mutex);
        initial_registration_in_progress = false;
        initial_registration_complete = registered;
        if (!registered)
        {
            return;
        }
        const auto image_found = images.find(Handle(image));
        if (image_found == images.end())
        {
            return;
        }
        info = image_found->second;
        static std::once_flag logged;
        std::call_once(logged, [] {
            SL_LOG_INFO("[RenoDX][client-fp16] Applied the 500 ms initialization delay to the first Streamline client-image set only");
        });
    }

    if (!Manage(
            renodx::streamline_bridge::kClientImageOperationActivate,
            nullptr,
            image,
            info))
    {
        SL_LOG_ERROR("[RenoDX][client-fp16] Failed to activate client image 0x%llx",
            static_cast<unsigned long long>(Handle(image)));
        return;
    }
    static std::once_flag logged;
    std::call_once(logged, [] {
        SL_LOG_INFO("[RenoDX][client-fp16] Activated an FP16 clone for the acquired client image");
    });
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = images.find(Handle(image));
    if (found != images.end())
    {
        found->second.active = true;
    }
}

inline bool Convert(VkCommandBuffer command_buffer, VkImage image)
{
    ImageInfo info{};
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = images.find(Handle(image));
        if (found == images.end() || !found->second.active)
        {
            return false;
        }
        info = found->second;
        found->second.active = false;
    }

    if (Manage(
            renodx::streamline_bridge::kClientImageOperationConvert,
            command_buffer,
            image,
            info))
    {
        static std::once_flag logged;
        std::call_once(logged, [] {
            SL_LOG_INFO("[RenoDX][client-fp16] Preserved the native frame in FP16 and encoded it for DLSS-G at the read barrier");
        });
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex);
    const auto found = images.find(Handle(image));
    if (found != images.end())
    {
        found->second.active = true;
    }
    SL_LOG_ERROR("[RenoDX][client-fp16] Failed to convert client image 0x%llx",
        static_cast<unsigned long long>(Handle(image)));
    return false;
}

inline void ConvertDisplay(VkCommandBuffer command_buffer, VkImage image)
{
    const auto manager = GetManager();
    if (!manager || !manager(
            renodx::streamline_bridge::kAbiVersion,
            renodx::streamline_bridge::kClientImageOperationConvertDisplay,
            Handle(command_buffer),
            Handle(image),
            0u,
            0u,
            0u))
    {
        return;
    }
    static std::once_flag logged;
    std::call_once(logged, [] {
        SL_LOG_INFO("[RenoDX][display-output] Copied the generated frame into the physical swapchain image on DLSS-G's command buffer");
    });
}

inline void VKAPI_CALL CmdPipelineBarrier(
    VkCommandBuffer command_buffer,
    VkPipelineStageFlags source_stage,
    VkPipelineStageFlags destination_stage,
    VkDependencyFlags dependency_flags,
    uint32_t memory_barrier_count,
    const VkMemoryBarrier* memory_barriers,
    uint32_t buffer_memory_barrier_count,
    const VkBufferMemoryBarrier* buffer_memory_barriers,
    uint32_t image_memory_barrier_count,
    const VkImageMemoryBarrier* image_memory_barriers)
{
    std::vector<VkImageMemoryBarrier> rewritten;
    for (uint32_t i = 0u; image_memory_barriers && i < image_memory_barrier_count; ++i)
    {
        const auto& barrier = image_memory_barriers[i];
        if (barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
            && barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL
            && Convert(command_buffer, barrier.image))
        {
            if (rewritten.empty())
            {
                rewritten.assign(
                    image_memory_barriers,
                    image_memory_barriers + image_memory_barrier_count);
            }
            rewritten[i].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
    }
    s_ddt.CmdPipelineBarrier(
        command_buffer,
        source_stage,
        destination_stage,
        dependency_flags,
        memory_barrier_count,
        memory_barriers,
        buffer_memory_barrier_count,
        buffer_memory_barriers,
        image_memory_barrier_count,
        rewritten.empty() ? image_memory_barriers : rewritten.data());

    for (uint32_t i = 0u;
        image_memory_barriers && i < image_memory_barrier_count;
        ++i)
    {
        if (image_memory_barriers[i].newLayout
            == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        {
            ConvertDisplay(command_buffer, image_memory_barriers[i].image);
        }
    }
}

inline void VKAPI_CALL CmdPipelineBarrier2(
    VkCommandBuffer command_buffer,
    const VkDependencyInfo* dependency_info)
{
    std::vector<VkImageMemoryBarrier2> rewritten;
    VkDependencyInfo rewritten_dependency{};
    if (dependency_info)
    {
        for (uint32_t i = 0u;
            dependency_info->pImageMemoryBarriers
                && i < dependency_info->imageMemoryBarrierCount;
            ++i)
        {
            const auto& barrier = dependency_info->pImageMemoryBarriers[i];
            if (barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                && barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL
                && Convert(command_buffer, barrier.image))
            {
                if (rewritten.empty())
                {
                    rewritten.assign(
                        dependency_info->pImageMemoryBarriers,
                        dependency_info->pImageMemoryBarriers
                            + dependency_info->imageMemoryBarrierCount);
                }
                rewritten[i].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            }
        }
    }
    if (!rewritten.empty())
    {
        rewritten_dependency = *dependency_info;
        rewritten_dependency.pImageMemoryBarriers = rewritten.data();
    }
    s_ddt.CmdPipelineBarrier2(
        command_buffer,
        rewritten.empty() ? dependency_info : &rewritten_dependency);

    if (dependency_info)
    {
        for (uint32_t i = 0u;
            dependency_info->pImageMemoryBarriers
                && i < dependency_info->imageMemoryBarrierCount;
            ++i)
        {
            const auto& barrier = dependency_info->pImageMemoryBarriers[i];
            if (barrier.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            {
                ConvertDisplay(command_buffer, barrier.image);
            }
        }
    }
}


inline VkResult VKAPI_CALL QueuePresent(
    VkQueue queue,
    const VkPresentInfoKHR* present_info)
{
    const bool asynchronous = outer_present_hook_depth == 0u;
    const bool marked = asynchronous && SetDisplayReadyPQPresent(true);
    if (asynchronous && !marked)
    {
        static std::once_flag logged;
        std::call_once(logged, [] {
            SL_LOG_ERROR("[RenoDX][client-fp16] Failed to mark DLSS-G display-ready present");
        });
    }

    const VkResult result = s_ddt.QueuePresentKHR(queue, present_info);

    if (marked && !SetDisplayReadyPQPresent(false))
    {
        static std::once_flag logged;
        std::call_once(logged, [] {
            SL_LOG_ERROR("[RenoDX][client-fp16] Failed to clear DLSS-G display-ready present");
        });
    }
    return result;
}

inline PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(
    VkDevice device,
    const char* name)
{
    if (name && strcmp(name, "vkQueuePresentKHR") == 0)
    {
        return reinterpret_cast<PFN_vkVoidFunction>(QueuePresent);
    }
    if (name && strcmp(name, "vkCmdPipelineBarrier") == 0)
    {
        static std::once_flag logged;
        std::call_once(logged, [] {
            SL_LOG_INFO("[RenoDX][client-fp16] DLSS-G requested the client-image barrier function");
        });
        return reinterpret_cast<PFN_vkVoidFunction>(CmdPipelineBarrier);
    }
    if (name && (strcmp(name, "vkCmdPipelineBarrier2") == 0
        || strcmp(name, "vkCmdPipelineBarrier2KHR") == 0))
    {
        static std::once_flag logged;
        std::call_once(logged, [] {
            SL_LOG_INFO("[RenoDX][client-fp16] DLSS-G requested the client-image barrier2 function");
        });
        return reinterpret_cast<PFN_vkVoidFunction>(CmdPipelineBarrier2);
    }
    return native_get_device_proc_addr
        ? native_get_device_proc_addr(device, name)
        : nullptr;
}

inline void Install(PFN_vkGetDeviceProcAddr get_device_proc_addr)
{
    native_get_device_proc_addr = get_device_proc_addr;
}

} // namespace renodx::streamline_client
