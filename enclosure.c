/*
 * mdadm - Enclosure management support
 *
 * Copyright (C) 2013 Facebook
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "mdadm.h"

#include <ctype.h>
#include <sys/types.h>
#include <dirent.h>


#define ENCLOSURE_STRING 40
struct slot {
	struct slot *next;
	char *devname;
	int slot_num;
};

const char ENC_BASE[] = "/sys/class/enclosure";

struct enclosure {
	char vendor[ENCLOSURE_STRING];
	char model[ENCLOSURE_STRING];
	char id[ENCLOSURE_STRING];
	struct slot *slot;
	char *devname;
	int num_slots;
	struct enclosure *next;
};

static void free_slot(struct slot *slot)
{
	while (slot) {
		struct slot *next = slot->next;

		free(slot->devname);
		free(slot);
		slot = next;
	}
}

static void free_enclosure(struct enclosure *enclosure)
{
	while (enclosure) {
		struct enclosure *next = enclosure->next;

		free_slot(enclosure->slot);
		free(enclosure->devname);
		free(enclosure);
		enclosure = next;
	}
}

static char *devpath_to_diskname_deprecated(const char *devpath)
{
	struct dirent *de = NULL;
	char *r_devpath;
	DIR *b_dir;
	char *diskname = NULL;

	r_devpath = realpath(devpath, NULL);
	if (!r_devpath)
		return NULL;

	b_dir = opendir("/sys/block");
	if (!b_dir)
		goto out;

	for (de = readdir(b_dir); de; de = readdir(b_dir)) {
		char diskpath[100];
		char *r_diskpath;
		int done;

		if (de->d_name[0] == '.')
			continue;
		sprintf(diskpath, "/sys/block/%s/device", de->d_name);
		r_diskpath = realpath(diskpath, NULL);
		if (!r_diskpath)
			continue;

		done = strcmp(r_devpath, r_diskpath) == 0;
		free(r_diskpath);
		if (done) {
			diskname = xstrdup(de->d_name);
			break;
		}
	}
	closedir(b_dir);
 out:
	free(r_devpath);
	return de ? diskname : NULL;
}

static char *device_to_diskname(const char *devpath)
{
	struct dirent *de = NULL;
	struct stat st;
	char *b_path;
	DIR *b_dir;

	if (stat(devpath, &st))
		return NULL; /* no device */

	xasprintf(&b_path, "%s/block", devpath);
	b_dir = opendir(b_path);

	/* on sysfs-deprecated hosts we need to do this the long way... */
	if (!b_dir)
		goto out;

	for (de = readdir(b_dir); de; de = readdir(b_dir)) {
		if (de->d_name[0] == '.')
			continue;
		if (de->d_type != DT_DIR)
			continue;
		/* 1:1 mapping of scsi device to block device */
		break;
	}
	closedir(b_dir);
 out:
	free(b_path);

	return de ? xstrdup(de->d_name) : devpath_to_diskname_deprecated(devpath);
}

static struct slot *parse_slots(struct enclosure *enclosure, DIR *slot_dir)
{
	struct slot *slot_list = NULL, **pos = &slot_list;
	char path[PATH_MAX];
	struct dirent *de;

	if (!slot_dir)
		return NULL;

	for (de = readdir(slot_dir); de; de = readdir(slot_dir)) {
		char slot_id[ENCLOSURE_STRING];
		struct slot *slot;
		char *e;

		if (de->d_name[0] == '.')
			continue;
		if (de->d_type != DT_DIR)
			continue;

		snprintf(path, sizeof(path), "%s/%s/%s/slot",
			 ENC_BASE, enclosure->devname, de->d_name);
		if (load_sys_n(path, slot_id, sizeof(slot_id)) != 0)
			continue;

		slot = xcalloc(1, sizeof(*slot));
		*pos = slot;
		pos = &slot->next;
		slot->slot_num = strtoul(slot_id, &e, 10);

		snprintf(path, sizeof(path), "%s/%s/%s/device",
			 ENC_BASE, enclosure->devname, de->d_name);
		slot->devname = device_to_diskname(path);
	}

