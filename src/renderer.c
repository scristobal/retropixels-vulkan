#define GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_NONE

#include <GLFW/glfw3.h>
#include <stb_image.h>
#include "renderer.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES_IN_FLIGHT 2
#define SPRITE_SIZE 32
#define CANVAS_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define TEXTURE_FORMAT VK_FORMAT_R8G8B8A8_SRGB
#define VALIDATION_LAYER "VK_LAYER_KHRONOS_validation"

#define VK_TRY(call) do { if (!vk_ok((call), #call)) return 0; } while (0)

typedef struct Buffer {
    VkBuffer handle;
    VkDeviceMemory memory;
} Buffer;

typedef struct Image {
    VkImage handle;
    VkDeviceMemory memory;
    VkImageView view;
} Image;

typedef struct Frame {
    VkCommandBuffer command;
    VkSemaphore acquired;
    VkFence fence;

    Image canvas;
    bool canvas_initialized;
} Frame;

typedef struct SwapchainImage {
    VkImage handle;

    /* Presentation can outlive a frame's submit fence. Reuse by image index. */
    VkSemaphore finished;
} SwapchainImage;

typedef struct Swapchain {
    VkSwapchainKHR handle;
    VkExtent2D extent;
    uint32_t count;
    SwapchainImage *images;
} Swapchain;

typedef struct SpriteConstants {
    float rectangle[4];
    float canvas_size[2];
} SpriteConstants;

static_assert(sizeof(SpriteConstants) == 24, "Sprite push constants must occupy 24 bytes.");
static_assert(offsetof(SpriteConstants, canvas_size) == 16, "Canvas size must match the shader block offset.");

struct Renderer {
    GLFWwindow *window;

    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    VkSurfaceKHR surface;

    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t graphics_family, present_family;
    VkQueue graphics_queue, present_queue;
    VkCommandPool command_pool;

    Image texture;
    VkSampler sampler;

    VkDescriptorSetLayout descriptor_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    Swapchain swapchain;
    Frame frames[FRAMES_IN_FLIGHT];
    uint32_t frame_index;

    bool surface_changed;
    atomic_uint validation_errors;
};

