/*
 * Copyright © 2017 Google
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <vulkan/vk_layer.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <unordered_map>

//namespace {

struct instance_info {
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
	PFN_GetPhysicalDeviceProcAddr  GetPhysicalDeviceProcAddr;
	PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
};

std::unordered_map<VkInstance, struct instance_info> instances;
VkResult CreateInstance(
        const VkInstanceCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkInstance *pInstance)
{
   VkLayerInstanceCreateInfo *chain_info;
   for(chain_info = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext; chain_info; chain_info = (VkLayerInstanceCreateInfo*)chain_info->pNext)
	   if(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain_info->function == VK_LAYER_LINK_INFO)
		   break;

    assert(chain_info->u.pLayerInfo);
    struct instance_info info = {0};
    info.GetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    info.GetPhysicalDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    PFN_vkCreateInstance fpCreateInstance =
        (PFN_vkCreateInstance)info.GetInstanceProcAddr(NULL, "vkCreateInstance");
    if (fpCreateInstance == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS)
        return result;



    info.DestroyInstance = (PFN_vkDestroyInstance)info.GetInstanceProcAddr(*pInstance, "vkDestroyInstance");
    info.EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)info.GetInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDevices");
    info.GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)info.GetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceProperties");

    instances[*pInstance] = info;
    return VK_SUCCESS;
}

void DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
	auto info = instances[instance];
	instances.erase(instances.find(instance));
	info.DestroyInstance(instance, pAllocator);
}


void print_gpu(const instance_info& info, unsigned index, VkPhysicalDevice device) {
	const char* type = "";

	VkPhysicalDeviceProperties properties;
	info.GetPhysicalDeviceProperties(device, &properties);

	switch(properties.deviceType) {
		case VK_PHYSICAL_DEVICE_TYPE_OTHER:
			type = "other";
			break;
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
			type = "integrated GPU";
			break;
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			type = "discrete GPU";
			break;
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
			type = "virtual GPU";
			break;
		case VK_PHYSICAL_DEVICE_TYPE_CPU:
			type = "CPU";
			break;
	}
	fprintf(stderr, "  GPU %d: %x:%x \"%s\" %s\n", index, properties.vendorID, properties.deviceID, properties.deviceName, type);
}

VkResult device_select_EnumeratePhysicalDevices(VkInstance instance,
                                  uint32_t* pPhysicalDeviceCount,
                                  VkPhysicalDevice *pPhysicalDevices)
{
	auto info = instances[instance];
	uint32_t physical_device_count = 0;
	uint32_t selected_physical_device_count = 0;
	const char* selection = getenv("MESA_VK_DEVICE_SELECT");
	VkResult result = info.EnumeratePhysicalDevices(instance, &physical_device_count, nullptr);
	if (result != VK_SUCCESS)
		return result;

	VkPhysicalDevice *physical_devices = (VkPhysicalDevice*)calloc(sizeof(VkPhysicalDevice),  physical_device_count);
	VkPhysicalDevice *selected_physical_devices = (VkPhysicalDevice*)calloc(sizeof(VkPhysicalDevice),
							     physical_device_count);
	if (!physical_devices || !selected_physical_devices) {
		result = VK_ERROR_OUT_OF_HOST_MEMORY;
		goto out;
	}

	result = info.EnumeratePhysicalDevices(instance, &physical_device_count, physical_devices);
	if (result != VK_SUCCESS)
		goto out;

	if (selection && strcmp(selection, "list") == 0) {
		fprintf(stderr, "selectable devices:\n");
		for (unsigned i = 0; i < physical_device_count; ++i)
			print_gpu(info, i, physical_devices[i]);
		exit(0);
	} else if (selection) {
		unsigned vendor_id, device_id;
		int matched = sscanf(selection, "%x:%x", &vendor_id, &device_id);
		if (matched != 2) {
			fprintf(stderr, "failed to parse MESA_VK_DEVICE_SELECT: \"%s\"\n", selection);
			exit(1);
		}
		VkPhysicalDeviceProperties properties;
		for (unsigned i = 0; i < physical_device_count; ++i) {
			info.GetPhysicalDeviceProperties(physical_devices[i], &properties);
			if (properties.vendorID == vendor_id &&
			    properties.deviceID == device_id) {
				selected_physical_devices[selected_physical_device_count++] = physical_devices[i];
			}
		}
	} else {
		selected_physical_device_count = physical_device_count;
		for (unsigned i = 0; i < physical_device_count; ++i)
			selected_physical_devices[i] = physical_devices[i];
	}

	if (selected_physical_device_count == 0) {
             fprintf(stderr, "WARNING: selected no devices with MESA_VK_DEVICE_SELECT\n");
        }

	assert(result == VK_SUCCESS);
	if (!pPhysicalDevices) {
		*pPhysicalDeviceCount = selected_physical_device_count;
	} else {
		if (selected_physical_device_count < *pPhysicalDeviceCount) {
			*pPhysicalDeviceCount = selected_physical_device_count;
		} else if (selected_physical_device_count > *pPhysicalDeviceCount)
			result = VK_INCOMPLETE;

		for (unsigned i = 0; i < *pPhysicalDeviceCount; ++i) {
			pPhysicalDevices[i] = selected_physical_devices[i];
		}
	}

out:
	free(physical_devices);
	free(selected_physical_devices);
	return result;
}

void  (*get_pdevice_proc_addr(VkInstance instance, const char* name))()
{
	auto info = instances[instance];
	return info.GetPhysicalDeviceProcAddr(instance, name);
}

void  (*get_instance_proc_addr(VkInstance instance, const char* name))()
{
	if (strcmp(name, "vkCreateInstance") == 0)
		return (void(*)())CreateInstance;
	if (strcmp(name, "vkDestroyInstance") == 0)
		return (void(*)())DestroyInstance;
	if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
		return (void(*)())device_select_EnumeratePhysicalDevices;

	auto info = instances[instance];
	return info.GetInstanceProcAddr(instance, name);
}

//}  // namespace

extern "C" VkResult vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
	if (pVersionStruct->loaderLayerInterfaceVersion < 2)
		return VK_ERROR_INITIALIZATION_FAILED;
	pVersionStruct->loaderLayerInterfaceVersion = 2;

	pVersionStruct->pfnGetInstanceProcAddr = get_instance_proc_addr;
	pVersionStruct->pfnGetPhysicalDeviceProcAddr = get_pdevice_proc_addr;

	return VK_SUCCESS;
}
