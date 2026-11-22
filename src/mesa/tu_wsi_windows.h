#pragma once
struct tu_device;
struct wsi_device;
void tu_windows_capture_context(windows_context **sink);
VkResult tu_windows_wsi_signal_acquire(VkDevice device,VkFence previous,VkSemaphore semaphore,VkFence ready);
void tu_windows_wsi_init(wsi_device *wsi);
void tu_windows_wsi_finish(tu_device *dev);
const void *VKAPI_CALL tu_windows_get_composition_api();