static int vk_ok(VkResult result, const char *operation)
{
    if (result == VK_SUCCESS) return 1;

    fprintf(stderr, "%s failed (VkResult %d).\n", operation, (int)result);
    return 0;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    Renderer *r = user;
    (void)type;

    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        atomic_fetch_add(&r->validation_errors, 1);

    fprintf(stderr, "Vulkan %s: %s\n",
            (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning",
            data->pMessage);

    return VK_FALSE;
}

static int create_instance(Renderer *r, int validation)
{
    uint32_t layer_count = 0, extension_count = 0, glfw_count = 0;
    VkLayerProperties *layers = NULL;
    VkExtensionProperties *available = NULL;
    const char **extensions = NULL;
    const char **glfw_extensions;
    int has_validation = 0, has_debug = 0, has_portability = 0, success = 0;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback,
        .pUserData = r
    };
    const char *layer_name = VALIDATION_LAYER;

    if (!glfwVulkanSupported()) {
        fprintf(stderr, "Vulkan is unavailable. Install a Vulkan loader and a compatible GPU driver.\n");
        return 0;
    }

    {
        uint32_t version = VK_API_VERSION_1_0;

        VK_TRY(vkEnumerateInstanceVersion(&version));
        if (version < VK_API_VERSION_1_4) {
            fprintf(stderr, "Vulkan 1.4 or newer is required.\n");
            return 0;
        }
    }

    glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_count);
    if (!glfw_extensions || !glfw_count) {
        fprintf(stderr, "GLFW could not find Vulkan window-surface extensions.\n");
        return 0;
    }

    if (!vk_ok(vkEnumerateInstanceLayerProperties(&layer_count, NULL), "Enumerate instance layers")) goto done;
    layers = calloc(layer_count ? layer_count : 1, sizeof(*layers));
    if (!layers) goto done;
    if (!vk_ok(vkEnumerateInstanceLayerProperties(&layer_count, layers), "Enumerate instance layers")) goto done;
    for (uint32_t i = 0; i < layer_count; ++i)
        if (strcmp(layers[i].layerName, layer_name) == 0) has_validation = 1;

    if (!vk_ok(vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL), "Enumerate instance extensions")) goto done;
    available = calloc(extension_count ? extension_count : 1, sizeof(*available));
    if (!available) goto done;
    if (!vk_ok(vkEnumerateInstanceExtensionProperties(NULL, &extension_count, available), "Enumerate instance extensions")) goto done;
    for (uint32_t i = 0; i < extension_count; ++i) {
        if (strcmp(available[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) has_debug = 1;
        if (strcmp(available[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) has_portability = 1;
    }

    if (validation == 1 && (!has_validation || !has_debug)) {
        fprintf(stderr, "--validation requires %s and VK_EXT_debug_utils.\n", layer_name);
        goto done;
    }
    if (validation == -1 && (!has_validation || !has_debug))
        fprintf(stderr, "Validation is unavailable; continuing without it.\n");
    validation = validation != 0 && has_validation && has_debug;

    extensions = calloc((size_t)glfw_count + 2, sizeof(*extensions));
    if (!extensions) goto done;
    memcpy(extensions, glfw_extensions, glfw_count * sizeof(*extensions));
    extension_count = glfw_count;
    if (validation) extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (has_portability) extensions[extension_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;

    app.pApplicationName = "RetroPixels Vulkan";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "RetroPixels";
    app.apiVersion = VK_API_VERSION_1_4;

    info.pApplicationInfo = &app;
    info.enabledExtensionCount = extension_count;
    info.ppEnabledExtensionNames = extensions;
    if (has_portability) info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    if (validation) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &layer_name;
        info.pNext = &debug;
    }

    if (!vk_ok(vkCreateInstance(&info, NULL, &r->instance), "Create Vulkan instance")) goto done;

    if (validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_debug =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(r->instance, "vkCreateDebugUtilsMessengerEXT");

        if (!create_debug || !vk_ok(create_debug(r->instance, &debug, NULL, &r->debug_messenger), "Create debug messenger")) goto done;
        fprintf(stderr, "Vulkan validation enabled.\n");
    }

    success = 1;

done:
    free(extensions);
    free(available);
    free(layers);

    return success;
}

static int device_has_extension(VkPhysicalDevice device, const char *name)
{
    uint32_t count = 0;
    VkExtensionProperties *extensions;
    int found = 0;

    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) != VK_SUCCESS) return 0;
    extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) return 0;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, extensions) == VK_SUCCESS)
        for (uint32_t i = 0; i < count; ++i)
            if (strcmp(extensions[i].extensionName, name) == 0) found = 1;

    free(extensions);

    return found;
}

static int queue_families(Renderer *r, VkPhysicalDevice device, uint32_t *graphics, uint32_t *present)
{
    uint32_t count = 0;
    VkQueueFamilyProperties *properties;

    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    properties = calloc(count ? count : 1, sizeof(*properties));
    if (!properties) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties);

    *graphics = *present = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        VkBool32 supported = VK_FALSE;

        if (!properties[i].queueCount) continue;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, r->surface, &supported) != VK_SUCCESS) continue;

        if (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            *graphics = i;
            if (supported) {
                *present = i;
                break;
            }
        }
        if (supported) *present = i;
    }

    free(properties);

    return *graphics != UINT32_MAX && *present != UINT32_MAX;
}

static int surface_format(Renderer *r, VkPhysicalDevice device, VkSurfaceFormatKHR *selected)
{
    uint32_t count = 0;
    VkSurfaceFormatKHR *formats;
    const VkFormat preferences[] = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB };
    int found = 0;

    if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, r->surface, &count, NULL) != VK_SUCCESS || !count) return 0;
    formats = calloc(count, sizeof(*formats));
    if (!formats) return 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, r->surface, &count, formats) != VK_SUCCESS) goto done;

    for (size_t p = 0; p < sizeof(preferences) / sizeof(preferences[0]) && !found; ++p) {
        VkFormatProperties properties;

        vkGetPhysicalDeviceFormatProperties(device, preferences[p], &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) continue;

        for (uint32_t i = 0; i < count; ++i) {
            if ((formats[i].format == preferences[p] || formats[i].format == VK_FORMAT_UNDEFINED) &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                *selected = (VkSurfaceFormatKHR){ preferences[p], VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
                found = 1;
                break;
            }
        }
    }

done:
    free(formats);

    return found;
}