	if (de) {
		free_slot(slot_list);
		slot_list = NULL;
	}

	return slot_list;
}

static struct enclosure *parse_enclosures(void)
{
	struct enclosure *enc_list = NULL, **pos = &enc_list;
	struct dirent *de;
	DIR *enc_dir;

	enc_dir = opendir(ENC_BASE);
	if (!enc_dir)
		return NULL;

	for (de = readdir(enc_dir); de; de = readdir(enc_dir)) {
		char num_slots[ENCLOSURE_STRING];
		struct enclosure *enclosure;
		char path[PATH_MAX];
		DIR *slots;
		char *e;

		if (de->d_name[0] == '.')
			continue;

		enclosure = xcalloc(1, sizeof(*enclosure));
		enclosure->devname = xstrdup(de->d_name);
		*pos = enclosure;
		pos = &enclosure->next;

		snprintf(path, sizeof(path), "%s/%s/id", ENC_BASE, de->d_name);
		if (load_sys_n(path, enclosure->id, sizeof(enclosure->id)) != 0)
			break;

		snprintf(path, sizeof(path), "%s/%s/components", ENC_BASE, de->d_name);
		if (load_sys_n(path, num_slots, sizeof(num_slots)) != 0)
			break;
		enclosure->num_slots = strtoul(num_slots, &e, 10);

		snprintf(path, sizeof(path), "%s/%s/device/model", ENC_BASE, de->d_name);
		load_sys_n(path, enclosure->model, sizeof(enclosure->model));

		snprintf(path, sizeof(path), "%s/%s/device/vendor", ENC_BASE, de->d_name);
		load_sys_n(path, enclosure->vendor, sizeof(enclosure->vendor));

		snprintf(path, sizeof(path), "%s/%s", ENC_BASE, de->d_name);
		slots = opendir(path);
		if (!slots)
			break;

		enclosure->slot = parse_slots(enclosure, slots);
		closedir(slots);

		if (!enclosure->slot)
			break;

	}

	if (de) {
		free_enclosure(enc_list);
		enc_list = NULL;
	}

	closedir(enc_dir);

	return enc_list;
}

static int detail_platform_enclosure(int verbose, int enumerate, char *enclosure_name)
{
	struct enclosure *enclosure = parse_enclosures();
	struct enclosure *e;
	int i;

	if (!enclosure)
		return 0;

	for (e = enclosure, i = 0; e; e = e->next, i++) {
		struct slot *s;

		if (i)
			printf("\n");
		printf("Enclosure%d:\n", i);
		printf("             Id : %s\n", e->id);
		if (e->vendor[0])
			printf("         Vendor : %s\n", e->vendor);
		if (e->model[0])
			printf("          Model : %s\n", e->model);
		printf("          Slots : %d\n", e->num_slots);
		if (e->slot)
			printf("\n");

		for (s = e->slot; s; s = s->next) {
			int fd;
			char buf[255];

			printf("         Slot%02d :", s->slot_num);
			snprintf(buf, sizeof(buf), "/dev/%s", s->devname ? : "");
			printf("%s%s", buf[0] ? " " : "", buf);
			if ((fd = dev_open(buf, O_RDONLY)) >= 0) {
				memset(buf, 0, sizeof(buf));
				if (scsi_get_serial(fd, buf, sizeof(buf)) == 0
				    && buf[3]) {
					char *serial;
					char *end;

					serial = &buf[4];

					while (isspace(*serial))
						serial++;
					end = serial;
					while (!isspace(*end) && *end != '\0')
						end++;
					if (end - &buf[4] > buf[3])
						end = &buf[4] + buf[3] - 1;
					*end = '\0';
					printf(" %s", serial);
				}
				close(fd);
			}
			printf("\n");
		}
	}

	free_enclosure(enclosure);
	return 0;
}

static int export_detail_enclosure(int verbose, char *enclosure_name)
{
	return 0;
}

const struct platform_ops enclosure_platform = {
	.detail = detail_platform_enclosure,
	.export_detail = export_detail_enclosure,
	.name = "enclosure",
};
