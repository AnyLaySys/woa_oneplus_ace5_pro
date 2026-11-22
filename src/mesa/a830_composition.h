#pragma once

/* Private, process-local interface between the DXGI runtime and this ICD.
 * DXGI objects remain native Windows objects; only their GPU allocations are
 * imported. No application or public Vulkan ABI depends on this interface.
 */
#define A830_COMPOSITION_VERSION 1
struct A830_COMPOSITION_IMAGE {
   VkImage image;
   VkDeviceMemory memory;
   void *data;
};
struct A830_COMPOSITION_API {
   unsigned version, size;
   HRESULT (WINAPI *create)(VkDevice, const void *, void **);
   VkResult (WINAPI *open_image)(VkDevice, void *, unsigned, A830_COMPOSITION_IMAGE *);
   void (WINAPI *close_image)(VkDevice, A830_COMPOSITION_IMAGE *);
   VkResult (WINAPI *prepare)(VkDevice, void *, void *, unsigned *);
   VkResult (WINAPI *ready)(VkDevice);
   void (WINAPI *finish)(VkDevice);
};
typedef const A830_COMPOSITION_API *(VKAPI_PTR *A830_GET_COMPOSITION_API)();