static int create_device(Renderer *r)
{
    uint32_t count = 0;
    VkPhysicalDevice *devices;
    int best_score = -1;

    VK_TRY(vkEnumeratePhysicalDevices(r->instance, &count, NULL));
    if (!count) {
        fprintf(stderr, "No Vulkan physical devices found.\n");
        return 0;
    }

    devices = calloc(count, sizeof(*devices));
    if (!devices) return 0;
    if (!vk_ok(vkEnumeratePhysicalDevices(r->instance, &count, devices), "Enumerate physical devices")) {
        free(devices);
        return 0;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t graphics, present;
        VkPhysicalDeviceProperties properties;
        VkFormatProperties texture, canvas;
        VkSurfaceCapabilitiesKHR capabilities;
        VkSurfaceFormatKHR format;

        VkPhysicalDeviceVulkan14Features vulkan14 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
        VkPhysicalDeviceVulkan13Features vulkan13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &vulkan14
        };
        VkPhysicalDeviceFeatures2 features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vulkan13 };
        const VkFormatFeatureFlags canvas_features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        int score;

        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_4) continue;

        vkGetPhysicalDeviceFeatures2(devices[i], &features);
        if (!vulkan13.dynamicRendering || !vulkan13.synchronization2 || !vulkan14.pushDescriptor) continue;

        if (!device_has_extension(devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
            !queue_families(r, devices[i], &graphics, &present)) continue;

        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(devices[i], r->surface, &capabilities) != VK_SUCCESS ||
            !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) continue;
        if (!surface_format(r, devices[i], &format)) continue;

        vkGetPhysicalDeviceFormatProperties(devices[i], TEXTURE_FORMAT, &texture);
        vkGetPhysicalDeviceFormatProperties(devices[i], CANVAS_FORMAT, &canvas);
        if (!(texture.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
            (canvas.optimalTilingFeatures & canvas_features) != canvas_features) continue;

        score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 :
                properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 50 : 0;
        if (score > best_score) {
            best_score = score;
            r->physical_device = devices[i];
            r->graphics_family = graphics;
            r->present_family = present;
        }
    }

    free(devices);
    if (!r->physical_device) {
        fprintf(stderr, "A Vulkan 1.4 device with dynamic rendering, synchronization2, push descriptors, "
                        "linear floating-point rendering and sRGB blit presentation is required.\n");
        return 0;
    }

    {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queues[2] = {
            { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
              .queueFamilyIndex = r->graphics_family, .queueCount = 1, .pQueuePriorities = &priority },
            { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
              .queueFamilyIndex = r->present_family, .queueCount = 1, .pQueuePriorities = &priority }
        };

        VkPhysicalDeviceVulkan14Features vulkan14 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pushDescriptor = VK_TRUE
        };
        VkPhysicalDeviceVulkan13Features features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &vulkan14,
            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE
        };

        const char *extensions[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset" };
        VkDeviceCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features,
            .queueCreateInfoCount = r->graphics_family == r->present_family ? 1 : 2,
            .pQueueCreateInfos = queues,
            .enabledExtensionCount = device_has_extension(r->physical_device, extensions[1]) ? 2 : 1,
            .ppEnabledExtensionNames = extensions
        };
        VkPhysicalDeviceProperties properties;

        VK_TRY(vkCreateDevice(r->physical_device, &info, NULL, &r->device));
        vkGetDeviceQueue(r->device, r->graphics_family, 0, &r->graphics_queue);
        vkGetDeviceQueue(r->device, r->present_family, 0, &r->present_queue);

        vkGetPhysicalDeviceProperties(r->physical_device, &properties);
        fprintf(stderr, "Vulkan device: %s (API %u.%u.%u)\n", properties.deviceName,
                VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
                VK_API_VERSION_PATCH(properties.apiVersion));
    }

    return 1;
}

