#define GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <stb_image.h>
#include "renderer.h"

#include <stdatomic.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES_IN_FLIGHT 2
#define SPRITE_SIZE 32
#define MIP_LEVELS 6
#define VALIDATION_LAYER "VK_LAYER_KHRONOS_validation"
#define VK_TRY(call) do { if (!vk_ok((call), #call)) return 0; } while (0)

typedef struct Buffer {
    VkBuffer handle;
    VkDeviceMemory memory;
} Buffer;

typedef struct Frame {
    VkCommandBuffer command;
    VkSemaphore acquired;
    VkFence fence;
} Frame;

typedef struct SwapchainImage {
    VkImage handle;
    VkImageView view;
    VkFramebuffer framebuffer;
    /* Presentation can outlive a frame's submit fence. Reuse by image index. */
    VkSemaphore finished;
} SwapchainImage;

typedef struct Swapchain {
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    uint32_t count;
    SwapchainImage *images;
    VkRenderPass render_pass;
    VkPipeline pipeline;
} Swapchain;

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
    Buffer vertices, indices, models;
    VkImage texture;
    VkDeviceMemory texture_memory;
    VkImageView texture_view;
    VkSampler sampler;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout pipeline_layout;
    Swapchain swapchain;
    Frame frames[FRAMES_IN_FLIGHT];
    uint32_t frame_index;
    int resized;
    atomic_uint validation_errors;
};

static const float identity[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
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
    app.apiVersion = VK_API_VERSION_1_0;
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
        uint32_t graphics, present, formats = 0, modes = 0;
        VkPhysicalDeviceProperties properties;
        VkFormatProperties texture_format;
        VkSurfaceCapabilitiesKHR capabilities;
        int score;
        if (!device_has_extension(devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
            !queue_families(r, devices[i], &graphics, &present)) continue;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(devices[i], r->surface, &capabilities) != VK_SUCCESS ||
            !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) continue;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(devices[i], r->surface, &formats, NULL) != VK_SUCCESS || !formats) continue;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(devices[i], r->surface, &modes, NULL) != VK_SUCCESS || !modes) continue;
        vkGetPhysicalDeviceFormatProperties(devices[i], VK_FORMAT_R8G8B8A8_UNORM, &texture_format);
        if (!(texture_format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
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
        fprintf(stderr, "No Vulkan device supports graphics and presentation to this window.\n");
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
        const char *extensions[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset" };
        VkDeviceCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
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
        fprintf(stderr, "Vulkan device: %s\n", properties.deviceName);
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
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command
    };
    int success = vk_ok(vkEndCommandBuffer(command), "End upload");
    if (success) success = vk_ok(vkQueueSubmit(r->graphics_queue, 1, &submit, VK_NULL_HANDLE), "Submit upload");
    if (success) success = vk_ok(vkQueueWaitIdle(r->graphics_queue), "Wait for upload");
    vkFreeCommandBuffers(r->device, r->command_pool, 1, &command);
    return success;
}

static int upload_buffer(Renderer *r, const void *data, VkDeviceSize size,
                         VkBufferUsageFlags usage, Buffer *buffer)
{
    Buffer staging = {0};
    VkCommandBuffer command;
    VkBufferCopy copy = { .size = size };
    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ?
                         VK_ACCESS_INDEX_READ_BIT : VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .offset = 0, .size = VK_WHOLE_SIZE
    };
    int success = 0;
    if (!create_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) goto done;
    if (!fill_buffer(r, &staging, data, size)) goto done;
    if (!create_buffer(r, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer)) goto done;
    if (!begin_upload(r, &command)) goto done;
    vkCmdCopyBuffer(command, staging.handle, buffer->handle, 1, &copy);
    barrier.buffer = buffer->handle;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 0, NULL, 1, &barrier, 0, NULL);
    success = end_upload(r, command);
done:
    destroy_buffer(r, &staging);
    return success;
}

