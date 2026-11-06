// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2003-2008 Takahiro Hirofuchi
 * Copyright (C) 2015-2016 Nobuo Iwata
 */

#include <linux/kthread.h>
#include <linux/cleanup.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

/* Hardening for Spectre-v1 */
#include <linux/nospec.h>

#include "usbip_common.h"
#include "vhci.h"

/* TODO: refine locking ?*/

/*
 * output example:
 * hub port sta spd dev       sockfd local_busid
 * hs  0000 004 000 00000000  000003 1-2.3
 * ................................................
 * ss  0008 004 000 00000000  000004 2-3.4
 * ................................................
 *
 * Output includes socket fd instead of socket pointer address to avoid
 * leaking kernel memory address in:
 *	/sys/devices/platform/vhci_hcd.0/status and in debug output.
 * The socket pointer address is not used at the moment and it was made
 * visible as a convenient way to find IP address from socket pointer
 * address by looking up /proc/net/{tcp,tcp6}. As this opens a security
 * hole, the change is made to use sockfd instead.
 *
 */
static void port_show_vhci(char **out, int hub, int port, struct vhci_device *vdev)
{
	if (hub == HUB_SPEED_HIGH)
		*out += sprintf(*out, "hs  %04u %03u ",
				      port, vdev->ud.status);
	else /* hub == HUB_SPEED_SUPER */
		*out += sprintf(*out, "ss  %04u %03u ",
				      port, vdev->ud.status);

	if (vdev->ud.status == VDEV_ST_USED) {
		*out += sprintf(*out, "%03u %08x ",
				      vdev->speed, vdev->devid);
		*out += sprintf(*out, "%06u %s",
				      vdev->ud.sockfd,
				      dev_name(&vdev->udev->dev));

	} else {
		*out += sprintf(*out, "000 00000000 ");
		*out += sprintf(*out, "000000 0-0");
	}

	*out += sprintf(*out, "\n");
}

/* Sysfs entry to show port status */
static ssize_t status_show_vhci(struct vhci *vhci, char *out)
{
	char *s = out;
	int i;
	unsigned long flags;

	spin_lock_irqsave(&vhci->lock, flags);

	for (i = 0; i < vhci_hc_ports; i++) {
		struct vhci_device *vdev = &vhci->vhci_hcd_hs->vdev[i];

		spin_lock(&vdev->ud.lock);
		port_show_vhci(&out, HUB_SPEED_HIGH,
			       vhci->id * VHCI_PORTS + i, vdev);
		spin_unlock(&vdev->ud.lock);
	}

	for (i = 0; i < vhci_hc_ports; i++) {
		struct vhci_device *vdev = &vhci->vhci_hcd_ss->vdev[i];

		spin_lock(&vdev->ud.lock);
		port_show_vhci(&out, HUB_SPEED_SUPER,
			       vhci->id * VHCI_PORTS + vhci_hc_ports + i, vdev);
		spin_unlock(&vdev->ud.lock);
	}

	spin_unlock_irqrestore(&vhci->lock, flags);

	return out - s;
}

static ssize_t status_show_id(int id, char *out, bool legacy)
{
	struct vhci *vhci;
	char *s = out;

	guard(rwsem_read)(&vhci_registry_lock);
	if (vhci_reconfiguring)
		return -EAGAIN;
	vhci = vhci_get_controller(id);
	if (!vhci || (!vhci->ready && !legacy))
		return -ENODEV;
	out += sprintf(out, "hub port sta spd dev      sockfd local_busid\n");
	if (vhci->ready) {
		out += status_show_vhci(vhci, out);
	} else {
		unsigned int i;

		for (i = 0; i < VHCI_PORTS; i++)
			out += sprintf(out, "%s  %04u %03u 000 00000000 000000 0-0\n",
				       i < vhci_hc_ports ? "hs" : "ss",
				       vhci->id * VHCI_PORTS + i, VDEV_ST_NOTASSIGNED);
	}
	return out - s;
}

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr, char *out)
{
	return status_show_id(to_platform_device(dev)->id, out, false);
}
DEVICE_ATTR_RO(status);

static ssize_t num_ports_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct vhci *vhci;

	guard(rwsem_read)(&vhci_registry_lock);
	vhci = vhci_get_controller(to_platform_device(dev)->id);
	if (!vhci || !vhci->ready)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", vhci_hc_ports);
}
DEVICE_ATTR_RO(num_ports);

static ssize_t nports_show_value(char *out)
{
	unsigned int controllers, ports;
	int ret;

	ret = vhci_get_configuration(&controllers, &ports);
	return ret ? ret : sysfs_emit(out, "%u\n", 2 * ports * controllers);
}

static ssize_t nports_show(struct device_driver *driver, char *out)
{
	return nports_show_value(out);
}
DRIVER_ATTR_RO(nports);

