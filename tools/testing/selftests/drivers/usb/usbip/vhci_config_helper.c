// SPDX-License-Identifier: GPL-2.0-only
/* Test idle-only configuration using an attached stream socket pair. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DRIVER "/sys/bus/platform/drivers/vhci_hcd"

static int read_attribute(const char *name, char *value, size_t size)
{
	char path[256];
	ssize_t length;
	int fd, saved_errno;

	snprintf(path, sizeof(path), DRIVER "/%s", name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	length = read(fd, value, size - 1);
	saved_errno = errno;
	close(fd);
	if (length <= 0) {
		errno = length ? saved_errno : ENODEV;
		return -1;
	}
	value[length] = '\0';
	return 0;
}

static int write_attribute(const char *name, const char *value)
{
	char path[256];
	ssize_t length;
	int fd, saved_errno;

	snprintf(path, sizeof(path), DRIVER "/%s", name);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	length = write(fd, value, strlen(value));
	saved_errno = errno;
	close(fd);
	if (length != (ssize_t)strlen(value)) {
		errno = length < 0 ? saved_errno : EIO;
		return -1;
	}
	return 0;
}

static int port_state(void)
{
	char status[512], *row;
	unsigned int state;

	if (read_attribute("vhci_hcd.0/status", status, sizeof(status)))
		return -1;
	row = strchr(status, '\n');
	if (!row || sscanf(row + 1, "%*s %*u %u", &state) != 1)
		return -1;
	return state;
}

static int wait_idle(void)
{
	struct timespec deadline, now;
	const struct timespec pause = { .tv_nsec = 10000000 };

	if (clock_gettime(CLOCK_MONOTONIC, &deadline))
		return -1;
	deadline.tv_sec += 5;
	for (;;) {
		if (port_state() == 4)
			return 0;
		if (clock_gettime(CLOCK_MONOTONIC, &now) ||
		    now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
			return -1;
		nanosleep(&pause, NULL);
	}
}

int main(void)
{
	char before[64], after[64], request[64];
	bool attached = false;
	int sockets[2], ret = 1;

	if (read_attribute("configuration", before, sizeof(before)) ||
	    strcmp(before, "1 2\n")) {
		fprintf(stderr, "# Busy test requires configuration 1 2\n");
		return 1;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) {
		perror("# socketpair");
		return 1;
	}
	snprintf(request, sizeof(request), "0 %d 0 3", sockets[0]);
	if (write_attribute("attach", request)) {
		perror("# attach");
		goto out;
	}
	attached = true;
	if (port_state() != 5) {
		fprintf(stderr, "# Attached port is not awaiting enumeration\n");
		goto out;
	}
	if (!write_attribute("configuration", "1 1") || errno != EBUSY) {
		fprintf(stderr, "# Busy configuration did not return EBUSY\n");
		goto out;
	}
	if (write_attribute("configuration", before) ||
	    read_attribute("configuration", after, sizeof(after)) ||
	    strcmp(before, after) || port_state() != 5) {
		fprintf(stderr, "# Busy rejection or no-op changed the attachment\n");
		goto out;
	}
	ret = 0;
out:
	if (attached && (write_attribute("detach", "0") || wait_idle())) {
		fprintf(stderr, "# Could not return the test port to idle\n");
		ret = 1;
	}
	close(sockets[0]);
	close(sockets[1]);
	return ret;
}