static int create_geometry(Renderer *r)
{
    const float vertices[] = {
        /* x, y, z, u, v; same quad and instance matrix as the OpenGL version. */
        -1,  1, 0, 0, 0,
         1,  1, 0, 1, 0,
        -1, -1, 0, 0, 1,
         1, -1, 0, 1, 1
    };
    const uint32_t indices[] = { 0, 2, 1, 1, 2, 3 };
    return upload_buffer(r, vertices, sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &r->vertices) &&
           upload_buffer(r, indices, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &r->indices) &&
           upload_buffer(r, identity, sizeof(identity), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &r->models);
}

static int create_texture(Renderer *r)
{
    unsigned char *pixels = NULL, *mips = NULL;
    int width, height, channels, success = 0;
    size_t mip_bytes = 0;
    VkBufferImageCopy copies[MIP_LEVELS] = {{0}};
    Buffer staging = {0};
    VkCommandBuffer command;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { SPRITE_SIZE, SPRITE_SIZE, 1 },
        .mipLevels = MIP_LEVELS, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, MIP_LEVELS, 0, 1 }
    };
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, MIP_LEVELS, 0, 1 }
    };
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .minLod = 0, .maxLod = MIP_LEVELS - 1
    };
    const char *path = RETROPIXELS_ASSETS_DIR "/avatar-1x.png";
    pixels = stbi_load(path, &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        fprintf(stderr, "Failed to load %s: %s\n", path, stbi_failure_reason());
        goto done;
    }
    if (width != SPRITE_SIZE || height != SPRITE_SIZE) {
        fprintf(stderr, "Sprite must be %dx%d; got %dx%d.\n", SPRITE_SIZE, SPRITE_SIZE, width, height);
        goto done;
    }
    for (uint32_t level = 0, size = SPRITE_SIZE; level < MIP_LEVELS; ++level, size >>= 1) {
        copies[level].bufferOffset = mip_bytes;
        copies[level].imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        copies[level].imageExtent = (VkExtent3D){ size, size, 1 };
        mip_bytes += (size_t)size * size * 4;
    }
    mips = malloc(mip_bytes);
    if (!mips) goto done;
    memcpy(mips, pixels, SPRITE_SIZE * SPRITE_SIZE * 4);
    /* This tiny texture can be mipmapped on the CPU without requiring blit support. */
    for (uint32_t level = 1; level < MIP_LEVELS; ++level) {
        uint32_t size = copies[level].imageExtent.width;
        const unsigned char *source = mips + copies[level - 1].bufferOffset;
        unsigned char *destination = mips + copies[level].bufferOffset;
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x)
                for (uint32_t c = 0; c < 4; ++c) {
                    size_t top = ((size_t)(2 * y) * (2 * size) + 2 * x) * 4 + c;
                    size_t bottom = top + 2 * size * 4;
                    destination[((size_t)y * size + x) * 4 + c] = (unsigned char)(
                        (source[top] + source[top + 4] + source[bottom] + source[bottom + 4] + 2) / 4);
                }
    }
    if (!create_buffer(r, mip_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) goto done;
    if (!fill_buffer(r, &staging, mips, mip_bytes)) goto done;
    if (!vk_ok(vkCreateImage(r->device, &image, NULL, &r->texture), "Create texture")) goto done;
    vkGetImageMemoryRequirements(r->device, r->texture, &requirements);
    allocation.allocationSize = requirements.size;
    if (!memory_type(r, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &allocation.memoryTypeIndex)) goto done;
    if (!vk_ok(vkAllocateMemory(r->device, &allocation, NULL, &r->texture_memory), "Allocate texture memory")) goto done;
    if (!vk_ok(vkBindImageMemory(r->device, r->texture, r->texture_memory, 0), "Bind texture memory")) goto done;
    if (!begin_upload(r, &command)) goto done;
    barrier.image = r->texture;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    vkCmdCopyBufferToImage(command, staging.handle, r->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MIP_LEVELS, copies);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    if (!end_upload(r, command)) goto done;
    view.image = r->texture;
    if (!vk_ok(vkCreateImageView(r->device, &view, NULL, &r->texture_view), "Create texture view")) goto done;
    if (!vk_ok(vkCreateSampler(r->device, &sampler, NULL, &r->sampler), "Create texture sampler")) goto done;
    success = 1;
done:
    destroy_buffer(r, &staging);
    stbi_image_free(pixels);
    free(mips);
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
        .bindingCount = 1, .pBindings = &binding
    };
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size
    };
    VkDescriptorSetAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorSetCount = 1, .pSetLayouts = &r->descriptor_layout
    };
    VkDescriptorImageInfo image = {
        .sampler = r->sampler, .imageView = r->texture_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image
    };
    VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(identity) };
    VkPipelineLayoutCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &r->descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push
    };
    VK_TRY(vkCreateDescriptorSetLayout(r->device, &layout, NULL, &r->descriptor_layout));
    VK_TRY(vkCreateDescriptorPool(r->device, &pool, NULL, &r->descriptor_pool));
    allocation.descriptorPool = r->descriptor_pool;
    VK_TRY(vkAllocateDescriptorSets(r->device, &allocation, &r->descriptor_set));
    write.dstSet = r->descriptor_set;
    vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
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

