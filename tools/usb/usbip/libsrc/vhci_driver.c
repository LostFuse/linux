// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005-2007 Takahiro Hirofuchi
 */

#include "usbip_common.h"
#include "vhci_driver.h"
#include <limits.h>
#include <errno.h>
#include <netdb.h>
#include <libudev.h>
#include <dirent.h>
#include "sysfs_utils.h"

#undef  PROGNAME
#define PROGNAME "libusbip"

#define VHCI_DRIVER_PATH SYSFS_MNT_PATH "/" SYSFS_BUS_NAME "/" \
	USBIP_VHCI_BUS_TYPE "/" SYSFS_DRIVERS_NAME "/" USBIP_VHCI_DRV_NAME

struct usbip_vhci_driver *vhci_driver;
struct udev *udev_context;

static int driver_attrs;
static int configuration_attrs;
static int discovery_incomplete;

static struct usbip_imported_device *
imported_device_init(struct usbip_imported_device *idev, char *busid)
{
	struct udev_device *sudev;

	sudev = udev_device_new_from_subsystem_sysname(udev_context,
						       "usb", busid);
	if (!sudev) {
		dbg("udev_device_new_from_subsystem_sysname failed: %s", busid);
		goto err;
	}
	read_usb_device(sudev, &idev->udev);
	udev_device_unref(sudev);

	return idev;

err:
	errno = ENODEV;
	return NULL;
}

static int parse_status(const char *value)
{
	int ret = 0;
	char *c;

	/* skip a header line */
	c = strchr(value, '\n');
	if (!c)
		return -1;
	c++;

	while (*c != '\0') {
		unsigned int port, status, speed, devid;
		unsigned int sockfd;
		char lbusid[SYSFS_BUS_ID_SIZE];
		struct usbip_imported_device *idev;
		char hub[3];

		ret = sscanf(c, "%2s  %u %u %u %x %u %31s\n",
			     hub, &port, &status, &speed,
			     &devid, &sockfd, lbusid);

		if (ret != 7) {
			dbg("sscanf failed: %d", ret);
			return -1;
		}

		dbg("hub %s port %u status %u speed %u devid %x",
		    hub, port, status, speed, devid);
		dbg("sockfd %u lbusid %s", sockfd, lbusid);

		if (strcmp(hub, "hs") && strcmp(hub, "ss"))
			return -1;
		if (port >= (unsigned int)vhci_driver->nports) {
			errno = EAGAIN;
			return -1;
		}

		/* if a device is connected, look at it */
		idev = &vhci_driver->idev[port];
		memset(idev, 0, sizeof(*idev));

		if (strncmp("hs", hub, 2) == 0)
			idev->hub = HUB_SPEED_HIGH;
		else /* strncmp("ss", hub, 2) == 0 */
			idev->hub = HUB_SPEED_SUPER;

		idev->port	= port;
		idev->status	= status;

		idev->devid	= devid;

		idev->busnum	= (devid >> 16);
		idev->devnum	= (devid & 0x0000ffff);

		if (idev->status != VDEV_ST_NULL &&
		    idev->status != VDEV_ST_NOTASSIGNED &&
		    idev->status != VDEV_ST_ERROR) {
			idev = imported_device_init(idev, lbusid);
			if (!idev) {
				dbg("imported_device_init failed");
				return -1;
			}
		}

		/* go to the next line */
		c = strchr(c, '\n');
		if (!c)
			break;
		c++;
	}

	dbg("exit");

	return 0;
}

static int vhci_hcd_filter(const struct dirent *dirent)
{
	return !strncmp(dirent->d_name, "vhci_hcd.", 9);
}

static int read_configuration(unsigned int *controllers, unsigned int *ports)
{
	char value[SYSFS_NPORTS_SIZE];
	int length = 0;

	if (read_sysfs_attribute(VHCI_DRIVER_PATH "/configuration", value,
				 sizeof(value)) < 0)
		return -1;
	if (sscanf(value, "%u %u %n", controllers, ports, &length) != 2 ||
	    value[length] ||
	    ((!*controllers || !*ports) && (*controllers || *ports)) ||
	    *ports > 15 || (*ports && *controllers > INT_MAX / (2 * *ports))) {
		errno = ENODEV;
		return -1;
	}
	return 0;
}

