// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 * Copyright (C) 2015-2016 Samsung Electronics
 *               Igor Kotrasinski <i.kotrasinsk@samsung.com>
 *               Krzysztof Opasiak <k.opasiak@samsung.com>
 */

#include <sys/stat.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>

#include "vhci_driver.h"
#include "usbip_common.h"
#include "usbip_network.h"
#include "usbip.h"

static const char usbip_attach_usage_string[] =
	"usbip attach <args>\n"
	"    -r, --remote=<host>      The machine with exported USB devices\n"
	"    -b, --busid=<busid>    Busid of the device on <host>\n"
	"    -d, --device=<devid>    Id of the virtual UDC on <host>\n";

void usbip_attach_usage(void)
{
	printf("usage: %s", usbip_attach_usage_string);
}

#define MAX_BUFF 100
static int record_connection(char *host, char *port, char *busid, int rhport)
{
	int fd;
	char path[PATH_MAX+1];
	char buff[MAX_BUFF+1];
	int ret;

	ret = mkdir(VHCI_STATE_PATH, 0700);
	if (ret < 0) {
		/* if VHCI_STATE_PATH exists, then it better be a directory */
		if (errno == EEXIST) {
			struct stat s;

			ret = stat(VHCI_STATE_PATH, &s);
			if (ret < 0)
				return -1;
			if (!(s.st_mode & S_IFDIR))
				return -1;
		} else
			return -1;
	}

	snprintf(path, PATH_MAX, VHCI_STATE_PATH"/port%d", rhport);

	fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU);
	if (fd < 0)
		return -1;

	snprintf(buff, MAX_BUFF, "%s %s %s\n",
			host, port, busid);

	ret = write(fd, buff, strlen(buff));
	if (ret != (ssize_t) strlen(buff)) {
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

static int selected_port_changed(int port, uint32_t speed)
{
	enum hub_speed hub = speed >= USB_SPEED_SUPER ? HUB_SPEED_SUPER : HUB_SPEED_HIGH;

	if (usbip_vhci_refresh_device_list() < 0)
		return errno == ENODEV || errno == ENOENT || errno == EAGAIN;
	return port >= vhci_driver->nports ||
	       vhci_driver->idev[port].status == VDEV_ST_NOTASSIGNED ||
	       vhci_driver->idev[port].hub != hub;
}

static int import_device(int sockfd, struct usbip_usb_device *udev)
{
	struct timespec deadline, now;
	uint32_t speed = udev->speed;
	int rc, port, saved_errno;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
		return -1;
	deadline.tv_sec++;

	for (;;) {
		rc = usbip_vhci_driver_open();
		if (rc < 0) {
			if (errno == ENODEV || errno == ENOENT || errno == EAGAIN)
				goto retry;
			err("open vhci_driver (is vhci_hcd loaded?)");
			return -1;
		}

		port = usbip_vhci_get_free_port(speed);
		if (port < 0) {
			if (errno == EAGAIN) {
				usbip_vhci_driver_close();
				errno = EAGAIN;
				goto retry;
			}
			err("no available port for device speed");
			goto err_driver_close;
		}

		dbg("got free port %d", port);
		rc = usbip_vhci_attach_device(port, sockfd, udev->busnum,
					      udev->devnum, udev->speed);
		if (rc < 0 && errno == EINVAL) {
			if (selected_port_changed(port, speed))
				errno = EAGAIN;
			else
				errno = EINVAL;
		}
		if (rc < 0 && errno != EBUSY && errno != ENODEV &&
		    errno != ENOENT && errno != EAGAIN) {
			err("import device");
			goto err_driver_close;
		}

		saved_errno = errno;
		usbip_vhci_driver_close();
		errno = saved_errno;
		if (rc == 0)
			return port;
retry:
		saved_errno = errno;
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return -1;
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
			errno = saved_errno;
			err("import device");
			return -1;
		}
		usleep(10000);
		errno = saved_errno;
	}

err_driver_close:
	saved_errno = errno;
	usbip_vhci_driver_close();
	errno = saved_errno;
	return -1;
}

static int query_import_device(int sockfd, char *busid)
{
	int rc;
	struct op_import_request request;
	struct op_import_reply   reply;
	uint16_t code = OP_REP_IMPORT;
	int status;

	memset(&request, 0, sizeof(request));
	memset(&reply, 0, sizeof(reply));

	/* send a request */
	rc = usbip_net_send_op_common(sockfd, OP_REQ_IMPORT, 0);
	if (rc < 0) {
		err("send op_common");
		return -1;
	}

	strncpy(request.busid, busid, SYSFS_BUS_ID_SIZE-1);

	PACK_OP_IMPORT_REQUEST(0, &request);

	rc = usbip_net_send(sockfd, (void *) &request, sizeof(request));
	if (rc < 0) {
		err("send op_import_request");
		return -1;
	}

	/* receive a reply */
	rc = usbip_net_recv_op_common(sockfd, &code, &status);
	if (rc < 0) {
		err("Attach Request for %s failed - %s\n",
		    busid, usbip_op_common_status_string(status));
		return -1;
	}

	rc = usbip_net_recv(sockfd, (void *) &reply, sizeof(reply));
	if (rc < 0) {
		err("recv op_import_reply");
		return -1;
	}

	PACK_OP_IMPORT_REPLY(0, &reply);

	/* check the reply */
	if (strncmp(reply.udev.busid, busid, SYSFS_BUS_ID_SIZE)) {
		err("recv different busid %s", reply.udev.busid);
		return -1;
	}

	/* import a device */
	return import_device(sockfd, &reply.udev);
}

static int attach_device(char *host, char *busid)
{
	int sockfd;
	int rc;
	int rhport;

	sockfd = usbip_net_tcp_connect(host, usbip_port_string);
	if (sockfd < 0) {
		err("tcp connect");
		return -1;
	}

	rhport = query_import_device(sockfd, busid);
	close(sockfd);
	if (rhport < 0)
		return -1;

	rc = record_connection(host, usbip_port_string, busid, rhport);
	if (rc < 0) {
		err("record connection");
		return -1;
	}

	return 0;
}

int usbip_attach(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "remote", required_argument, NULL, 'r' },
		{ "busid",  required_argument, NULL, 'b' },
		{ "device",  required_argument, NULL, 'd' },
		{ NULL, 0,  NULL, 0 }
	};
	char *host = NULL;
	char *busid = NULL;
	int opt;
	int ret = -1;

	for (;;) {
		opt = getopt_long(argc, argv, "d:r:b:", opts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case 'r':
			host = optarg;
			break;
		case 'd':
		case 'b':
			busid = optarg;
			break;
		default:
			goto err_out;
		}
	}

	if (!host || !busid)
		goto err_out;

	ret = attach_device(host, busid);
	goto out;

err_out:
	usbip_attach_usage();
out:
	return ret;
}
