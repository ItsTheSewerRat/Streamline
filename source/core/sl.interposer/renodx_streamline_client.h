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
#include "source/core/sl.interposer/renodx_streamline_diagnostics.h"

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
};

inline std::mutex mutex;
inline std::unordered_map<uint64_t, SwapchainInfo> swapchains;
inline std::unordered_map<uint64_t, ImageInfo> images;
inline bool initial_registration_complete{};
inline bool registration_in_progress{};
inline std::chrono::steady_clock::time_point initial_registration_ready_at{};
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
        SL_LOG_INFO(
            "[RenoDX][diag-v1] #%llu client.register image=0x%llx size=%ux%u format=%u",
            static_cast<unsigned long long>(
                streamline_diagnostics::NextSequence()),
            static_cast<unsigned long long>(Handle(image)),
            image_info.width,
            image_info.height,
            image_info.format);
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
    streamline_diagnostics::ArmDetailedTrace();
    SL_LOG_INFO(
        "[RenoDX][diag-v1] #%llu client.swapchain.create swapchain=0x%llx size=%ux%u format=%u",
        static_cast<unsigned long long>(
            streamline_diagnostics::NextSequence()),
        static_cast<unsigned long long>(Handle(swapchain)),
        create_info.imageExtent.width,
        create_info.imageExtent.height,
        static_cast<uint32_t>(create_info.imageFormat));
}

inline void OnDestroySwapchain(VkSwapchainKHR swapchain)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = swapchains.find(Handle(swapchain));
    if (found == swapchains.end())
    {
        SL_LOG_INFO(
            "[RenoDX][diag-v1] #%llu client.swapchain.destroy swapchain=0x%llx tracked=0",
            static_cast<unsigned long long>(
                streamline_diagnostics::NextSequence()),
            static_cast<unsigned long long>(Handle(swapchain)));
        return;
    }
    const size_t image_count = found->second.images.size();
    for (const VkImage image : found->second.images)
    {
        images.erase(Handle(image));
    }
    swapchains.erase(found);
    streamline_diagnostics::ArmDetailedTrace();
    SL_LOG_INFO(
        "[RenoDX][diag-v1] #%llu client.swapchain.destroy swapchain=0x%llx tracked=1 images=%llu",
        static_cast<unsigned long long>(
            streamline_diagnostics::NextSequence()),
        static_cast<unsigned long long>(Handle(swapchain)),
        static_cast<unsigned long long>(image_count));
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

    SL_LOG_INFO(
        "[RenoDX][diag-v1] #%llu client.swapchain.images swapchain=0x%llx count=%u first=0x%llx",
        static_cast<unsigned long long>(
            streamline_diagnostics::NextSequence()),
        static_cast<unsigned long long>(Handle(swapchain)),
        image_count,
        static_cast<unsigned long long>(Handle(swapchain_images[0])));

    SwapchainInfo info{};
    std::vector<VkImage> pending_images;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = swapchains.find(Handle(swapchain));
        if (found == swapchains.end())
        {
            return;
        }
        const std::vector<VkImage> queried_images(
            swapchain_images, swapchain_images + image_count);
        found->second.images = queried_images;
        if (initial_registration_ready_at
            == std::chrono::steady_clock::time_point{})
        {
            initial_registration_ready_at =
                std::chrono::steady_clock::now()
                + std::chrono::milliseconds(500);
        }
        info = found->second;
        if (initial_registration_complete && !registration_in_progress)
        {
            for (const VkImage image : found->second.images)
            {
                if (images.find(Handle(image)) == images.end())
                {
                    pending_images.push_back(image);
                }
            }
            registration_in_progress = !pending_images.empty();
        }
    }

    if (!pending_images.empty())
    {
        RegisterClientImages(info, pending_images);
        std::lock_guard<std::mutex> lock(mutex);
        registration_in_progress = false;
    }
}