static int check_configuration(void)
{
	unsigned int count, ports;

	if (read_configuration(&count, &ports) < 0)
		return errno == ENOENT && !configuration_attrs ? 0 : -1;
	if (count != (unsigned int)vhci_driver->ncontrollers ||
	    2 * ports * count != (unsigned int)vhci_driver->nports) {
		errno = EAGAIN;
		return -1;
	}
	return 0;
}

static int refresh_imported_device_list(void)
{
	const char *attr_status;
	char status[SYSFS_DEVICE_NAME_SIZE];
	char path[PATH_MAX], value[4096];
	int i, ret;
	struct udev_device *hc_device;

	discovery_incomplete = 0;
	memset(vhci_driver->idev, 0,
	       vhci_driver->nports * sizeof(*vhci_driver->idev));
	for (i = 0; i < vhci_driver->nports; i++)
		vhci_driver->idev[i].status = VDEV_ST_NOTASSIGNED;

	for (i = 0; i < vhci_driver->ncontrollers; i++) {
		if (driver_attrs) {
			snprintf(path, sizeof(path), "%s/vhci_hcd.%d/status",
				 VHCI_DRIVER_PATH, i);
			if (read_sysfs_attribute(path, value, sizeof(value)) < 0) {
				if (errno == ENOENT || errno == ENODEV) {
					discovery_incomplete = 1;
					continue;
				}
				return -1;
			}
			ret = parse_status(value);
		} else {
			hc_device = udev_device_ref(vhci_driver->hc_device);
			if (i == 0)
				snprintf(status, sizeof(status), "status");
			else
				snprintf(status, sizeof(status), "status.%d", i);
			if (!hc_device) {
				err("cannot find controller %d", i);
				return -1;
			}
			attr_status = udev_device_get_sysattr_value(hc_device, status);
			if (!attr_status) {
				err("udev_device_get_sysattr_value %s failed", status);
				udev_device_unref(hc_device);
				return -1;
			}

			dbg("controller %d", i);
			ret = parse_status(attr_status);
			udev_device_unref(hc_device);
		}
		if (ret != 0)
			return -1;
	}

	/* Status paths may vanish while an idle pool is being replaced. */
	return driver_attrs ? check_configuration() : 0;
}

static int get_nports(struct udev_device **hc_device, int *controllers)
{
	char value[SYSFS_NPORTS_SIZE];
	const char *attr_nports = value;
	char *end;
	unsigned long nports;
	unsigned int count, ports;

	*controllers = 0;
	if (read_configuration(&count, &ports) == 0) {
		if (!count) {
			errno = ENODEV;
			return -1;
		}
		driver_attrs = 1;
		configuration_attrs = 1;
		*controllers = count;
		return 2 * ports * count;
	}
	if (errno != ENOENT)
		return -1;
	configuration_attrs = 0;

	if (read_sysfs_attribute(VHCI_DRIVER_PATH "/nports", value,
				 sizeof(value)) < 0) {
		/* An absent new attribute identifies the legacy layout. */
		if (errno != ENOENT) {
			err("error reading driver nports attribute");
			return -1;
		}
		if (!*hc_device)
			*hc_device = udev_device_new_from_subsystem_sysname(udev_context,
									    USBIP_VHCI_BUS_TYPE,
									    USBIP_VHCI_DEVICE_NAME);
		if (!*hc_device) {
			errno = ENODEV;
			return -1;
		}
		attr_nports = udev_device_get_sysattr_value(*hc_device, "nports");
		if (!attr_nports) {
			err("udev_device_get_sysattr_value nports failed");
			return -1;
		}
		driver_attrs = 0;
	} else {
		driver_attrs = 1;
	}

	errno = 0;
	nports = strtoul(attr_nports, &end, 10);
	if (errno || end == attr_nports || !nports || nports > INT_MAX ||
	    (*end && !(*end == '\n' && !end[1]))) {
		err("invalid nports attribute");
		return -1;
	}
	return nports;
}

static int get_ncontrollers(void)
{
	struct dirent **namelist;
	struct udev_device *platform;
	int n;

	if (driver_attrs) {
		/* Include unbound controllers when configuration is absent. */
		n = scandir(SYSFS_MNT_PATH "/bus/platform/devices",
			    &namelist, vhci_hcd_filter, NULL);
	} else {
		platform = udev_device_get_parent(vhci_driver->hc_device);
		if (!platform)
			return -1;

		n = scandir(udev_device_get_syspath(platform), &namelist, vhci_hcd_filter, NULL);
	}
	if (n < 0)
		err("scandir failed");
	else {
		for (int i = 0; i < n; i++)
			free(namelist[i]);
		free(namelist);
	}

	return n;
}

