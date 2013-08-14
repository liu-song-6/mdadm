#include <asm/types.h>

struct scsi_mode_sense_data {
	int hdr_len;
	int block_len;
	int media_type;
	int data_len;
	int device_specific;
	char long_lba;
};

enum page_format {
	PF_VENDOR = 0,
	PF_STANDARD = 1,
};

static inline __u8 *to_mode_page(__u8 *buf, const struct scsi_mode_sense_data *data)
{
	return buf + data->hdr_len + data->block_len;
}

int scsi_mode_sense(int fd, __u8 pg, __u8 spg,
		    struct scsi_mode_sense_data *data,
		    __u8 *buf, size_t buf_len);
int scsi_mode_select(int fd, enum page_format pf, int save,
		     const struct scsi_mode_sense_data *data,
		     __u8 *page);