static int format_is_srgb(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

static int create_render_pass(Renderer *r)
{
    Swapchain *s = &r->swapchain;
    VkAttachmentDescription color = {
        .format = s->format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference
    };
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    };
    VkRenderPassCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &color,
        .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 1, .pDependencies = &dependency
    };
    VK_TRY(vkCreateRenderPass(r->device, &info, NULL, &s->render_pass));
    return 1;
}

static int create_pipeline(Renderer *r)
{
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkVertexInputBindingDescription bindings[] = {
        { 0, 5 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, 16 * sizeof(float), VK_VERTEX_INPUT_RATE_INSTANCE }
    };
    VkVertexInputAttributeDescription attributes[] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, 3 * sizeof(float) },
        { 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float) },
        { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 8 * sizeof(float) },
        { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 12 * sizeof(float) }
    };
    VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = bindings,
        .vertexAttributeDescriptionCount = 6, .pVertexAttributeDescriptions = attributes
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = VK_FALSE,
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
    VkBool32 srgb = format_is_srgb(r->swapchain.format);
    VkSpecializationMapEntry entry = { 0, 0, sizeof(srgb) };
    VkSpecializationInfo specialization = {
        .mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof(srgb), .pData = &srgb
    };
    VkPipelineShaderStageCreateInfo stages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main", .pSpecializationInfo = &specialization }
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &input, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &raster,
        .pMultisampleState = &multisample, .pColorBlendState = &blend, .pDynamicState = &dynamic,
        .layout = r->pipeline_layout, .renderPass = r->swapchain.render_pass, .subpass = 0
    };
    int success = 0;
    if (!load_shader(r, RETROPIXELS_SHADERS_DIR "/sprite.vert.spv", &vertex)) goto done;
    if (!load_shader(r, RETROPIXELS_SHADERS_DIR "/sprite.frag.spv", &fragment)) goto done;
    stages[0].module = vertex;
    stages[1].module = fragment;
    success = vk_ok(vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &pipeline, NULL,
                                             &r->swapchain.pipeline), "Create sprite pipeline");
done:
    if (fragment) vkDestroyShaderModule(r->device, fragment, NULL);
    if (vertex) vkDestroyShaderModule(r->device, vertex, NULL);
    return success;
}

static uint32_t clamp_extent(uint32_t value, uint32_t min, uint32_t max)
{
    return value < min ? min : value > max ? max : value;
}

