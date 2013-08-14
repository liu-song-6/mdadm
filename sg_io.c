/*
 * Copyright (C) 2007-2008 Intel Corporation
 *
 *	Retrieve drive serial numbers for scsi disks
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
#include <stdio.h>
#include <string.h>
#include <scsi/scsi.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <stdlib.h>

#include "sg_io.h"

int scsi_get_serial(int fd, void *buf, size_t buf_len)
{
	__u8 inq_cmd[] = {INQUIRY, 1, 0x80, 0, buf_len, 0};
	__u8 sense[32];
	struct sg_io_hdr io_hdr;

	memset(&io_hdr, 0, sizeof(io_hdr));
	io_hdr.interface_id = 'S';
	io_hdr.cmdp = inq_cmd;
	io_hdr.cmd_len = sizeof(inq_cmd);
	io_hdr.dxferp = buf;
	io_hdr.dxfer_len = buf_len;
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.sbp = sense;
	io_hdr.mx_sb_len = sizeof(sense);
	io_hdr.timeout = 5000;

	return ioctl(fd, SG_IO, &io_hdr);
}

static int process_response(const char *cmd, struct sg_io_hdr *io)
{
	if ((io->info & SG_INFO_OK_MASK) != SG_INFO_OK) {
		if (io->sb_len_wr > 0) {
			int i;

			dprintf("resp: %x sense: %x asc: %x asq: %x\n",
				io->sbp[0], io->sbp[2] & 0xf, io->sbp[12],
				io->sbp[13]);
			dprintf("%s sense data:\n", cmd);
			for (i = 0; i < io->sb_len_wr; ++i) {
				if ((i > 0) && (0 == (i % 10)))
					printf("\n  ");
				dprintf("0x%02x ", io->sbp[i]);
			}
			dprintf("\n");
		}
		if (io->masked_status)
			dprintf("%s SCSI status=0x%x\n", cmd, io->status);
		if (io->host_status)
			dprintf("%s host_status=0x%x\n", cmd, io->host_status);
		if (io->driver_status)
			dprintf("%s driver_status=0x%x\n", cmd, io->driver_status);
		return -1;
	}
	return 0;
}

/**
 * scsi_mode_sense - fetch mode page(s) and a description
 *
 * cdb len based on buffer size
 */
static int __scsi_mode_sense(int fd, __u8 pg, __u8 spg,
			     struct scsi_mode_sense_data *data,
			     __u8 *buf, size_t buf_len, int force6)
{
	struct sg_io_hdr io_hdr;
	int rc, cdb_len;
	__u8 sense[32];
	__u8 cmd[10];

	if (buf_len < 8)
		return -1;

	memset(data, 0, sizeof(data));
	memset(cmd, 0, sizeof(cmd));
	cmd[2] = pg;
	cmd[3] = spg;

	if (buf_len > 255 || !force6) {
		cdb_len = 10;
		cmd[0] = MODE_SENSE_10;
		cmd[7] = buf_len >> 8;
		cmd[8] = buf_len;
	} else {
		cdb_len = 6;
		cmd[0] = MODE_SENSE;
		cmd[4] = buf_len;
	}
	memset(&io_hdr, 0, sizeof(io_hdr));
	memset(sense, 0, sizeof(sense));
	io_hdr.interface_id = 'S';
	io_hdr.cmdp = cmd;
	io_hdr.cmd_len = cdb_len;
	io_hdr.dxferp = buf;
	io_hdr.dxfer_len = buf_len;
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.sbp = sense;
	io_hdr.mx_sb_len = sizeof(sense);
	io_hdr.timeout = 30000;

	rc = ioctl(fd, SG_IO, &io_hdr);
	if (rc)
		return rc;

	if (cdb_len == 10) {
		data->data_len = buf[0]*256 + buf[1] + 2;
		data->media_type = buf[2];
		data->device_specific = buf[3];
		data->long_lba = buf[4] & 1;
		data->block_len = buf[6]*256 + buf[7];
	} else {
		data->data_len = buf[0] + 1;
		data->media_type = buf[1];
		data->device_specific = buf[2];
		data->block_len = buf[3];
	}
	data->hdr_len = cdb_len - 2;

	if ((size_t) (to_mode_page(buf, data) - buf) > buf_len)
		return -1;

	return process_response("MODE SENSE", &io_hdr);
}

int scsi_mode_sense(int fd, __u8 pg, __u8 spg,
		    struct scsi_mode_sense_data *data,
		    __u8 *buf, size_t buf_len)
{
	return __scsi_mode_sense(fd, pg, spg, data, buf, buf_len, 0);
}

int scsi_mode_select(int fd, enum page_format pf, int save,
		     const struct scsi_mode_sense_data *data,
		     __u8 *param_list)
{
	struct sg_io_hdr io_hdr;
	int rc, cdb_len;
	__u8 sense[32];
	__u8 cmd[10];
	__u8 *page;

	memset(cmd, 0, sizeof(cmd));
	page = to_mode_page(param_list, data);
	cmd[1] = (!!pf << 4) | ((page[0] >> 7) & !!save);

	/* ps reserved */
	page[0] &= 0x7f;

	if (data->hdr_len > 4) {
		cdb_len = 10;
		cmd[0] = MODE_SELECT_10;
		cmd[7] = data->data_len >> 8;
		cmd[8] = data->data_len;

		param_list[0] = 0; /* data len reserved */
		param_list[1] = 0; /*  "    "     "     */
		param_list[2] = data->media_type;
		param_list[3] = 0; /* whole disk parameter */
		param_list[6] = data->block_len >> 8;
		param_list[7] = data->block_len;
	} else {
		cdb_len = 6;
		cmd[0] = MODE_SELECT;
		cmd[4] = data->data_len;

		param_list[0] = 0; /* data len reserved */
		param_list[1] = data->media_type;
		param_list[2] = 0; /* whole disk parameter */
		param_list[3] = data->block_len;
	}
	memset(&io_hdr, 0, sizeof(io_hdr));
	memset(sense, 0, sizeof(sense));
	io_hdr.interface_id = 'S';
	io_hdr.cmdp = cmd;
	io_hdr.cmd_len = cdb_len;
	io_hdr.dxferp = param_list;
	io_hdr.dxfer_len = data->data_len;
	io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
	io_hdr.sbp = sense;
	io_hdr.mx_sb_len = sizeof(sense);
	io_hdr.timeout = 30000;

	rc = ioctl(fd, SG_IO, &io_hdr);
	if (rc)
		return rc;

	return process_response("MODE SELECT", &io_hdr);
}
