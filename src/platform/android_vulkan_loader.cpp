// Vulkan-loader bridge for rootless per-app Adreno drivers.
//
// SDL expects to dlopen a library exporting vkGetInstanceProcAddr. AdrenoTools
// instead returns an already-open, namespace-isolated libvulkan handle. This
// tiny bridge joins those two APIs: SDL loads us, while our exported entry
// point forwards into the handle prepared by AdrenoTools.

#if defined(__ANDROID__)

#include <adrenotools/driver.h>
#include <adrenotools/priv.h>

#include <dlfcn.h>

#include <mutex>
#include <string>

namespace {

using GetInstanceProcAddress = void* (*)(void*, const char*);

std::mutex g_config_mutex;
std::string g_private_directory;
std::string g_native_library_directory;
std::string g_driver_directory;
std::string g_driver_library;
void* g_vulkan_handle = nullptr;
GetInstanceProcAddress g_get_instance_proc_address = nullptr;
bool g_open_attempted = false;

void open_driver_once() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    if (g_open_attempted) return;
    g_open_attempted = true;
    if (g_private_directory.empty() || g_native_library_directory.empty() ||
        g_driver_directory.empty() || g_driver_library.empty()) {
        return;
    }

    g_vulkan_handle = adrenotools_open_libvulkan(
        RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
        g_private_directory.c_str(), g_native_library_directory.c_str(),
        g_driver_directory.c_str(), g_driver_library.c_str(), nullptr, nullptr);
    if (g_vulkan_handle != nullptr) {
        g_get_instance_proc_address = reinterpret_cast<GetInstanceProcAddress>(
            dlsym(g_vulkan_handle, "vkGetInstanceProcAddr"));
    }
}

}  // namespace

extern "C" bool retro3do_configure_vulkan_driver(
    const char* private_directory, const char* native_library_directory,
    const char* driver_directory, const char* driver_library) {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    if (g_open_attempted || private_directory == nullptr ||
        native_library_directory == nullptr || driver_directory == nullptr ||
        driver_library == nullptr) {
        return false;
    }
    g_private_directory = private_directory;
    g_native_library_directory = native_library_directory;
    g_driver_directory = driver_directory;
    g_driver_library = driver_library;
    return !g_private_directory.empty() && !g_native_library_directory.empty() &&
           !g_driver_directory.empty() && !g_driver_library.empty();
}

// Vulkan types intentionally remain opaque here. On Android/ARM64 these match
// VkInstance and PFN_vkVoidFunction exactly, while avoiding a second Vulkan
// header/loader dependency in the application.
extern "C" __attribute__((visibility("default"))) void* vkGetInstanceProcAddr(
    void* instance, const char* name) {
    open_driver_once();
    return g_get_instance_proc_address != nullptr
               ? g_get_instance_proc_address(instance, name)
               : nullptr;
}

#endif
