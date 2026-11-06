// SPDX-License-Identifier: GPL-2.0+
#include <linux/nospec.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "usbip_common.h"
#include "vhci.h"

DECLARE_RWSEM(vhci_registry_lock);

struct vhci_pool {
	struct vhci *controllers;
	unsigned int count;
};

static struct vhci_pool *vhci_pool;

unsigned int vhci_get_controller_count(void)
{
	return vhci_pool ? vhci_pool->count : 0;
}

struct vhci *vhci_get_controller(unsigned int id)
{
	if (!vhci_pool || id >= vhci_pool->count)
		return NULL;
	id = array_index_nospec(id, vhci_pool->count);
	return &vhci_pool->controllers[id];
}

void vhci_manager_stop(void)
{
	unsigned int i;

	for (i = 0; i < vhci_pool->count; i++) {
		platform_device_unregister(vhci_pool->controllers[i].pdev);
		vhci_pool->controllers[i].pdev = NULL;
	}
	sysfs_remove_link(&platform_bus.kobj, "vhci_hcd");
	kfree(vhci_pool->controllers);
	kfree(vhci_pool);
	vhci_pool = NULL;
}

int vhci_manager_start(void)
{
	unsigned int i;
	int ret;

	vhci_pool = kzalloc_obj(*vhci_pool);
	if (!vhci_pool)
		return -ENOMEM;
	vhci_pool->count = VHCI_NR_HCS;
	vhci_pool->controllers = kzalloc_objs(struct vhci, vhci_pool->count);
	if (!vhci_pool->controllers) {
		kfree(vhci_pool);
		vhci_pool = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < vhci_pool->count; i++) {
		struct vhci *controller = &vhci_pool->controllers[i];
		void *vhci = controller;
		struct platform_device_info pdevinfo = {
			.name = "vhci_hcd",
			.id = i,
			.data = &vhci,
			.size_data = sizeof(void *),
		};

		controller->pdev = platform_device_register_full(&pdevinfo);
		ret = PTR_ERR_OR_ZERO(controller->pdev);
		if (ret) {
			while (i)
				platform_device_unregister(vhci_pool->controllers[--i].pdev);
			kfree(vhci_pool->controllers);
			kfree(vhci_pool);
			vhci_pool = NULL;
			return ret;
		}
	}

	return 0;
}
