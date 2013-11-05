/*
 *  Copyright (C) 2008-2013, Parallels, Inc. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/vfs.h>

#include "ploop.h"

int create_gpt_partition(const char *device, off_t size, __u32 blocksize)
{
	unsigned long long start = blocksize;
	unsigned long long end = size - start - GPT_DATA_SIZE;
	char *argv[7];
	char s1[22], s2[22];

	if (size <= start + GPT_DATA_SIZE) {
		ploop_err(0, "Image size should be greater than %llu", start);
		return -1;
	}
	argv[0] = "parted";
	argv[1] = "-s";
	argv[2] = (char *)device;
	argv[3] = "mklabel gpt mkpart primary";
	snprintf(s1, sizeof(s1), "%llus", (start >> 3) << 3);
	argv[4] = s1;
	snprintf(s2, sizeof(s2), "%llus", ((end >> 3) << 3) - 1);
	argv[5] = s2;
	argv[6] = NULL;

	if (run_prg(argv)) {
		ploop_err(0, "Failed to create partition");
		return -1;
	}

	return 0;
}

int make_fs(const char *device, const char *fstype, unsigned int fsblocksize)
{
	char part_device[64];
	char fsblock_size[14];
	char *argv[8];
	char ext_opts[1024];

	fsblocksize = fsblocksize != 0 ? fsblocksize : 4096;

	if (get_partition_device_name(device, part_device, sizeof(part_device)))
		return SYSEXIT_MKFS;

	argv[0] = "mkfs";
	argv[1] = "-t";
	argv[2] = (char*)fstype;
	argv[3] = "-j";
	snprintf(fsblock_size, sizeof(fsblock_size), "-b%u",
			fsblocksize);
	argv[4] = fsblock_size;
	/* Reserve enough space so that the block group descriptor table can grow to 16T */
	snprintf(ext_opts, sizeof(ext_opts), "-Elazy_itable_init,resize=4294967295");
	argv[5] = ext_opts;
	argv[6] = part_device;
	argv[7] = NULL;

	if (run_prg(argv))
		return SYSEXIT_MKFS;

	argv[0] = "tune2fs";
	argv[1] =  "-ouser_xattr,acl";
	argv[2] = "-c0";
	argv[3] = "-i0";
	argv[4] = part_device;
	argv[5] = NULL;

	if (run_prg(argv))
		return SYSEXIT_MKFS;

	return 0;
}

void tune_fs(const char *target, const char *device, unsigned long long size_sec)
{
	unsigned long long reserved_blocks;
	struct statfs fs;
	char *argv[5];
	char buf[21];

	if (statfs(target, &fs) != 0) {
		ploop_err(errno, "tune_fs: can't statfs %s", target);
		return;
	}
	reserved_blocks = size_sec / 100 * 5 * 512 / fs.f_bsize;
	if (reserved_blocks == 0) {
		ploop_err(0, "Can't set reserved blocks for size %llu",
				size_sec);
		return;
	}
	argv[0] = "tune2fs";
	argv[1] = "-r";
	snprintf(buf, sizeof(buf), "%llu", reserved_blocks);
	argv[2] = buf;
	argv[3] = (char *)device;
	argv[4] = NULL;

	run_prg(argv);
}

static char *get_resize_prog(void)
{
	int i;
	struct stat st;
	static char *progs[] = {"/usr/libexec/resize2fs", "/sbin/resize4fs", "/sbin/resize2fs", NULL};

	for (i = 0; progs[i] != NULL; i++)
		if (stat(progs[i], &st) == 0)
			return progs[i];

	return "resize2fs";
}

int resize_fs(const char *device, off_t size_sec)
{
	char *prog;
	char *argv[5];
	char buf[22];

	prog = get_resize_prog();
	argv[0] = prog;
	argv[1] = "-p";
	argv[2] = (char *)device;
	if (size_sec) {
		// align size to 4K
		snprintf(buf, sizeof(buf), "%luk", (long)(size_sec >> 3 << 3) >> 1);
		argv[3] = buf;
	} else
		argv[3] = NULL;
	argv[4] = NULL;

	if (run_prg(argv))
		return SYSEXIT_RESIZE_FS;
	return 0;
}

enum {
	BLOCK_COUNT,
	BLOCK_FREE,
	BLOCK_SIZE,
};

#define BLOCK_COUNT_BIT (1 << BLOCK_COUNT)
#define BLOCK_FREE_BIT (1 << BLOCK_FREE)
#define BLOCK_SIZE_BIT (1 << BLOCK_SIZE)

int dumpe2fs(const char *device, struct dump2fs_data *data)
{
	char cmd[512];
	char buf[512];
	FILE *fp;
	int found = BLOCK_COUNT_BIT | BLOCK_FREE_BIT | BLOCK_SIZE_BIT;

	snprintf(cmd, sizeof(cmd),  "LANG=C /sbin/dumpe2fs -h %s", device);
	fp = popen(cmd, "r");
	if (fp == NULL) {
		ploop_err(0, "Failed %s", cmd);
		return SYSEXIT_SYS;
	}

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		if ((found & BLOCK_COUNT_BIT) &&
				sscanf(buf, "Block count: %llu", &data->block_count) == 1)
			found &= ~BLOCK_COUNT_BIT;
		else if ((found & BLOCK_FREE_BIT) &&
				sscanf(buf, "Free blocks: %llu", &data->block_free) == 1)
			found &= ~BLOCK_FREE_BIT;
		else if ((found & BLOCK_SIZE_BIT) &&
				sscanf(buf, "Block size: %u", &data->block_size) == 1)
			found &= ~BLOCK_SIZE_BIT;
	}

	if (pclose(fp)) {
		ploop_err(0, "failed %s", cmd);
		return SYSEXIT_SYS;
	}
	if (found) {
		ploop_err(0, "Not enough data: %s (0x%x)", cmd, found);
		return SYSEXIT_SYS;
	}

	return 0;
}

int e2fsck(const char *device, int flags)
{
	char *arg[5];
	int i = 0;
	int ret;

	arg[i++] = "e2fsck";
	if (flags & E2FSCK_PREEN)
		arg[i++] = "-p";
	if (flags & E2FSCK_FORCE)
		arg[i++] = "-f";
	arg[i++] = (char *)device;
	arg[i++] = NULL;

	if (run_prg_rc(arg, &ret))
		return SYSEXIT_FSCK;

	/* exit code < 4 is OK, see man e2fsck */
	if (ret >= 4) {
		ploop_err(0, "e2fsck failed (exit code %d)\n", ret);
		return SYSEXIT_FSCK;
	}

	return 0;
}