static VkSurfaceFormatKHR choose_surface_format(const VkSurfaceFormatKHR *formats, uint32_t count)
{
    const VkFormat preferences[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB };
    /* UNORM preserves the original OpenGL framebuffer's encoded colors. */
    if (count == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        return (VkSurfaceFormatKHR){ preferences[0], VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    for (size_t p = 0; p < sizeof(preferences) / sizeof(preferences[0]); ++p)
        for (uint32_t i = 0; i < count; ++i)
            if (formats[i].format == preferences[p] && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return formats[i];
    return formats[0];
}

static int create_swapchain(Renderer *r)
{
    Swapchain *s = &r->swapchain;
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR *formats = NULL, format;
    VkImage *images = NULL;
    uint32_t format_count = 0, image_count;
    uint32_t families[] = { r->graphics_family, r->present_family };
    VkSwapchainCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };
    int width, height, success = 0;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical_device, r->surface, &capabilities));
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical_device, r->surface, &format_count, NULL));
    if (!format_count) return 0;
    formats = calloc(format_count, sizeof(*formats));
    if (!formats) goto done;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical_device, r->surface, &format_count, formats), "Query surface formats")) goto done;
    format = choose_surface_format(formats, format_count);
    s->format = format.format;
    glfwGetFramebufferSize(r->window, &width, &height);
    if (capabilities.currentExtent.width != UINT32_MAX) s->extent = capabilities.currentExtent;
    else {
        s->extent.width = clamp_extent((uint32_t)width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        s->extent.height = clamp_extent((uint32_t)height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount) image_count = capabilities.maxImageCount;
    info.surface = r->surface;
    info.minImageCount = image_count;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = s->extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (r->graphics_family != r->present_family) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    }
    info.preTransform = capabilities.currentTransform;
    for (size_t i = 0; i < sizeof(alphas) / sizeof(alphas[0]); ++i)
        if (capabilities.supportedCompositeAlpha & alphas[i]) {
            info.compositeAlpha = alphas[i];
            break;
        }
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; /* Guaranteed, vsync like glfwSwapInterval(1). */
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
    if (!create_render_pass(r) || !create_pipeline(r)) goto done;
    for (uint32_t i = 0; i < s->count; ++i) {
        SwapchainImage *image = &s->images[i];
        VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = s->format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        VkFramebufferCreateInfo framebuffer = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = s->render_pass, .attachmentCount = 1, .pAttachments = &image->view,
            .width = s->extent.width, .height = s->extent.height, .layers = 1
        };
        VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        image->handle = images[i];
        if (!vk_ok(vkCreateImageView(r->device, &view, NULL, &image->view), "Create swapchain image view")) goto done;
        if (!vk_ok(vkCreateFramebuffer(r->device, &framebuffer, NULL, &image->framebuffer), "Create framebuffer")) goto done;
        if (!vk_ok(vkCreateSemaphore(r->device, &semaphore, NULL, &image->finished), "Create present semaphore")) goto done;
    }
    success = 1;
done:
    free(images);
    free(formats);
    return success;
}

static void destroy_swapchain(Renderer *r)
{
    Swapchain *s = &r->swapchain;
    for (uint32_t i = 0; i < s->count; ++i) {
        if (s->images[i].framebuffer) vkDestroyFramebuffer(r->device, s->images[i].framebuffer, NULL);
        if (s->images[i].view) vkDestroyImageView(r->device, s->images[i].view, NULL);
        if (s->images[i].finished) vkDestroySemaphore(r->device, s->images[i].finished, NULL);
    }
    if (s->pipeline) vkDestroyPipeline(r->device, s->pipeline, NULL);
    if (s->render_pass) vkDestroyRenderPass(r->device, s->render_pass, NULL);
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
    r->resized = 0;
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
    }
    return 1;
}