static int memory_type(Renderer *r, uint32_t mask, VkMemoryPropertyFlags flags, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties properties;

    vkGetPhysicalDeviceMemoryProperties(r->physical_device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((mask & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) {
            *index = i;
            return 1;
        }
    }

    fprintf(stderr, "No Vulkan memory type matches flags 0x%x.\n", flags);
    return 0;
}

static void destroy_buffer(Renderer *r, Buffer *buffer)
{
    if (buffer->handle) vkDestroyBuffer(r->device, buffer->handle, NULL);
    if (buffer->memory) vkFreeMemory(r->device, buffer->memory, NULL);

    memset(buffer, 0, sizeof(*buffer));
}

static int create_buffer(Renderer *r, VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags flags, Buffer *buffer)
{
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };

    if (!vk_ok(vkCreateBuffer(r->device, &info, NULL, &buffer->handle), "Create buffer")) goto fail;
    vkGetBufferMemoryRequirements(r->device, buffer->handle, &requirements);

    allocation.allocationSize = requirements.size;
    if (!memory_type(r, requirements.memoryTypeBits, flags, &allocation.memoryTypeIndex)) goto fail;
    if (!vk_ok(vkAllocateMemory(r->device, &allocation, NULL, &buffer->memory), "Allocate buffer memory")) goto fail;
    if (!vk_ok(vkBindBufferMemory(r->device, buffer->handle, buffer->memory, 0), "Bind buffer memory")) goto fail;

    return 1;

fail:
    destroy_buffer(r, buffer);
    return 0;
}

static int fill_buffer(Renderer *r, Buffer *buffer, const void *data, VkDeviceSize size)
{
    void *mapped;

    VK_TRY(vkMapMemory(r->device, buffer->memory, 0, size, 0, &mapped));
    memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(r->device, buffer->memory);

    return 1;
}

static int begin_upload(Renderer *r, VkCommandBuffer *command)
{
    VkCommandBufferAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1
    };
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    VK_TRY(vkAllocateCommandBuffers(r->device, &allocation, command));
    if (!vk_ok(vkBeginCommandBuffer(*command, &begin), "Begin upload")) {
        vkFreeCommandBuffers(r->device, r->command_pool, 1, command);
        *command = VK_NULL_HANDLE;
        return 0;
    }

    return 1;
}

static int end_upload(Renderer *r, VkCommandBuffer command)
{
    VkCommandBufferSubmitInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = command
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1, .pCommandBufferInfos = &command_info
    };
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    int success = vk_ok(vkEndCommandBuffer(command), "End upload");

    if (success) success = vk_ok(vkCreateFence(r->device, &fence_info, NULL, &fence), "Create upload fence");
    if (success) success = vk_ok(vkQueueSubmit2(r->graphics_queue, 1, &submit, fence), "Submit upload");
    if (success) success = vk_ok(vkWaitForFences(r->device, 1, &fence, VK_TRUE, UINT64_MAX), "Wait for upload");

    if (fence) vkDestroyFence(r->device, fence, NULL);
    vkFreeCommandBuffers(r->device, r->command_pool, 1, &command);

    return success;
}

static void image_barrier(VkCommandBuffer command, VkImage image,
                           VkImageLayout old_layout, VkImageLayout new_layout,
                           VkPipelineStageFlags2 source_stage, VkAccessFlags2 source_access,
                           VkPipelineStageFlags2 destination_stage, VkAccessFlags2 destination_access)
{
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = source_stage, .srcAccessMask = source_access,
        .dstStageMask = destination_stage, .dstAccessMask = destination_access,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier
    };

    vkCmdPipelineBarrier2(command, &dependency);
}

static void destroy_image(Renderer *r, Image *image)
{
    if (image->view) vkDestroyImageView(r->device, image->view, NULL);
    if (image->handle) vkDestroyImage(r->device, image->handle, NULL);
    if (image->memory) vkFreeMemory(r->device, image->memory, NULL);

    memset(image, 0, sizeof(*image));
}

