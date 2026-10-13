/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>

#include <sys/types.h>
#include <vulkan/vulkan.h>

struct wl_display;
struct wl_surface;

namespace Ladybird {

// Shows the compositor's shared GPU images on a Wayland surface without touching their pixels on the CPU: each
// dma-buf is imported once as a Vulkan image and then copied to a swapchain image on the GPU.
class VulkanPresenter {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<VulkanPresenter>> create(wl_display*, wl_surface*, Gfx::IntSize);
    ~VulkanPresenter();

    ErrorOr<void> present(Gfx::LinuxDmaBufHandle const&, Gfx::IntSize painted_size);
    ErrorOr<void> resize(Gfx::IntSize);

private:
    struct ImportedImage {
        dev_t device { 0 };
        ino_t inode { 0 };
        Gfx::IntSize size;
        VkImage image { VK_NULL_HANDLE };
        VkDeviceMemory memory { VK_NULL_HANDLE };
        u64 last_used_frame { 0 };
    };

    VulkanPresenter() = default;

    ErrorOr<void> initialize(wl_display*, wl_surface*, Gfx::IntSize);
    ErrorOr<void> create_swapchain(Gfx::IntSize);
    void destroy_swapchain();
    ErrorOr<ImportedImage*> imported_image_for(Gfx::LinuxDmaBufHandle const&);
    ErrorOr<ImportedImage> import(Gfx::LinuxDmaBufHandle const&, dev_t, ino_t);
    void destroy(ImportedImage&);

    VkInstance m_instance { VK_NULL_HANDLE };
    VkSurfaceKHR m_surface { VK_NULL_HANDLE };
    VkPhysicalDevice m_physical_device { VK_NULL_HANDLE };
    VkDevice m_device { VK_NULL_HANDLE };
    u32 m_queue_family { 0 };
    VkQueue m_queue { VK_NULL_HANDLE };
    VkCommandPool m_command_pool { VK_NULL_HANDLE };
    VkCommandBuffer m_command_buffer { VK_NULL_HANDLE };
    VkSemaphore m_image_acquired { VK_NULL_HANDLE };
    VkSemaphore m_copy_finished { VK_NULL_HANDLE };
    VkFence m_frame_fence { VK_NULL_HANDLE };
    bool m_frame_in_flight { false };

    VkSwapchainKHR m_swapchain { VK_NULL_HANDLE };
    VkFormat m_swapchain_format { VK_FORMAT_UNDEFINED };
    Gfx::IntSize m_swapchain_size;
    Vector<VkImage> m_swapchain_images;

    Vector<ImportedImage> m_imported_images;
    u64 m_frame_number { 0 };
};

}
