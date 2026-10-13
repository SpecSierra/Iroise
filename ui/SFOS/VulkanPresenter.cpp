/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define VK_USE_PLATFORM_WAYLAND_KHR

#include <AK/Array.h>
#include <AK/Format.h>
#include <UI/SFOS/VulkanPresenter.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan_wayland.h>

namespace Ladybird {

// The compositor cycles through a few backing stores per view; keep a little more than that imported.
static constexpr size_t maximum_imported_images = 6;

static constexpr VkImageSubresourceRange color_range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

static Error vulkan_error(char const* what, VkResult result)
{
    warnln("ladybird-sfos: {} failed with VkResult {}", what, to_underlying(result));
    return Error::from_string_literal("A Vulkan call failed");
}

#define VK_TRY(what, expression)                           \
    do {                                                   \
        if (auto _result = (expression); _result != VK_SUCCESS) \
            return vulkan_error(what, _result);            \
    } while (0)

ErrorOr<NonnullOwnPtr<VulkanPresenter>> VulkanPresenter::create(wl_display* display, wl_surface* surface, Gfx::IntSize size)
{
    auto presenter = adopt_own(*new VulkanPresenter);
    TRY(presenter->initialize(display, surface, size));
    return presenter;
}

ErrorOr<void> VulkanPresenter::initialize(wl_display* display, wl_surface* surface, Gfx::IntSize size)
{
    static constexpr Array instance_extensions { "VK_KHR_surface", "VK_KHR_wayland_surface" };
    VkApplicationInfo application_info {};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Ladybird";
    application_info.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;
    instance_info.enabledExtensionCount = instance_extensions.size();
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    VK_TRY("vkCreateInstance", vkCreateInstance(&instance_info, nullptr, &m_instance));

    VkWaylandSurfaceCreateInfoKHR surface_info {};
    surface_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    surface_info.display = display;
    surface_info.surface = surface;
    VK_TRY("vkCreateWaylandSurfaceKHR", vkCreateWaylandSurfaceKHR(m_instance, &surface_info, nullptr, &m_surface));

    u32 device_count = 1;
    auto enumerate_result = vkEnumeratePhysicalDevices(m_instance, &device_count, &m_physical_device);
    if ((enumerate_result != VK_SUCCESS && enumerate_result != VK_INCOMPLETE) || device_count == 0)
        return Error::from_string_literal("No Vulkan device");

    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count, nullptr);
    Vector<VkQueueFamilyProperties> families;
    families.resize(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count, families.data());
    Optional<u32> queue_family;
    for (u32 i = 0; i < family_count; ++i) {
        VkBool32 can_present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, i, m_surface, &can_present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) {
            queue_family = i;
            break;
        }
    }
    if (!queue_family.has_value())
        return Error::from_string_literal("No Vulkan queue can present to the window");
    m_queue_family = *queue_family;

    static constexpr Array device_extensions {
        "VK_KHR_swapchain",
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier",
        "VK_KHR_image_format_list",
    };
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = m_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;
    VkDeviceCreateInfo device_info {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = device_extensions.size();
    device_info.ppEnabledExtensionNames = device_extensions.data();
    VK_TRY("vkCreateDevice", vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

    VkCommandPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = m_queue_family;
    VK_TRY("vkCreateCommandPool", vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool));
    VkCommandBufferAllocateInfo command_buffer_info {};
    command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_info.commandPool = m_command_pool;
    command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_info.commandBufferCount = 1;
    VK_TRY("vkAllocateCommandBuffers", vkAllocateCommandBuffers(m_device, &command_buffer_info, &m_command_buffer));

    VkSemaphoreCreateInfo semaphore_info {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_TRY("vkCreateSemaphore", vkCreateSemaphore(m_device, &semaphore_info, nullptr, &m_image_acquired));
    VK_TRY("vkCreateSemaphore", vkCreateSemaphore(m_device, &semaphore_info, nullptr, &m_copy_finished));
    VkFenceCreateInfo fence_info {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_TRY("vkCreateFence", vkCreateFence(m_device, &fence_info, nullptr, &m_frame_fence));

    return create_swapchain(size);
}

VulkanPresenter::~VulkanPresenter()
{
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        for (auto& image : m_imported_images)
            destroy(image);
        destroy_swapchain();
        if (m_frame_fence != VK_NULL_HANDLE)
            vkDestroyFence(m_device, m_frame_fence, nullptr);
        if (m_copy_finished != VK_NULL_HANDLE)
            vkDestroySemaphore(m_device, m_copy_finished, nullptr);
        if (m_image_acquired != VK_NULL_HANDLE)
            vkDestroySemaphore(m_device, m_image_acquired, nullptr);
        if (m_command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(m_device, m_command_pool, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_instance, nullptr);
}

ErrorOr<void> VulkanPresenter::create_swapchain(Gfx::IntSize size)
{
    VkSurfaceCapabilitiesKHR capabilities {};
    VK_TRY("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface, &capabilities));

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, nullptr);
    Vector<VkSurfaceFormatKHR> formats;
    formats.resize(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, formats.data());
    if (formats.is_empty())
        return Error::from_string_literal("The window has no Vulkan surface formats");
    // NB: The J2 offers RGBA orders only, while shared images are BGRA. vkCmdBlitImage converts between them.
    auto surface_format = formats.first();
    for (auto const& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) {
            surface_format = format;
            break;
        }
    }

    // NB: The surface reports a 1x1 current extent until something is attached, so the size is ours to choose.
    VkSwapchainCreateInfoKHR swapchain_info {};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = m_surface;
    swapchain_info.minImageCount = max(capabilities.minImageCount, 3u);
    swapchain_info.imageFormat = surface_format.format;
    swapchain_info.imageColorSpace = surface_format.colorSpace;
    swapchain_info.imageExtent = { static_cast<u32>(size.width()), static_cast<u32>(size.height()) };
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;
    VK_TRY("vkCreateSwapchainKHR", vkCreateSwapchainKHR(m_device, &swapchain_info, nullptr, &m_swapchain));

    u32 image_count = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, nullptr);
    m_swapchain_images.resize(image_count);
    VK_TRY("vkGetSwapchainImagesKHR", vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, m_swapchain_images.data()));

    m_swapchain_format = surface_format.format;
    m_swapchain_size = size;
    return {};
}