static int create_image(Renderer *r, uint32_t width, uint32_t height, VkFormat format,
                        VkImageUsageFlags usage, Image *image)
{
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { width, height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };

    if (!vk_ok(vkCreateImage(r->device, &info, NULL, &image->handle), "Create image")) goto fail;
    vkGetImageMemoryRequirements(r->device, image->handle, &requirements);

    allocation.allocationSize = requirements.size;
    if (!memory_type(r, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &allocation.memoryTypeIndex)) goto fail;
    if (!vk_ok(vkAllocateMemory(r->device, &allocation, NULL, &image->memory), "Allocate image memory")) goto fail;
    if (!vk_ok(vkBindImageMemory(r->device, image->handle, image->memory, 0), "Bind image memory")) goto fail;

    view.image = image->handle;
    if (!vk_ok(vkCreateImageView(r->device, &view, NULL, &image->view), "Create image view")) goto fail;

    return 1;

fail:
    destroy_image(r, image);
    return 0;
}

static int create_texture(Renderer *r)
{
    unsigned char *pixels;
    int width, height, channels, success = 0;
    Buffer staging = {0};
    VkCommandBuffer command;

    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0
    };

    VkBufferImageCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { SPRITE_SIZE, SPRITE_SIZE, 1 }
    };
    VkCopyBufferToImageInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .regionCount = 1, .pRegions = &region
    };
    const char *path = RETROPIXELS_ASSETS_DIR "/avatar-1x.png";

    pixels = stbi_load(path, &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        fprintf(stderr, "Failed to load %s: %s\n", path, stbi_failure_reason());
        return 0;
    }

    if (width != SPRITE_SIZE || height != SPRITE_SIZE) {
        fprintf(stderr, "Sprite must be %dx%d; got %dx%d.\n", SPRITE_SIZE, SPRITE_SIZE, width, height);
        goto done;
    }

    if (!create_buffer(r, SPRITE_SIZE * SPRITE_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) goto done;
    if (!fill_buffer(r, &staging, pixels, SPRITE_SIZE * SPRITE_SIZE * 4)) goto done;

    if (!create_image(r, SPRITE_SIZE, SPRITE_SIZE, TEXTURE_FORMAT,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &r->texture)) goto done;

    if (!begin_upload(r, &command)) goto done;
    image_barrier(command, r->texture.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    copy.srcBuffer = staging.handle;
    copy.dstImage = r->texture.handle;
    vkCmdCopyBufferToImage2(command, &copy);

    image_barrier(command, r->texture.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (!end_upload(r, command)) goto done;

    if (!vk_ok(vkCreateSampler(r->device, &sampler, NULL, &r->sampler), "Create sprite sampler")) goto done;
    success = 1;

done:
    destroy_buffer(r, &staging);
    stbi_image_free(pixels);

    return success;
}

static int create_descriptors(Renderer *r)
{
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
        .bindingCount = 1, .pBindings = &binding
    };

    VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SpriteConstants) };
    VkPipelineLayoutCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &r->descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push
    };

    VK_TRY(vkCreateDescriptorSetLayout(r->device, &layout, NULL, &r->descriptor_layout));
    VK_TRY(vkCreatePipelineLayout(r->device, &pipeline, NULL, &r->pipeline_layout));

    return 1;
}

static int load_shader(Renderer *r, const char *path, VkShaderModule *module)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint32_t *code = NULL;
    int success = 0;
    VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };

    if (!file) {
        fprintf(stderr, "Cannot open shader %s. Build the RetroPixelsShaders target.\n", path);
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) goto done;
    size = ftell(file);
    if (size < 20 || size > 64 * 1024 * 1024 || size % 4 != 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Invalid SPIR-V file: %s\n", path);
        goto done;
    }

    code = malloc((size_t)size);
    if (!code || fread(code, 1, (size_t)size, file) != (size_t)size) goto done;

    info.codeSize = (size_t)size;
    info.pCode = code;
    success = vk_ok(vkCreateShaderModule(r->device, &info, NULL, module), "Create shader module");

done:
    free(code);
    fclose(file);

    return success;
}