/*
 * Read the given port's record.
 *
 * To avoid buffer overflow we will read the entire line and
 * validate each part's size. The initial buffer is padded by 4 to
 * accommodate the 2 spaces, 1 newline and an additional character
 * which is needed to properly validate the 3rd part without it being
 * truncated to an acceptable length.
 */
static int read_record(int rhport, char *host, unsigned long host_len,
		char *port, unsigned long port_len, char *busid)
{
	int part;
	FILE *file;
	char path[PATH_MAX+1];
	char *buffer, *start, *end;
	char delim[] = {' ', ' ', '\n'};
	int max_len[] = {(int)host_len, (int)port_len, SYSFS_BUS_ID_SIZE};
	size_t buffer_len = host_len + port_len + SYSFS_BUS_ID_SIZE + 4;

	buffer = malloc(buffer_len);
	if (!buffer)
		return -1;

	snprintf(path, PATH_MAX, VHCI_STATE_PATH"/port%d", rhport);

	file = fopen(path, "r");
	if (!file) {
		err("fopen");
		free(buffer);
		return -1;
	}

	if (fgets(buffer, buffer_len, file) == NULL) {
		err("fgets");
		free(buffer);
		fclose(file);
		return -1;
	}
	fclose(file);

	/* validate the length of each of the 3 parts */
	start = buffer;
	for (part = 0; part < 3; part++) {
		end = strchr(start, delim[part]);
		if (end == NULL || (end - start) > max_len[part]) {
			free(buffer);
			return -1;
		}
		start = end + 1;
	}

	if (sscanf(buffer, "%s %s %s\n", host, port, busid) != 3) {
		err("sscanf");
		free(buffer);
		return -1;
	}

	free(buffer);

	return 0;
}

/* ---------------------------------------------------------------------- */

int usbip_vhci_driver_open(void)
{
	int nports, ncontrollers;
	struct udev_device *hc_device = NULL;
	int saved_errno;

	udev_context = udev_new();
	if (!udev_context) {
		err("udev_new failed");
		return -1;
	}

	nports = get_nports(&hc_device, &ncontrollers);
	if (nports <= 0) {
		err("no available ports");
		goto err;
	}
	dbg("available ports: %d", nports);

	vhci_driver = calloc(1, sizeof(struct usbip_vhci_driver) +
			nports * sizeof(struct usbip_imported_device));
	if (!vhci_driver) {
		err("vhci_driver allocation failed");
		goto err;
	}

	vhci_driver->nports = nports;
	vhci_driver->hc_device = hc_device;
	vhci_driver->ncontrollers = ncontrollers ? ncontrollers : get_ncontrollers();
	dbg("available controllers: %d", vhci_driver->ncontrollers);

	if (vhci_driver->ncontrollers <= 0) {
		err("no available usb controllers");
		goto err;
	}

	if (refresh_imported_device_list())
		goto err;

	return 0;

err:
	saved_errno = errno;
	udev_device_unref(hc_device);

	if (vhci_driver)
		free(vhci_driver);

	vhci_driver = NULL;

	udev_unref(udev_context);

	errno = saved_errno;
	return -1;
}


void usbip_vhci_driver_close(void)
{
	if (!vhci_driver)
		return;

	udev_device_unref(vhci_driver->hc_device);

	free(vhci_driver);

	vhci_driver = NULL;

	udev_unref(udev_context);
}


int usbip_vhci_refresh_device_list(void)
{
	struct usbip_vhci_driver *next;
	int nports, ncontrollers;

	if (driver_attrs) {
		nports = get_nports(&vhci_driver->hc_device, &ncontrollers);
		if (nports <= 0)
			goto err;
		next = realloc(vhci_driver, sizeof(*next) +
			       nports * sizeof(*next->idev));
		if (!next)
			goto err;
		vhci_driver = next;
		vhci_driver->nports = nports;
		vhci_driver->ncontrollers = ncontrollers ? ncontrollers : get_ncontrollers();
		if (vhci_driver->ncontrollers <= 0)
			goto err;
	}

	if (refresh_imported_device_list())
		goto err;

	return 0;
err:
	dbg("failed to refresh device list");
	return -1;
}


