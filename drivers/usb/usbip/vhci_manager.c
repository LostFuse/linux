// SPDX-License-Identifier: GPL-2.0+
#include <linux/cleanup.h>
#include <linux/limits.h>
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
/*
 * Serializes whole-pool replacement and shutdown. Lock order is configuration
 * mutex, then registry lock. Drop the registry lock across platform device
 * registration/removal: probe/remove take it while holding the device lock.
 * The configuration mutex stays held until the new pool is published, keeping
 * shared geometry stable throughout HCD registration and teardown.
 */
static DEFINE_MUTEX(vhci_config_lock);

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

static int register_pool(struct vhci_pool *pool, const bool *bound)
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

		/* Suppress automatic probing when restoring an unbound controller. */
		if (bound && !bound[i]) {
			ret = device_set_driver_override(&vhci->pdev->dev,
							 "vhci_hcd-unbound");
			if (ret)
				return ret;
		}
		ret = platform_device_add(vhci->pdev);
		if (ret)
			return ret;
		vhci->registered = true;
		/* Device registration alone does not imply a successful HCD probe. */
		device_lock(&vhci->pdev->dev);
		if (bound && !bound[i])
			ret = device_set_driver_override(&vhci->pdev->dev, "");
		else
			ret = vhci->probe_error ?: (vhci->ready ? 0 : -ENODEV);
		device_unlock(&vhci->pdev->dev);
		if (ret)
			return ret;
	}
	return 0;
}

static bool pool_busy(struct vhci_pool *pool)
{
	unsigned int i;

	if (!pool)
		return false;
	for (i = 0; i < pool->count; i++) {
		struct vhci *vhci = &pool->controllers[i];
		struct vhci_hcd *hubs[] = { vhci->vhci_hcd_hs, vhci->vhci_hcd_ss };
		unsigned long flags;
		int hub, port;
		bool busy = false;

		if (!vhci->ready)
			continue;
		spin_lock_irqsave(&vhci->lock, flags);
		for (hub = 0; hub < ARRAY_SIZE(hubs); hub++) {
			for (port = 0; port < pool->ports; port++) {
				struct usbip_device *ud = &hubs[hub]->vdev[port].ud;

				spin_lock(&ud->lock);
				busy |= ud->status != VDEV_ST_NULL;
				spin_unlock(&ud->lock);
			}
		}
		spin_unlock_irqrestore(&vhci->lock, flags);
		if (busy)
			return true;
	}
	return false;
}

int vhci_configure(unsigned int controllers, unsigned int ports)
{
	struct vhci_pool *next, *old;
	unsigned int old_count = 0, old_ports = 0, i;
	bool *bound = NULL;
	int ret, restore_ret;

	/* Global ports and platform controller IDs must fit in an int. */
	if (!ports || ports > USB_SS_MAXPORTS || !controllers ||
	    controllers > INT_MAX / (2 * ports))
		return -EINVAL;
	/* Each controller needs two of USB core's allocatable bus numbers. */
	if (controllers > (USB_MAXBUS - 1) / 2)
		return -E2BIG;
	mutex_lock(&vhci_config_lock);
	down_write(&vhci_registry_lock);
	old = vhci_pool;
	if (old && old->count == controllers && old->ports == ports) {
		ret = 0;
		goto unlock_registry;
	}
	if (pool_busy(old)) {
		ret = -EBUSY;
		goto unlock_registry;
	}
	if (old) {
		bound = kcalloc(old->count, sizeof(*bound), GFP_KERNEL);
		if (!bound) {
			ret = -ENOMEM;
			goto unlock_registry;
		}
		for (i = 0; i < old->count; i++)
			bound[i] = old->controllers[i].ready;
	}
	/*
	 * Block registry readers during preparation so an attach cannot make the
	 * checked pool busy before it is withdrawn. Allocation failure leaves the
	 * existing pool published and usable.
	 */
	next = prepare_pool(controllers, ports);
	if (IS_ERR(next)) {
		ret = PTR_ERR(next);
		goto unlock_registry;
	}
	vhci_reconfiguring = true;
	vhci_pool = NULL;
	up_write(&vhci_registry_lock);
	if (old) {
		old_count = old->count;
		old_ports = old->ports;
	}
	release_pool(old);
	ret = register_pool(next, NULL);
	if (ret) {
		release_pool(next);
		next = NULL;
		if (old_count) {
			next = prepare_pool(old_count, old_ports);
			restore_ret = IS_ERR(next) ? PTR_ERR(next) :
				      register_pool(next, bound);
			if (restore_ret) {
				if (!IS_ERR(next))
					release_pool(next);
				next = NULL;
				pr_err("failed to restore VHCI controller pool: %d\n",
				       restore_ret);
			}
		}
	}
	down_write(&vhci_registry_lock);
	vhci_pool = next;
	if (!next) {
		vhci_num_controllers = 0;
		vhci_hc_ports = 0;
	}
	vhci_reconfiguring = false;
unlock_registry:
	up_write(&vhci_registry_lock);
	mutex_unlock(&vhci_config_lock);
	kfree(bound);
	return ret;
}

void vhci_manager_stop(void)
{
	struct vhci_pool *old;

	mutex_lock(&vhci_config_lock);
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
	mutex_unlock(&vhci_config_lock);
}

int vhci_manager_start(void)
{
	return vhci_configure(VHCI_NR_HCS, VHCI_HC_PORTS);
}