/* Sysfs entry to shutdown a virtual connection */
static int vhci_port_disconnect(struct vhci_hcd *vhci_hcd, __u32 rhport)
{
	struct vhci_device *vdev = &vhci_hcd->vdev[rhport];
	struct vhci *vhci = vhci_hcd->vhci;
	unsigned long flags;

	usbip_dbg_vhci_sysfs("enter\n");

	mutex_lock(&vdev->ud.sysfs_lock);

	/* lock */
	spin_lock_irqsave(&vhci->lock, flags);
	spin_lock(&vdev->ud.lock);

	if (vdev->ud.status == VDEV_ST_NULL) {
		pr_err("not connected %d\n", vdev->ud.status);

		/* unlock */
		spin_unlock(&vdev->ud.lock);
		spin_unlock_irqrestore(&vhci->lock, flags);
		mutex_unlock(&vdev->ud.sysfs_lock);

		return -EINVAL;
	}

	/* unlock */
	spin_unlock(&vdev->ud.lock);
	spin_unlock_irqrestore(&vhci->lock, flags);

	usbip_event_add(&vdev->ud, VDEV_EVENT_DOWN);

	mutex_unlock(&vdev->ud.sysfs_lock);

	return 0;
}

/* The caller holds vhci_registry_lock until the port operation completes. */
static struct vhci_hcd *resolve_port(u32 port, u32 *rhport)
{
	struct vhci *vhci = vhci_get_controller_by_port(port);
	u32 relative;

	if (!vhci)
		return NULL;
	relative = port % VHCI_PORTS;
	*rhport = array_index_nospec(relative % vhci_hc_ports, vhci_hc_ports);
	return relative < vhci_hc_ports ? vhci->vhci_hcd_hs : vhci->vhci_hcd_ss;
}

static ssize_t vhci_detach(const char *buf, size_t count)
{
	u32 port, rhport;
	struct vhci_hcd *hcd;
	int ret;

	guard(rwsem_read)(&vhci_registry_lock);
	if (vhci_reconfiguring)
		return -EAGAIN;
	if (kstrtou32(buf, 10, &port))
		return -EINVAL;
	hcd = resolve_port(port, &rhport);
	if (!hcd)
		return -EINVAL;
	ret = vhci_port_disconnect(hcd, rhport);
	return ret ? ret : count;
}

static ssize_t detach_store(struct device_driver *driver,
			    const char *buf, size_t count)
{
	return vhci_detach(buf, count);
}
DRIVER_ATTR_WO(detach);

static int valid_speed(enum usb_device_speed speed)
{
	switch (speed) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
	case USB_SPEED_WIRELESS:
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:
		break;
	default:
		pr_err("Failed attach request for unsupported USB speed: %s\n",
			usb_speed_string(speed));
		return 0;
	}

	return 1;
}

/* Sysfs entry to establish a virtual connection */
/*
 * To start a new USB/IP attachment, a userland program needs to setup a TCP
 * connection and then write its socket descriptor with remote device
 * information into this sysfs file.
 *
 * A remote device is virtually attached to the root-hub port of @rhport with
 * @speed. @devid is embedded into a request to specify the remote device in a
 * server host.
 *
 * write() returns 0 on success, else negative errno.
 */