int usbip_vhci_get_free_port(uint32_t speed)
{
	for (int i = 0; i < vhci_driver->nports; i++) {

		switch (speed) {
		case	USB_SPEED_SUPER:
		case	USB_SPEED_SUPER_PLUS:
			if (vhci_driver->idev[i].hub != HUB_SPEED_SUPER)
				continue;
		break;
		default:
			if (vhci_driver->idev[i].hub != HUB_SPEED_HIGH)
				continue;
		break;
		}

		if (vhci_driver->idev[i].status == VDEV_ST_NULL)
			return vhci_driver->idev[i].port;
	}

	/* Missing status may reflect replacement followed by restoration. */
	errno = discovery_incomplete ? EAGAIN : ENOSPC;
	return -1;
}

int usbip_vhci_attach_device2(uint32_t port, int sockfd, uint32_t devid,
			      uint32_t speed)
{
	char buff[200]; /* what size should be ? */
	char attach_attr_path[SYSFS_PATH_MAX];
	char attr_attach[] = "attach";
	const char *path;
	int ret;

	snprintf(buff, sizeof(buff), "%u %d %u %u",
			port, sockfd, devid, speed);
	dbg("writing: %s", buff);

	path = driver_attrs ? VHCI_DRIVER_PATH :
		udev_device_get_syspath(vhci_driver->hc_device);
	snprintf(attach_attr_path, sizeof(attach_attr_path), "%s/%s",
		 path, attr_attach);

	dbg("attach attribute path: %s", attach_attr_path);

	ret = write_sysfs_attribute(attach_attr_path, buff, strlen(buff));
	if (ret < 0) {
		dbg("write_sysfs_attribute failed");
		return -1;
	}

	dbg("attached port: %u", port);

	return 0;
}

static unsigned long get_devid(uint8_t busnum, uint8_t devnum)
{
	return (busnum << 16) | devnum;
}

/* will be removed */
int usbip_vhci_attach_device(uint32_t port, int sockfd, uint8_t busnum,
			     uint8_t devnum, uint32_t speed)
{
	int devid = get_devid(busnum, devnum);

	return usbip_vhci_attach_device2(port, sockfd, devid, speed);
}

int usbip_vhci_detach_device(uint32_t port)
{
	char detach_attr_path[SYSFS_PATH_MAX];
	char attr_detach[] = "detach";
	const char *path;
	char buff[200]; /* what size should be ? */
	int ret;

	snprintf(buff, sizeof(buff), "%u", port);
	dbg("writing: %s", buff);

	path = driver_attrs ? VHCI_DRIVER_PATH :
		udev_device_get_syspath(vhci_driver->hc_device);
	snprintf(detach_attr_path, sizeof(detach_attr_path), "%s/%s",
		 path, attr_detach);
	dbg("detach attribute path: %s", detach_attr_path);

	ret = write_sysfs_attribute(detach_attr_path, buff, strlen(buff));
	if (ret < 0) {
		dbg("write_sysfs_attribute failed");
		return -1;
	}

	dbg("detached port: %u", port);

	return 0;
}

int usbip_vhci_imported_device_dump(struct usbip_imported_device *idev)
{
	char product_name[100];
	char host[NI_MAXHOST] = "unknown host";
	char serv[NI_MAXSERV] = "unknown port";
	char remote_busid[SYSFS_BUS_ID_SIZE];
	int ret;
	int read_record_error = 0;

	if (idev->status == VDEV_ST_NULL || idev->status == VDEV_ST_NOTASSIGNED)
		return 0;

	if (idev->status == VDEV_ST_ERROR) {
		printf("Port %02u: <%s>\n", idev->port,
		       usbip_status_string(idev->status));
		return 0;
	}

	ret = read_record(idev->port, host, sizeof(host), serv, sizeof(serv),
			  remote_busid);
	if (ret) {
		err("read_record");
		read_record_error = 1;
	}

	printf("Port %02u: <%s> at %s\n", idev->port,
	       usbip_status_string(idev->status),
	       usbip_speed_string(idev->udev.speed));

	usbip_names_get_product(product_name, sizeof(product_name),
				idev->udev.idVendor, idev->udev.idProduct);

	printf("       %s\n",  product_name);

	if (!read_record_error) {
		printf("%10s -> usbip://%s:%s/%s\n", idev->udev.busid,
		       host, serv, remote_busid);
		printf("%10s -> remote bus/dev %03d/%03d\n", " ",
		       idev->busnum, idev->devnum);
	} else {
		printf("%10s -> unknown host, remote port and remote busid\n",
		       idev->udev.busid);
		printf("%10s -> remote bus/dev %03d/%03d\n", " ",
		       idev->busnum, idev->devnum);
	}

	return 0;
}