inline void OnAcquire(VkSwapchainKHR swapchain, uint32_t image_index)
{
    const bool trace = streamline_diagnostics::TakeDetailedTrace();
    const uint64_t event = trace
        ? streamline_diagnostics::NextSequence() : 0u;
    const auto start = streamline_diagnostics::Clock::now();
    if (trace)
    {
        SL_LOG_INFO(
            "[RenoDX][diag-v1] #%llu client.acquire.begin swapchain=0x%llx index=%u",
            static_cast<unsigned long long>(event),
            static_cast<unsigned long long>(Handle(swapchain)),
            image_index);
    }
    VkImage image{};
    ImageInfo info{};
    SwapchainInfo pending_swapchain{};
    std::vector<VkImage> pending_images;
    bool completes_initial_registration{};
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = swapchains.find(Handle(swapchain));
        if (found == swapchains.end() || image_index >= found->second.images.size())
        {
            if (trace)
            {
                SL_LOG_INFO(
                    "[RenoDX][diag-v1] #%llu client.acquire.end tracked=0 durationUs=%llu",
                    static_cast<unsigned long long>(event),
                    static_cast<unsigned long long>(
                        streamline_diagnostics::ElapsedMicros(start)));
            }
            return;
        }
        image = found->second.images[image_index];
        const auto image_found = images.find(Handle(image));
        if (image_found == images.end())
        {
            if (registration_in_progress)
            {
                if (trace)
                {
                    SL_LOG_INFO(
                        "[RenoDX][diag-v1] #%llu client.acquire.end registrationInProgress=1 image=0x%llx durationUs=%llu",
                        static_cast<unsigned long long>(event),
                        static_cast<unsigned long long>(Handle(image)),
                        static_cast<unsigned long long>(
                            streamline_diagnostics::ElapsedMicros(start)));
                }
                return;
            }
            if (!initial_registration_complete)
            {
                if (initial_registration_ready_at
                    == std::chrono::steady_clock::time_point{})
                {
                    initial_registration_ready_at =
                        std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(500);
                }
                if (std::chrono::steady_clock::now()
                    < initial_registration_ready_at)
                {
                    static std::once_flag delay_logged;
                    std::call_once(delay_logged, [] {
                        SL_LOG_INFO("[RenoDX][diag-v1] Initial client-image registration guard started for 500 ms");
                    });
                    return;
                }
                completes_initial_registration = true;
            }
            for (const VkImage candidate : found->second.images)
            {
                if (images.find(Handle(candidate)) == images.end())
                {
                    pending_images.push_back(candidate);
                }
            }
            pending_swapchain = found->second;
            registration_in_progress = true;
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
        registration_in_progress = false;
        if (completes_initial_registration)
        {
            initial_registration_complete = registered;
        }
        if (!registered)
        {
            SL_LOG_ERROR(
                "[RenoDX][diag-v1] #%llu client.acquire.end registrationFailed=1 image=0x%llx",
                static_cast<unsigned long long>(
                    streamline_diagnostics::NextSequence()),
                static_cast<unsigned long long>(Handle(image)));
            return;
        }
        const auto image_found = images.find(Handle(image));
        if (image_found == images.end())
        {
            SL_LOG_ERROR(
                "[RenoDX][diag-v1] #%llu client.acquire.end registeredImageMissing=1 image=0x%llx",
                static_cast<unsigned long long>(
                    streamline_diagnostics::NextSequence()),
                static_cast<unsigned long long>(Handle(image)));
            return;
        }
        info = image_found->second;
        if (completes_initial_registration)
        {
            static std::once_flag logged;
            std::call_once(logged, [] {
                SL_LOG_INFO("[RenoDX][client-fp16] Delayed only the initial Streamline client-image registration by 500 ms");
            });
        }
    }

    const uint64_t activation = streamline_diagnostics::activation_calls.fetch_add(
        1u, std::memory_order_relaxed) + 1u;
    const bool activated = Manage(
            renodx::streamline_bridge::kClientImageOperationActivate,
            nullptr,
            image,
            info);
    const uint64_t duration = streamline_diagnostics::ElapsedMicros(start);
    if (!activated)
    {
        SL_LOG_ERROR("[RenoDX][client-fp16] Failed to activate client image 0x%llx",
            static_cast<unsigned long long>(Handle(image)));
        SL_LOG_ERROR(
            "[RenoDX][diag-v1] #%llu client.acquire.end activation=%llu activated=0 image=0x%llx durationUs=%llu",
            static_cast<unsigned long long>(
                streamline_diagnostics::NextSequence()),
            static_cast<unsigned long long>(activation),
            static_cast<unsigned long long>(Handle(image)),
            static_cast<unsigned long long>(duration));
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
    if (trace || duration >= 50000u)
    {
        SL_LOG_INFO(
            "[RenoDX][diag-v1] #%llu client.acquire.end activation=%llu activated=1 image=0x%llx size=%ux%u format=%u durationUs=%llu",
            static_cast<unsigned long long>(
                trace ? event : streamline_diagnostics::NextSequence()),
            static_cast<unsigned long long>(activation),
            static_cast<unsigned long long>(Handle(image)),
            info.width,
            info.height,
            info.format,
            static_cast<unsigned long long>(duration));
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

    const uint64_t conversion =
        streamline_diagnostics::conversion_calls.fetch_add(
            1u, std::memory_order_relaxed) + 1u;
    const bool trace = streamline_diagnostics::TakeDetailedTrace();
    const uint64_t event = trace
        ? streamline_diagnostics::NextSequence() : 0u;
    const auto start = streamline_diagnostics::Clock::now();
    if (trace)
    {
        SL_LOG_INFO(
            "[RenoDX][diag-v1] #%llu client.convert.begin conversion=%llu commandBuffer=0x%llx image=0x%llx size=%ux%u format=%u",
            static_cast<unsigned long long>(event),
            static_cast<unsigned long long>(conversion),
            static_cast<unsigned long long>(Handle(command_buffer)),
            static_cast<unsigned long long>(Handle(image)),
            info.width,
            info.height,
            info.format);
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
        const uint64_t duration =
            streamline_diagnostics::ElapsedMicros(start);
        if (trace || duration >= 50000u)
        {
            SL_LOG_INFO(
                "[RenoDX][diag-v1] #%llu client.convert.end conversion=%llu converted=1 durationUs=%llu",
                static_cast<unsigned long long>(
                    trace ? event : streamline_diagnostics::NextSequence()),
                static_cast<unsigned long long>(conversion),
                static_cast<unsigned long long>(duration));
        }
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
    SL_LOG_ERROR(
        "[RenoDX][diag-v1] #%llu client.convert.end conversion=%llu converted=0 durationUs=%llu",
        static_cast<unsigned long long>(
            trace ? event : streamline_diagnostics::NextSequence()),
        static_cast<unsigned long long>(conversion),
        static_cast<unsigned long long>(
            streamline_diagnostics::ElapsedMicros(start)));
    return false;
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

}


inline VkResult VKAPI_CALL QueuePresent(
    VkQueue queue,
    const VkPresentInfoKHR* present_info)
{
    using namespace streamline_diagnostics;
    Announce();
    ObserveForeground("dlssg-present");
    const uint64_t call = dlssg_present_calls.fetch_add(
        1u, std::memory_order_relaxed) + 1u;
    const bool trace = TakeDetailedTrace();
    const uint64_t event = trace ? NextSequence() : 0u;
    const auto start = Clock::now();
    const bool asynchronous = outer_present_hook_depth == 0u;
    if (trace)
    {
        SL_LOG_INFO(
            "[RenoDX][diag-v1] #%llu dlssgPresent.begin call=%llu queue=0x%llx async=%u outerDepth=%u waits=%u swapchains=%u firstSwapchain=0x%llx firstImage=%u",
            static_cast<unsigned long long>(event),
            static_cast<unsigned long long>(call),
            static_cast<unsigned long long>(Handle(queue)),
            asynchronous ? 1u : 0u,
            outer_present_hook_depth,
            present_info ? present_info->waitSemaphoreCount : 0u,
            present_info ? present_info->swapchainCount : 0u,
            static_cast<unsigned long long>(
                present_info && present_info->swapchainCount != 0u
                    && present_info->pSwapchains
                    ? Handle(present_info->pSwapchains[0]) : 0u),
            present_info && present_info->swapchainCount != 0u
                    && present_info->pImageIndices
                ? present_info->pImageIndices[0] : UINT32_MAX);
    }
    const bool marked = asynchronous && SetDisplayReadyPQPresent(true);
    if (asynchronous && !marked)
    {
        static std::once_flag logged;
        std::call_once(logged, [] {
            SL_LOG_ERROR("[RenoDX][client-fp16] Failed to mark DLSS-G display-ready present");
        });
    }

    if (trace)
    {
        SL_LOG_INFO(
            "[RenoDX][diag-v1] #%llu dlssgPresent.native.begin call=%llu marked=%u",
            static_cast<unsigned long long>(event),
            static_cast<unsigned long long>(call),
            marked ? 1u : 0u);
    }
    const VkResult result = s_ddt.QueuePresentKHR(queue, present_info);
    if (trace)
    {
        SL_LOG_INFO(
            "[RenoDX][diag-v1] #%llu dlssgPresent.native.end call=%llu result=%d",
            static_cast<unsigned long long>(event),
            static_cast<unsigned long long>(call),
            static_cast<int32_t>(result));
    }

    const bool cleared = !marked || SetDisplayReadyPQPresent(false);
    if (!cleared)
    {
        static std::once_flag logged;
        std::call_once(logged, [] {
            SL_LOG_ERROR("[RenoDX][client-fp16] Failed to clear DLSS-G display-ready present");
        });
    }
    const uint64_t duration = ElapsedMicros(start);
    if (trace || result != VK_SUCCESS || duration >= 50000u)
    {
        SL_LOG_INFO(
            "[RenoDX][diag-v1] #%llu dlssgPresent.end call=%llu result=%d async=%u marked=%u cleared=%u durationUs=%llu",
            static_cast<unsigned long long>(trace ? event : NextSequence()),
            static_cast<unsigned long long>(call),
            static_cast<int32_t>(result),
            asynchronous ? 1u : 0u,
            marked ? 1u : 0u,
            cleared ? 1u : 0u,
            static_cast<unsigned long long>(duration));
    }
    if (call % 300u == 0u)
    {
        LogHeartbeat("dlssg-present", call);
    }
    return result;
}

inline PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(
    VkDevice device,
    const char* name)
{
    if (name && strcmp(name, "vkQueuePresentKHR") == 0)
    {
        static std::once_flag logged;
        std::call_once(logged, [] {
            streamline_diagnostics::Announce();
            SL_LOG_INFO("[RenoDX][diag-v1] DLSS-G requested the client-image present function");
        });
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