static float srgb_to_linear(float value)
{
    return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

static int record_frame(Renderer *r, VkCommandBuffer command, uint32_t image_index)
{
    Swapchain *s = &r->swapchain;
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkClearValue clear = { .color = { .float32 = { 0.08f, 0.08f, 0.10f, 1 } } };
    VkRenderPassBeginInfo render = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = s->render_pass, .framebuffer = s->images[image_index].framebuffer,
        .renderArea = { { 0, 0 }, s->extent }, .clearValueCount = 1, .pClearValues = &clear
    };
    VkViewport viewport = { 0, 0, (float)s->extent.width, (float)s->extent.height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, s->extent };
    VkBuffer buffers[] = { r->vertices.handle, r->models.handle };
    VkDeviceSize offsets[] = { 0, 0 };
    if (format_is_srgb(s->format))
        for (int i = 0; i < 3; ++i) clear.color.float32[i] = srgb_to_linear(clear.color.float32[i]);
    VK_TRY(vkResetCommandBuffer(command, 0));
    VK_TRY(vkBeginCommandBuffer(command, &begin));
    vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindVertexBuffers(command, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(command, r->indices.handle, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline_layout, 0, 1, &r->descriptor_set, 0, NULL);
    vkCmdPushConstants(command, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(identity), identity);
    vkCmdDrawIndexed(command, 6, 1, 0, 0, 0);
    vkCmdEndRenderPass(command);
    VK_TRY(vkEndCommandBuffer(command));
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
    if (!create_device(r) || !create_frames(r) || !create_geometry(r) ||
        !create_texture(r) || !create_descriptors(r) || !wait_for_framebuffer(r) ||
        !create_swapchain(r)) goto fail;
    return r;
fail:
    fprintf(stderr, "Failed to initialize the Vulkan renderer.\n");
    renderer_destroy(r);
    return NULL;
}

void renderer_resize(Renderer *r)
{
    if (r) r->resized = 1;
}

int renderer_draw(Renderer *r)
{
    Frame *frame = &r->frames[r->frame_index];
    uint32_t image_index;
    VkResult acquired, presented;
    VkSemaphore finished;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &frame->acquired, .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &frame->command, .signalSemaphoreCount = 1
    };
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .swapchainCount = 1,
        .pSwapchains = &r->swapchain.handle, .pImageIndices = &image_index
    };
    if (!wait_for_framebuffer(r)) return 1;
    if (r->resized && !recreate_swapchain(r)) return 0;
    VK_TRY(vkWaitForFences(r->device, 1, &frame->fence, VK_TRUE, UINT64_MAX));
    acquired = vkAcquireNextImageKHR(r->device, r->swapchain.handle, UINT64_MAX, frame->acquired, VK_NULL_HANDLE, &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) return recreate_swapchain(r);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return vk_ok(acquired, "Acquire swapchain image");
    if (!record_frame(r, frame->command, image_index)) return 0;
    /* Reset only after acquisition succeeds, so an out-of-date swapchain cannot strand the fence. */
    VK_TRY(vkResetFences(r->device, 1, &frame->fence));
    finished = r->swapchain.images[image_index].finished;
    submit.pSignalSemaphores = &finished;
    VK_TRY(vkQueueSubmit(r->graphics_queue, 1, &submit, frame->fence));
    present.pWaitSemaphores = &finished;
    presented = vkQueuePresentKHR(r->present_queue, &present);
    r->frame_index = (r->frame_index + 1) % FRAMES_IN_FLIGHT;
    if (presented != VK_SUCCESS && presented != VK_ERROR_OUT_OF_DATE_KHR && presented != VK_SUBOPTIMAL_KHR)
        return vk_ok(presented, "Present swapchain image");
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR || r->resized)
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
            if (r->frames[i].acquired) vkDestroySemaphore(r->device, r->frames[i].acquired, NULL);
            if (r->frames[i].fence) vkDestroyFence(r->device, r->frames[i].fence, NULL);
        }
        if (r->command_pool) vkDestroyCommandPool(r->device, r->command_pool, NULL);
        if (r->pipeline_layout) vkDestroyPipelineLayout(r->device, r->pipeline_layout, NULL);
        if (r->descriptor_pool) vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
        if (r->descriptor_layout) vkDestroyDescriptorSetLayout(r->device, r->descriptor_layout, NULL);
        if (r->sampler) vkDestroySampler(r->device, r->sampler, NULL);
        if (r->texture_view) vkDestroyImageView(r->device, r->texture_view, NULL);
        if (r->texture) vkDestroyImage(r->device, r->texture, NULL);
        if (r->texture_memory) vkFreeMemory(r->device, r->texture_memory, NULL);
        destroy_buffer(r, &r->models);
        destroy_buffer(r, &r->indices);
        destroy_buffer(r, &r->vertices);
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