void VulkanPresenter::destroy_swapchain()
{
    if (m_swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
    m_swapchain_images.clear();
}

ErrorOr<void> VulkanPresenter::resize(Gfx::IntSize size)
{
    if (size == m_swapchain_size)
        return {};
    vkDeviceWaitIdle(m_device);
    m_frame_in_flight = false;
    destroy_swapchain();
    return create_swapchain(size);
}

void VulkanPresenter::destroy(ImportedImage& imported)
{
    if (imported.image != VK_NULL_HANDLE)
        vkDestroyImage(m_device, imported.image, nullptr);
    if (imported.memory != VK_NULL_HANDLE)
        vkFreeMemory(m_device, imported.memory, nullptr);
    imported = {};
}

ErrorOr<VulkanPresenter::ImportedImage*> VulkanPresenter::imported_image_for(Gfx::LinuxDmaBufHandle const& dmabuf)
{
    // Every message about a backing store carries a fresh duplicate of its file descriptor, so recognize the
    // buffer behind it by its inode.
    struct stat status {};
    if (fstat(dmabuf.file.fd(), &status) < 0)
        return Error::from_syscall("fstat"sv, errno);

    for (auto& imported : m_imported_images) {
        if (imported.device == status.st_dev && imported.inode == status.st_ino && imported.size == dmabuf.size)
            return &imported;
    }

    if (m_imported_images.size() >= maximum_imported_images) {
        size_t oldest = 0;
        for (size_t i = 1; i < m_imported_images.size(); ++i) {
            if (m_imported_images[i].last_used_frame < m_imported_images[oldest].last_used_frame)
                oldest = i;
        }
        vkDeviceWaitIdle(m_device);
        m_frame_in_flight = false;
        destroy(m_imported_images[oldest]);
        m_imported_images.remove(oldest);
    }

    m_imported_images.append(TRY(import(dmabuf, status.st_dev, status.st_ino)));
    return &m_imported_images.last();
}

ErrorOr<VulkanPresenter::ImportedImage> VulkanPresenter::import(Gfx::LinuxDmaBufHandle const& dmabuf, dev_t device, ino_t inode)
{
    ImportedImage imported;
    imported.device = device;
    imported.inode = inode;
    imported.size = dmabuf.size;

    VkSubresourceLayout plane_layout {};
    plane_layout.offset = 0;
    plane_layout.rowPitch = dmabuf.pitch;
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info {};
    modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifier_info.drmFormatModifier = dmabuf.modifier;
    modifier_info.drmFormatModifierPlaneCount = 1;
    modifier_info.pPlaneLayouts = &plane_layout;
    VkExternalMemoryImageCreateInfo external_info {};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_info.pNext = &modifier_info;
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo image_info {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    // NB: Shared images are always DRM_FORMAT_ARGB8888, which is this format.
    image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    image_info.extent = { static_cast<u32>(dmabuf.size.width()), static_cast<u32>(dmabuf.size.height()), 1 };
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_TRY("vkCreateImage", vkCreateImage(m_device, &image_info, nullptr, &imported.image));

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(m_device, imported.image, &requirements);

    auto get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(m_device, "vkGetMemoryFdPropertiesKHR"));
    VkMemoryFdPropertiesKHR fd_properties {};
    fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (!get_memory_fd_properties || get_memory_fd_properties(m_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dmabuf.file.fd(), &fd_properties) != VK_SUCCESS) {
        destroy(imported);
        return Error::from_string_literal("The driver rejected the shared image's dma-buf");
    }
    auto usable_types = requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    if (usable_types == 0) {
        destroy(imported);
        return Error::from_string_literal("No memory type fits the shared image");
    }

    struct stat status {};
    fstat(dmabuf.file.fd(), &status);
    // NB: A successful import takes ownership of the file descriptor, so give the driver a duplicate.
    int import_fd = fcntl(dmabuf.file.fd(), F_DUPFD_CLOEXEC, 0);
    if (import_fd < 0) {
        destroy(imported);
        return Error::from_syscall("fcntl"sv, errno);
    }
    VkMemoryDedicatedAllocateInfo dedicated_info {};
    dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_info.image = imported.image;
    VkImportMemoryFdInfoKHR import_info {};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.pNext = &dedicated_info;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = import_fd;
    VkMemoryAllocateInfo allocate_info {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &import_info;
    allocate_info.allocationSize = max(requirements.size, static_cast<VkDeviceSize>(status.st_size));
    allocate_info.memoryTypeIndex = static_cast<u32>(__builtin_ctz(usable_types));
    if (auto result = vkAllocateMemory(m_device, &allocate_info, nullptr, &imported.memory); result != VK_SUCCESS) {
        ::close(import_fd);
        destroy(imported);
        return vulkan_error("vkAllocateMemory (import)", result);
    }
    if (auto result = vkBindImageMemory(m_device, imported.image, imported.memory, 0); result != VK_SUCCESS) {
        destroy(imported);
        return vulkan_error("vkBindImageMemory", result);
    }
    return imported;
}

ErrorOr<void> VulkanPresenter::present(Gfx::LinuxDmaBufHandle const& dmabuf, Gfx::IntSize painted_size)
{
    if (m_frame_in_flight) {
        vkWaitForFences(m_device, 1, &m_frame_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &m_frame_fence);
        m_frame_in_flight = false;
    }

    auto* source = TRY(imported_image_for(dmabuf));
    source->last_used_frame = ++m_frame_number;

    u32 image_index = 0;
    auto acquire_result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_image_acquired, VK_NULL_HANDLE, &image_index);
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR)
        return vulkan_error("vkAcquireNextImageKHR", acquire_result);
    auto target = m_swapchain_images[image_index];

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_TRY("vkBeginCommandBuffer", vkBeginCommandBuffer(m_command_buffer, &begin_info));

    // NB: The other process owns the source image's contents and keeps it in the general layout. Taking it over
    //     from the foreign queue family with that same layout is what keeps those contents.
    Array<VkImageMemoryBarrier, 2> to_transfer {};
    to_transfer[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_transfer[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_transfer[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_transfer[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    to_transfer[0].dstQueueFamilyIndex = m_queue_family;
    to_transfer[0].image = source->image;
    to_transfer[0].subresourceRange = color_range;
    to_transfer[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer[1].image = target;
    to_transfer[1].subresourceRange = color_range;
    vkCmdPipelineBarrier(m_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, to_transfer.size(), to_transfer.data());

    auto width = min(min(dmabuf.size.width(), painted_size.width()), m_swapchain_size.width());
    auto height = min(min(dmabuf.size.height(), painted_size.height()), m_swapchain_size.height());
    if (width < m_swapchain_size.width() || height < m_swapchain_size.height()) {
        VkClearColorValue white {};
        white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.0f;
        vkCmdClearColorImage(m_command_buffer, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &color_range);
    }
    VkImageBlit region {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1] = { width, height, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[1] = { width, height, 1 };
    vkCmdBlitImage(m_command_buffer, source->image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

    Array<VkImageMemoryBarrier, 2> to_present {};
    to_present[0] = to_transfer[0];
    to_present[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_present[0].dstAccessMask = 0;
    to_present[0].srcQueueFamilyIndex = m_queue_family;
    to_present[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    to_present[1] = to_transfer[1];
    to_present[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present[1].dstAccessMask = 0;
    to_present[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(m_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, to_present.size(), to_present.data());
    VK_TRY("vkEndCommandBuffer", vkEndCommandBuffer(m_command_buffer));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit_info {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &m_image_acquired;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &m_copy_finished;
    VK_TRY("vkQueueSubmit", vkQueueSubmit(m_queue, 1, &submit_info, m_frame_fence));
    m_frame_in_flight = true;

    VkPresentInfoKHR present_info {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &m_copy_finished;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &m_swapchain;
    present_info.pImageIndices = &image_index;
    auto present_result = vkQueuePresentKHR(m_queue, &present_info);
    if (present_result != VK_SUCCESS && present_result != VK_SUBOPTIMAL_KHR)
        return vulkan_error("vkQueuePresentKHR", present_result);
    return {};
}

}