static int create_pipeline(Renderer *r)
{
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;

    VkPipelineVertexInputStateCreateInfo input = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };

    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };

    VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment
    };

    const VkDynamicState states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = states
    };

    VkPipelineShaderStageCreateInfo stages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main" }
    };

    const VkFormat format = CANVAS_FORMAT;
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &format
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &input, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &raster,
        .pMultisampleState = &multisample, .pColorBlendState = &blend, .pDynamicState = &dynamic,
        .layout = r->pipeline_layout
    };
    int success = 0;

    if (!load_shader(r, RETROPIXELS_SHADERS_DIR "/sprite.vert.spv", &vertex)) goto done;
    if (!load_shader(r, RETROPIXELS_SHADERS_DIR "/sprite.frag.spv", &fragment)) goto done;

    stages[0].module = vertex;
    stages[1].module = fragment;
    success = vk_ok(vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &pipeline, NULL,
                                             &r->pipeline), "Create sprite pipeline");

done:
    if (fragment) vkDestroyShaderModule(r->device, fragment, NULL);
    if (vertex) vkDestroyShaderModule(r->device, vertex, NULL);

    return success;
}

static int create_swapchain(Renderer *r)
{
    Swapchain *s = &r->swapchain;
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR format;
    VkImage *images = NULL;
    uint32_t image_count;
    uint32_t families[] = { r->graphics_family, r->present_family };
    VkSwapchainCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    int success = 0;

    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical_device, r->surface, &capabilities));
    if (!surface_format(r, r->physical_device, &format)) {
        fprintf(stderr, "The surface must support an sRGB swapchain with blit destinations.\n");
        return 0;
    }

    s->extent = (VkExtent2D){ PRESENT_WIDTH, PRESENT_HEIGHT };
    if ((capabilities.currentExtent.width != UINT32_MAX &&
         (capabilities.currentExtent.width != PRESENT_WIDTH || capabilities.currentExtent.height != PRESENT_HEIGHT)) ||
        capabilities.minImageExtent.width > PRESENT_WIDTH || capabilities.minImageExtent.height > PRESENT_HEIGHT ||
        capabilities.maxImageExtent.width < PRESENT_WIDTH || capabilities.maxImageExtent.height < PRESENT_HEIGHT) {
        fprintf(stderr, "A fixed %dx%d framebuffer is required for %dx presentation.\n",
                PRESENT_WIDTH, PRESENT_HEIGHT, PRESENT_SCALE);
        return 0;
    }

    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !(capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        fprintf(stderr, "The surface must support transfer destinations and opaque presentation.\n");
        return 0;
    }

    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount) image_count = capabilities.maxImageCount;

    info.surface = r->surface;
    info.minImageCount = image_count;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = s->extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (r->graphics_family != r->present_family) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    }
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;

    if (!vk_ok(vkCreateSwapchainKHR(r->device, &info, NULL, &s->handle), "Create swapchain")) goto done;
    if (!vk_ok(vkGetSwapchainImagesKHR(r->device, s->handle, &image_count, NULL), "Get swapchain image count")) goto done;
    if (!image_count) goto done;

    images = calloc(image_count, sizeof(*images));
    s->images = calloc(image_count, sizeof(*s->images));
    if (!images || !s->images) goto done;

    s->count = image_count;
    if (!vk_ok(vkGetSwapchainImagesKHR(r->device, s->handle, &image_count, images), "Get swapchain images")) goto done;
    s->count = image_count;

    for (uint32_t i = 0; i < s->count; ++i) {
        VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        s->images[i].handle = images[i];
        if (!vk_ok(vkCreateSemaphore(r->device, &semaphore, NULL, &s->images[i].finished), "Create present semaphore")) goto done;
    }

    fprintf(stderr, "Render target: %dx%d; presentation: %ux%u (%dx nearest).\n",
            RENDER_WIDTH, RENDER_HEIGHT, s->extent.width, s->extent.height, PRESENT_SCALE);
    success = 1;

done:
    free(images);

    return success;
}

static void destroy_swapchain(Renderer *r)
{
    Swapchain *s = &r->swapchain;

    for (uint32_t i = 0; i < s->count; ++i)
        if (s->images[i].finished) vkDestroySemaphore(r->device, s->images[i].finished, NULL);
    if (s->handle) vkDestroySwapchainKHR(r->device, s->handle, NULL);

    free(s->images);
    memset(s, 0, sizeof(*s));
}

