#pragma once

#include <cstdint>

namespace renodx::streamline_bridge {

constexpr uint32_t kAbiVersion = 3u;
constexpr uint32_t kClientImageOperationRegister = 1u;
constexpr uint32_t kClientImageOperationActivate = 2u;
constexpr uint32_t kClientImageOperationConvert = 3u;
constexpr uint32_t kClientImageOperationConvertDisplay = 4u;

using IsHDR10EnabledV1 = uint32_t(__cdecl*)() noexcept;
using ConvertVulkanTaggedResourceV1 = uint32_t(__cdecl*)(
    uint32_t abi_version,
    uint64_t command_buffer,
    uint64_t source_image_view,
    uint64_t target_image_view,
    uint32_t width,
    uint32_t height) noexcept;

using ManageVulkanClientImageV1 = uint32_t(__cdecl*)(
    uint32_t abi_version,
    uint32_t operation,
    uint64_t command_buffer,
    uint64_t image,
    uint32_t width,
    uint32_t height,
    uint32_t native_format) noexcept;

using SetVulkanDisplayReadyPQPresentV1 = uint32_t(__cdecl*)(
    uint32_t abi_version,
    uint32_t active) noexcept;

}  // namespace renodx::streamline_bridge

extern "C" void renodxUpdateDLSSGFocusRecovery(
    uint32_t foreground) noexcept;
extern "C" void renodxCompleteDLSSGFocusRecovery() noexcept;
