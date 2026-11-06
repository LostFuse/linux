// SPDX-License-Identifier: GPL-2.0+
#include <linux/nospec.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/cleanup.h>

#include "usbip_common.h"
#include "vhci.h"

struct vhci_pool {
	struct vhci *controllers;
	unsigned int count;
};

static struct vhci_pool *vhci_pool;
DECLARE_RWSEM(vhci_registry_lock);
bool vhci_reconfiguring;

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

struct vhci *vhci_get_controller_by_port(u32 port)
{
	struct vhci *vhci = vhci_get_controller(port / VHCI_PORTS);

	return vhci && vhci->ready ? vhci : NULL;
}

/*
 * Probe/remove call this with the platform device lock held. Clearing readiness
 * drains registry readers before remove tears down the HCDs; probe sets it only
 * after both HCDs are registered. Platform device registration and removal must
 * therefore run without the registry lock held.
 */
void vhci_set_ready(struct vhci *vhci, bool ready)
{
	down_write(&vhci_registry_lock);
	vhci->ready = ready;
	up_write(&vhci_registry_lock);
}

int vhci_get_configuration(unsigned int *controllers, unsigned int *ports)
{
	guard(rwsem_read)(&vhci_registry_lock);
	if (vhci_reconfiguring)
		return -EAGAIN;
	*controllers = vhci_get_controller_count();
	*ports = vhci_pool ? VHCI_HC_PORTS : 0;
	return 0;
}

static void release_pool(struct vhci_pool *pool)
{
	unsigned int i;

	for (i = 0; i < pool->count; i++) {
		if (pool->controllers[i].pdev)
			platform_device_unregister(pool->controllers[i].pdev);
	}
	sysfs_remove_link(&platform_bus.kobj, "vhci_hcd");
	kfree(pool->controllers);
	kfree(pool);
}

void vhci_manager_stop(void)
{
	struct vhci_pool *old;

	down_write(&vhci_registry_lock);
	old = vhci_pool;
	vhci_reconfiguring = true;
	vhci_pool = NULL;
	up_write(&vhci_registry_lock);
	if (old)
		release_pool(old);
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

		controller->id = i;
		controller->probe_error = -ENODEV;
		controller->pdev = platform_device_register_full(&pdevinfo);
		ret = PTR_ERR_OR_ZERO(controller->pdev);
		if (ret) {
			controller->pdev = NULL;
			vhci_manager_stop();
			return ret;
		}
		/* Platform registration alone does not imply a successful probe. */
		device_lock(&controller->pdev->dev);
		ret = controller->probe_error;
		device_unlock(&controller->pdev->dev);
		if (ret) {
			vhci_manager_stop();
			return ret;
		}
	}

	return 0;
}