static int wait_for_framebuffer(Renderer *r)
{
    int width, height;

    glfwGetFramebufferSize(r->window, &width, &height);
    while (!width || !height) {
        if (glfwWindowShouldClose(r->window)) return 0;

        glfwWaitEvents();
        glfwGetFramebufferSize(r->window, &width, &height);
    }

    return !glfwWindowShouldClose(r->window);
}

static int recreate_swapchain(Renderer *r)
{
    if (!wait_for_framebuffer(r)) return 1;
    VK_TRY(vkDeviceWaitIdle(r->device));

    destroy_swapchain(r);
    r->surface_changed = false;

    return create_swapchain(r);
}

static int create_frames(Renderer *r)
{
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = r->graphics_family
    };

    VK_TRY(vkCreateCommandPool(r->device, &pool, NULL, &r->command_pool));

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        Frame *frame = &r->frames[i];
        VkCommandBufferAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = r->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1
        };
        VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };

        VK_TRY(vkAllocateCommandBuffers(r->device, &allocation, &frame->command));
        VK_TRY(vkCreateSemaphore(r->device, &semaphore, NULL, &frame->acquired));
        VK_TRY(vkCreateFence(r->device, &fence, NULL, &frame->fence));

        if (!create_image(r, RENDER_WIDTH, RENDER_HEIGHT, CANVAS_FORMAT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &frame->canvas)) return 0;
    }

    return 1;
}

static int record_frame(Renderer *r, Frame *frame, uint32_t image_index)
{
    VkCommandBuffer command = frame->command;
    VkImage canvas = frame->canvas.handle;
    VkImage destination = r->swapchain.images[image_index].handle;

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = frame->canvas.view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.01f, 0.01f, 0.015f, 1.0f } } }
    };
    VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { RENDER_WIDTH, RENDER_HEIGHT } }, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &color
    };

    VkDescriptorImageInfo sprite_image = {
        .sampler = r->sampler, .imageView = r->texture.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkWriteDescriptorSet sprite_descriptor = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sprite_image
    };

    VkViewport viewport = { 0, 0, RENDER_WIDTH, RENDER_HEIGHT, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { RENDER_WIDTH, RENDER_HEIGHT } };
    SpriteConstants sprite = {
        { (RENDER_WIDTH - SPRITE_SIZE) / 2.0f, (RENDER_HEIGHT - SPRITE_SIZE) / 2.0f, SPRITE_SIZE, SPRITE_SIZE },
        { RENDER_WIDTH, RENDER_HEIGHT }
    };

    VkImageBlit2 region = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { RENDER_WIDTH, RENDER_HEIGHT, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { PRESENT_WIDTH, PRESENT_HEIGHT, 1 } }
    };
    VkBlitImageInfo2 blit = {
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = canvas, .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = destination, .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1, .pRegions = &region, .filter = VK_FILTER_NEAREST
    };

    VK_TRY(vkResetCommandBuffer(command, 0));
    VK_TRY(vkBeginCommandBuffer(command, &begin));

    image_barrier(command, canvas,
                  frame->canvas_initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  frame->canvas_initialized ? VK_PIPELINE_STAGE_2_BLIT_BIT : VK_PIPELINE_STAGE_2_NONE,
                  frame->canvas_initialized ? VK_ACCESS_2_TRANSFER_READ_BIT : 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    vkCmdBeginRendering(command, &rendering);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);

    vkCmdPushDescriptorSet(command, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline_layout, 0, 1, &sprite_descriptor);
    vkCmdPushConstants(command, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(sprite), &sprite);
    vkCmdDraw(command, 6, 1, 0, 0);
    vkCmdEndRendering(command);

    image_barrier(command, canvas, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    /* Chain the layout transition after the acquire semaphore's blit-stage wait. */
    image_barrier(command, destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    vkCmdBlitImage2(command, &blit);

    image_barrier(command, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);

    VK_TRY(vkEndCommandBuffer(command));
    frame->canvas_initialized = true;

    return 1;
}

Renderer *renderer_create(GLFWwindow *window, int validation)
{
    Renderer *r = calloc(1, sizeof(*r));
    if (!r) return NULL;

    atomic_init(&r->validation_errors, 0);
    r->window = window;

    if (!create_instance(r, validation)) goto fail;
    if (!vk_ok(glfwCreateWindowSurface(r->instance, window, NULL, &r->surface), "Create window surface")) goto fail;

    if (!create_device(r) || !create_frames(r) ||
        !create_texture(r) || !create_descriptors(r) || !create_pipeline(r) || !wait_for_framebuffer(r) ||
        !create_swapchain(r)) goto fail;

    return r;

fail:
    fprintf(stderr, "Failed to initialize the Vulkan renderer.\n");
    renderer_destroy(r);
    return NULL;
}

void renderer_surface_changed(Renderer *r)
{
    if (r) r->surface_changed = true;
}

int renderer_draw(Renderer *r)
{
    Frame *frame = &r->frames[r->frame_index];
    uint32_t image_index;
    VkResult acquired, presented;

    VkSemaphoreSubmitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = frame->acquired,
        .stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT
    };
    VkSemaphoreSubmitInfo signal = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
    };
    VkCommandBufferSubmitInfo command = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = frame->command
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1, .pCommandBufferInfos = &command,
        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal
    };
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .swapchainCount = 1,
        .pSwapchains = &r->swapchain.handle, .pImageIndices = &image_index
    };

    if (!wait_for_framebuffer(r)) return 1;
    if (r->surface_changed && !recreate_swapchain(r)) return 0;

    VK_TRY(vkWaitForFences(r->device, 1, &frame->fence, VK_TRUE, UINT64_MAX));

    acquired = vkAcquireNextImageKHR(r->device, r->swapchain.handle, UINT64_MAX, frame->acquired, VK_NULL_HANDLE, &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) return recreate_swapchain(r);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return vk_ok(acquired, "Acquire swapchain image");

    if (!record_frame(r, frame, image_index)) return 0;
    VK_TRY(vkResetFences(r->device, 1, &frame->fence));
    signal.semaphore = r->swapchain.images[image_index].finished;
    VK_TRY(vkQueueSubmit2(r->graphics_queue, 1, &submit, frame->fence));

    present.pWaitSemaphores = &signal.semaphore;
    presented = vkQueuePresentKHR(r->present_queue, &present);
    r->frame_index = (r->frame_index + 1) % FRAMES_IN_FLIGHT;

    if (presented != VK_SUCCESS && presented != VK_ERROR_OUT_OF_DATE_KHR && presented != VK_SUBOPTIMAL_KHR)
        return vk_ok(presented, "Present swapchain image");
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR || r->surface_changed)
        return recreate_swapchain(r);

    return vk_ok(presented, "Present swapchain image");
}

