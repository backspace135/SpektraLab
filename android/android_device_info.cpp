// android_device_info.cpp — on-device probe for issue #2 stage 0 task 0.3.
//
// Records, on the real Adreno device, the facts the stage-0 risks depend on:
//   R3  deviceName, deviceType, api/driver versions, maxStorageBufferRange,
//       maxComputeWorkGroupInvocations/Size/Count and the storage-descriptor
//       limits the engine checks before it will start
//   R2  the full memory-type table, and specifically whether a DEVICE_LOCAL
//       compute-buffer type exists (the engine's allocator refuses to start
//       with "no device-local Vulkan compute memory type" otherwise — see
//       vulkan_gpu.cpp). On unified-memory Adreno the device-local heap *is*
//       system RAM; whether the driver exposes a DEVICE_LOCAL type at all is
//       exactly what stage 0 must measure.
//   fast-math guard  Gpu::check_math_mode — the analogue of the Metal
//       `-fmetal-math-mode=safe` gate, which must hold on Adreno for the whole
//       parity system (render bar 3e-5) to mean anything here.
//
// Prints one JSON object on stdout so the harness can diff it across devices
// and runs. Exit 0 on success; non-zero when the device cannot run the engine
// (no compute queue, limits below the shader layout, math-mode guard tripped).
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <volk.h>

#include "gpu/gpu.hpp"

namespace {

const char* device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
    }
}

void print_flags_json(VkFlags f) {
    std::printf("{\"device_local\":%s,\"host_visible\":%s,\"host_coherent\":%s,"
        "\"host_cached\":%s,\"lazily_allocated\":%s,\"protected\":%s}",
        (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "true" : "false",
        (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "true" : "false",
        (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "true" : "false",
        (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "true" : "false",
        (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "true" : "false",
        (f & VK_MEMORY_PROPERTY_PROTECTED_BIT) ? "true" : "false");
}

// Enumerate the first physical device's properties + memory model on a
// throwaway instance (the engine keeps its own). Prints the R3 limits and the
// R2 memory table. Returns false when no usable device exists.
bool print_device_and_memory() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "spk_android_device_info";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (volkInitialize() != VK_SUCCESS ||
        vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) {
        std::printf("\"enumerate_error\":\"no instance\"");
        return false;
    }
    volkLoadInstance(instance);
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        std::printf("\"enumerate_error\":\"no physical device\"");
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(devices[0], &p);
    const VkPhysicalDeviceLimits& l = p.limits;
    std::printf("\"device_name\":\"%s\",", p.deviceName);
    std::printf("\"device_type\":\"%s\",", device_type_name(p.deviceType));
    std::printf("\"vendor_id\":%u,\"device_id\":%u,", p.vendorID, p.deviceID);
    std::printf("\"api_version\":\"%u.%u.%u\",",
        VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
        VK_API_VERSION_PATCH(p.apiVersion));
    std::printf("\"driver_version\":%u,", p.driverVersion);
    std::printf("\"maxStorageBufferRange\":%u,", l.maxStorageBufferRange);
    std::printf("\"maxComputeWorkGroupInvocations\":%u,", l.maxComputeWorkGroupInvocations);
    std::printf("\"maxComputeWorkGroupSize\":[%u,%u,%u],",
        l.maxComputeWorkGroupSize[0], l.maxComputeWorkGroupSize[1], l.maxComputeWorkGroupSize[2]);
    std::printf("\"maxComputeWorkGroupCount\":[%u,%u,%u],",
        l.maxComputeWorkGroupCount[0], l.maxComputeWorkGroupCount[1], l.maxComputeWorkGroupCount[2]);
    std::printf("\"maxPerStageDescriptorStorageBuffers\":%u,", l.maxPerStageDescriptorStorageBuffers);
    std::printf("\"maxDescriptorSetStorageBuffers\":%u,", l.maxDescriptorSetStorageBuffers);
    std::printf("\"minStorageBufferOffsetAlignment\":%" PRIu64 ",",
        (uint64_t)l.minStorageBufferOffsetAlignment);
    std::printf("\"maxImageDimension2D\":%u,", l.maxImageDimension2D);

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(devices[0], &mem);
    std::printf("\"memory_heaps\":[");
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        std::printf("%s{\"index\":%u,\"size\":%" PRIu64 ",\"device_local\":%s}",
            i ? "," : "", i, (uint64_t)mem.memoryHeaps[i].size,
            (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "true" : "false");
    }
    std::printf("],\"memory_types\":[");
    bool any_device_local = false;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        const VkFlags f = mem.memoryTypes[i].propertyFlags;
        any_device_local = any_device_local || (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        std::printf("%s{\"index\":%u,\"heap\":%u,\"flags\":", i ? "," : "", i,
            mem.memoryTypes[i].heapIndex);
        print_flags_json(f);
        std::printf("}");
    }
    std::printf("],\"has_device_local_compute_type\":%s,",
        any_device_local ? "true" : "false");
    vkDestroyInstance(instance, nullptr);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: spk_android_device_info <resources>\n");
        return 2;
    }
    const std::string resources = argv[1];
    std::string error;

    std::printf("{");
    const bool enumerated = print_device_and_memory();

    // The engine itself, and the fast-math guard that gates stage-0 R1.
    spk::gpu::Gpu* gpu = spk::gpu::Gpu::create_vulkan(resources + "/vulkan", error);
    if (!gpu) {
        std::printf("\"engine_create_ok\":false,\"create_error\":\"%s\",\"ok\":false}\n",
            error.c_str());
        std::fprintf(stderr, "device: %s\n", error.c_str());
        return 1;
    }
    std::string math;
    const bool math_ok = gpu->check_math_mode(math);
    std::printf("\"engine_create_ok\":true,");
    std::printf("\"engine_device_name\":\"%s\",", gpu->device_name().c_str());
    std::printf("\"math_mode_ok\":%s,\"math_mode_detail\":\"%s\",",
        math_ok ? "true" : "false", math.c_str());
    std::printf("\"ok\":%s}\n", (enumerated && math_ok) ? "true" : "false");

    delete gpu;
    return (enumerated && math_ok) ? 0 : 1;
}
