// SPDX-License-Identifier: GPL-2.0+
#include <linux/cleanup.h>
#include <linux/mutex.h>
#include <linux/nospec.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "usbip_common.h"
#include "vhci.h"

DECLARE_RWSEM(vhci_registry_lock);
static unsigned int vhci_num_controllers;
unsigned int vhci_hc_ports;
bool vhci_reconfiguring;

struct vhci_pool {
	struct vhci *controllers;
	unsigned int count;
	unsigned int ports;
};

static struct vhci_pool *vhci_pool;

unsigned int vhci_get_controller_count(void)
{
	return vhci_num_controllers;
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
	struct vhci *vhci;

	if (!vhci_pool)
		return NULL;
	vhci = vhci_get_controller(port / VHCI_PORTS);
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
	*controllers = vhci_pool ? vhci_pool->count : 0;
	*ports = vhci_pool ? vhci_pool->ports : 0;
	return 0;
}

static void release_pool(struct vhci_pool *pool)
{
	unsigned int i;

	if (!pool)
		return;
	for (i = 0; i < pool->count; i++) {
		struct vhci *vhci = &pool->controllers[i];

		if (!vhci->pdev)
			continue;
		if (vhci->registered)
			platform_device_del(vhci->pdev);
		vhci_release_hcds(vhci);
		platform_device_put(vhci->pdev);
	}
	kfree(pool->controllers);
	kfree(pool);
}

/* Prepare memory without registering devices or consuming USB bus numbers. */
static struct vhci_pool *prepare_pool(unsigned int count, unsigned int ports)
{
	struct vhci_pool *pool;
	unsigned int i;
	int ret;

	pool = kzalloc_obj(struct vhci_pool);
	if (!pool)
		return ERR_PTR(-ENOMEM);
	pool->controllers = kcalloc(count, sizeof(*pool->controllers), GFP_KERNEL);
	if (!pool->controllers) {
		ret = -ENOMEM;
		goto fail;
	}
	pool->count = count;
	pool->ports = ports;
	for (i = 0; i < count; i++) {
		struct vhci *vhci = &pool->controllers[i];

		vhci->pdev = platform_device_alloc("vhci_hcd", i);
		if (!vhci->pdev) {
			ret = -ENOMEM;
			goto fail;
		}
		vhci->id = i;
		vhci->probe_error = -ENODEV;
		ret = platform_device_add_data(vhci->pdev, &vhci, sizeof(vhci));
		if (ret)
			goto fail;
		ret = vhci_prepare_hcds(vhci, ports);
		if (ret)
			goto fail;
	}
	return pool;
fail:
	release_pool(pool);
	return ERR_PTR(ret);
}

static int register_pool(struct vhci_pool *pool)
{
	unsigned int i;
	int ret;

	/* The previous pool's HCDs must be gone before changing live geometry. */
	down_write(&vhci_registry_lock);
	vhci_num_controllers = pool->count;
	vhci_hc_ports = pool->ports;
	up_write(&vhci_registry_lock);

	for (i = 0; i < pool->count; i++) {
		struct vhci *vhci = &pool->controllers[i];

		ret = platform_device_add(vhci->pdev);
		if (ret)
			return ret;
		vhci->registered = true;
		/* Device registration alone does not imply a successful HCD probe. */
		device_lock(&vhci->pdev->dev);
		ret = vhci->probe_error ?: (vhci->ready ? 0 : -ENODEV);
		device_unlock(&vhci->pdev->dev);
		if (ret)
			return ret;
	}
	return 0;
}

void vhci_manager_stop(void)
{
	struct vhci_pool *old;

	down_write(&vhci_registry_lock);
	old = vhci_pool;
	vhci_reconfiguring = true;
	vhci_pool = NULL;
	up_write(&vhci_registry_lock);
	release_pool(old);
	down_write(&vhci_registry_lock);
	vhci_num_controllers = 0;
	vhci_hc_ports = 0;
	up_write(&vhci_registry_lock);
}

int vhci_manager_start(void)
{
	struct vhci_pool *pool;
	int ret;

	pool = prepare_pool(VHCI_NR_HCS, VHCI_HC_PORTS);
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	ret = register_pool(pool);
	if (ret) {
		release_pool(pool);
		pool = NULL;
	}
	down_write(&vhci_registry_lock);
	vhci_pool = pool;
	if (!pool) {
		vhci_num_controllers = 0;
		vhci_hc_ports = 0;
	}
	up_write(&vhci_registry_lock);
	return ret;
}