int renderer_destroy(Renderer *r)
{
    unsigned errors;
    int success = 1;

    if (!r) return 1;

    if (r->device) {
        success = vk_ok(vkDeviceWaitIdle(r->device), "Wait before cleanup");
        destroy_swapchain(r);

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
            destroy_image(r, &r->frames[i].canvas);
            if (r->frames[i].acquired) vkDestroySemaphore(r->device, r->frames[i].acquired, NULL);
            if (r->frames[i].fence) vkDestroyFence(r->device, r->frames[i].fence, NULL);
        }

        if (r->command_pool) vkDestroyCommandPool(r->device, r->command_pool, NULL);

        if (r->pipeline) vkDestroyPipeline(r->device, r->pipeline, NULL);
        if (r->pipeline_layout) vkDestroyPipelineLayout(r->device, r->pipeline_layout, NULL);
        if (r->descriptor_layout) vkDestroyDescriptorSetLayout(r->device, r->descriptor_layout, NULL);

        if (r->sampler) vkDestroySampler(r->device, r->sampler, NULL);
        destroy_image(r, &r->texture);

        vkDestroyDevice(r->device, NULL);
    }

    if (r->surface) vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (r->debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(r->instance, "vkDestroyDebugUtilsMessengerEXT");

        if (destroy_debug) destroy_debug(r->instance, r->debug_messenger, NULL);
    }
    if (r->instance) vkDestroyInstance(r->instance, NULL);

    errors = atomic_load(&r->validation_errors);
    if (errors) fprintf(stderr, "Vulkan validation reported %u error(s).\n", errors);

    free(r);

    return success && errors == 0;
}