static ssize_t vhci_attach(const char *buf, size_t count)
{
	struct device *dev;
	struct socket *socket;
	int sockfd = 0;
	__u32 port = 0, rhport = 0, devid = 0, speed = 0;
	struct vhci_hcd *vhci_hcd;
	struct vhci_hcd *expected;
	struct vhci_device *vdev;
	struct vhci *vhci;
	int err;
	unsigned long flags;
	struct task_struct *tcp_rx = NULL;
	struct task_struct *tcp_tx = NULL;

	guard(rwsem_read)(&vhci_registry_lock);
	if (vhci_reconfiguring)
		return -EAGAIN;

	/*
	 * @rhport: port number of vhci_hcd
	 * @sockfd: socket descriptor of an established TCP connection
	 * @devid: unique device identifier in a remote host
	 * @speed: usb device speed in a remote host
	 */
	if (sscanf(buf, "%u %u %u %u", &port, &sockfd, &devid, &speed) != 4)
		return -EINVAL;
	if (!valid_speed(speed))
		return -EINVAL;

	vhci_hcd = resolve_port(port, &rhport);
	if (!vhci_hcd)
		return -ENODEV;
	vhci = vhci_hcd->vhci;
	dev = &vhci->pdev->dev;
	expected = speed >= USB_SPEED_SUPER ? vhci->vhci_hcd_ss : vhci->vhci_hcd_hs;
	if (vhci_hcd != expected)
		return -EINVAL;
	vdev = &vhci_hcd->vdev[rhport];

	mutex_lock(&vdev->ud.sysfs_lock);

	/* Extract socket from fd. */
	socket = sockfd_lookup(sockfd, &err);
	if (!socket) {
		dev_err(dev, "failed to lookup sock");
		err = -EINVAL;
		goto unlock_mutex;
	}
	if (socket->type != SOCK_STREAM) {
		dev_err(dev, "Expecting SOCK_STREAM - found %d",
		       socket->type);
		sockfd_put(socket);
		err = -EINVAL;
		goto unlock_mutex;
	}

	/* create threads before locking */
	tcp_rx = kthread_create(vhci_rx_loop, &vdev->ud, "vhci_rx");
	if (IS_ERR(tcp_rx)) {
		sockfd_put(socket);
		err = -EINVAL;
		goto unlock_mutex;
	}
	tcp_tx = kthread_create(vhci_tx_loop, &vdev->ud, "vhci_tx");
	if (IS_ERR(tcp_tx)) {
		kthread_stop(tcp_rx);
		sockfd_put(socket);
		err = -EINVAL;
		goto unlock_mutex;
	}

	/* get task structs now */
	get_task_struct(tcp_rx);
	get_task_struct(tcp_tx);

	/* now begin lock until setting vdev status set */
	spin_lock_irqsave(&vhci->lock, flags);
	spin_lock(&vdev->ud.lock);

	if (vdev->ud.status != VDEV_ST_NULL) {
		/* end of the lock */
		spin_unlock(&vdev->ud.lock);
		spin_unlock_irqrestore(&vhci->lock, flags);

		sockfd_put(socket);
		kthread_stop_put(tcp_rx);
		kthread_stop_put(tcp_tx);

		dev_err(dev, "port %d already used\n", rhport);
		/*
		 * Will be retried from userspace
		 * if there's another free port.
		 */
		err = -EBUSY;
		goto unlock_mutex;
	}

	dev_info(dev, "pdev(%u) rhport(%u) sockfd(%d)\n",
		vhci->id, rhport, sockfd);
	dev_info(dev, "devid(%u) speed(%u) speed_str(%s)\n",
		devid, speed, usb_speed_string(speed));

	vdev->devid         = devid;
	vdev->speed         = speed;
	vdev->ud.sockfd     = sockfd;
	vdev->ud.tcp_socket = socket;
	vdev->ud.tcp_rx     = tcp_rx;
	vdev->ud.tcp_tx     = tcp_tx;
	vdev->ud.status     = VDEV_ST_NOTASSIGNED;
	vdev->ud.kcov_handle = kcov_common_handle();

	spin_unlock(&vdev->ud.lock);
	spin_unlock_irqrestore(&vhci->lock, flags);
	/* end the lock */

	wake_up_process(vdev->ud.tcp_rx);
	wake_up_process(vdev->ud.tcp_tx);

	rh_port_connect(vdev, speed);

	dev_info(dev, "Device attached\n");

	mutex_unlock(&vdev->ud.sysfs_lock);

	return count;

unlock_mutex:
	mutex_unlock(&vdev->ud.sysfs_lock);
	return err;
}

static ssize_t attach_store(struct device_driver *driver,
			    const char *buf, size_t count)
{
	return vhci_attach(buf, count);
}
DRIVER_ATTR_WO(attach);

/*
 * Silent controller-zero compatibility interface. Keep its wrappers
 * and registration data together for existing userspace.
 * Current tools use driver controls and each device's status file.
 */
static ssize_t nports_dev_show(struct device *dev,
			       struct device_attribute *attr, char *out)
{
	return nports_show_value(out);
}

static struct device_attribute dev_attr_nports =
	__ATTR(nports, 0444, nports_dev_show, NULL);

static ssize_t detach_dev_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	return vhci_detach(buf, count);
}

static struct device_attribute dev_attr_detach =
	__ATTR(detach, 0200, NULL, detach_dev_store);

static ssize_t attach_dev_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	return vhci_attach(buf, count);
}

static struct device_attribute dev_attr_attach =
	__ATTR(attach, 0200, NULL, attach_dev_store);

struct status_attr {
	struct device_attribute attr;
	char name[sizeof("status.2147483647")];
	int id;
};

static struct status_attr *status_attrs;

static ssize_t status_legacy_show(struct device *dev,
				  struct device_attribute *attr, char *out)
{
	struct status_attr *status = container_of(attr, struct status_attr, attr);

	return status_show_id(status->id, out, true);
}

struct attribute_group vhci_attr_group;

int vhci_init_attr_group(unsigned int count)
{
	struct attribute **attrs;
	int i;

	attrs = kzalloc_objs(struct attribute *, count + 4);
	if (!attrs)
		return -ENOMEM;

	status_attrs = kzalloc_objs(struct status_attr, count);
	if (!status_attrs) {
		kfree(attrs);
		return -ENOMEM;
	}

	attrs[0] = &dev_attr_nports.attr;
	attrs[1] = &dev_attr_detach.attr;
	attrs[2] = &dev_attr_attach.attr;
	attrs[3] = &dev_attr_usbip_debug.attr;
	/* Controller zero's status is provided by the device group. */
	for (i = 1; i < count; i++) {
		struct status_attr *status = &status_attrs[i];

		status->id = i;
		snprintf(status->name, sizeof(status->name), "status.%d", i);
		status->attr.attr.name = status->name;
		status->attr.attr.mode = 0444;
		status->attr.show = status_legacy_show;
		sysfs_attr_init(&status->attr.attr);
		attrs[i + 3] = &status->attr.attr;
	}
	vhci_attr_group.attrs = attrs;
	return 0;
}

void vhci_finish_attr_group(void)
{
	kfree(status_attrs);
	kfree(vhci_attr_group.attrs);
}

/* End of the controller-zero compatibility interface. */
