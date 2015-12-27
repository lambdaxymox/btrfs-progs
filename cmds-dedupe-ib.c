/*
 * Copyright (C) 2015 Fujitsu.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <getopt.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>

#include "ctree.h"
#include "ioctl.h"

#include "commands.h"
#include "utils.h"
#include "kerncompat.h"
#include "dedupe-ib.h"

static const char * const dedupe_ib_cmd_group_usage[] = {
	"btrfs dedupe-inband <command> [options] <path>",
	NULL
};

static const char dedupe_ib_cmd_group_info[] =
"manage inband(write time) de-duplication";

static const char * const cmd_dedupe_ib_enable_usage[] = {
	"btrfs dedupe-inband enable [options] <path>",
	"Enable in-band(write time) de-duplication of a btrfs.",
	"",
	"-s|--storage-backend <BACKEND>",
	"           specify dedupe hash storage backend",
	"           supported backend: 'inmemory'",
	"-b|--blocksize <BLOCKSIZE>",
	"           specify dedupe block size",
	"           default value is 128K",
	"-a|--hash-algorithm <HASH>",
	"           specify hash algorithm",
	"           only 'sha256' is supported yet",
	"-l|--limit-hash <LIMIT>",
	"           specify maximum number of hashes stored in memory",
	"           only for 'inmemory' backend",
	"           positive value is valid, default value is 32K",
	"-m|--limit-mem <LIMIT>",
	"           specify maximum memory used for hashes",
	"           only for 'inmemory' backend",
	"           value larger than or equal to 1024 is valid, no default",
	"           only one of '-m' and '-l' is allowed",
	"-f|--force",
	"           force enable command to be executed",
	"           will skip some memory limit check",
	"           also without this flag enable command is not allowed to be",
	"           executed if dedupe is already enabled",
	"           note: unspecified parameter will be reset to default value",
	NULL
};


#define report_fatal_parameter(dargs, old, member, type, err_val, fmt)	\
if (dargs->member != old->member && dargs->member == (type)(err_val)) {	\
	error("unsupported dedupe "#member": %"#fmt"", old->member);	\
	return;								\
}

#define report_option_parameter(dargs, old, member, type, err_val, fmt)	\
if (dargs->member != old->member && dargs->member == (type)(err_val))	\
	warning("unsupported optional "#member": %"#fmt", continue",	\
		old->member);
static void report_parameter_error(struct btrfs_ioctl_dedupe_args *dargs,
				   struct btrfs_ioctl_dedupe_args *old)
{
	if (dargs->flags == (u8)-1) {
		if (dargs->status == 1 &&
		    old->cmd == BTRFS_DEDUPE_CTL_ENABLE &&
		    !(old->flags & BTRFS_DEDUPE_FLAG_FORCE)) {
			error("can't re-enable dedupe without --force");
			return;
		}
		report_option_parameter(dargs, old, flags, u8, -1, x);
	}
	report_fatal_parameter(dargs, old, cmd, u16, -1, u);
	report_fatal_parameter(dargs, old, blocksize, u64, -1, llu);
	report_fatal_parameter(dargs, old, backend, u16, -1, u);
	report_fatal_parameter(dargs, old, hash_algo, u16, -1, u);
	if (dargs->limit_nr == 0 && dargs->limit_mem == 0)
		error("unsupported dedupe limit combination: nr: %llu, mem: %llu",
				old->limit_nr, old->limit_mem);
	return;
}

static int cmd_dedupe_ib_enable(int argc, char **argv)
{
	int ret;
	int fd = -1;
	char *path;
	u64 blocksize = BTRFS_DEDUPE_BLOCKSIZE_DEFAULT;
	u16 hash_algo = BTRFS_DEDUPE_HASH_SHA256;
	u16 backend = BTRFS_DEDUPE_BACKEND_INMEMORY;
	u64 limit_nr = 0;
	u64 limit_mem = 0;
	u64 sys_mem = 0;
	int force = 0;
	struct btrfs_ioctl_dedupe_args dargs;
	struct btrfs_ioctl_dedupe_args backup;
	struct sysinfo info;
	DIR *dirstream = NULL;

	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "storage-backend", required_argument, NULL, 's'},
			{ "blocksize", required_argument, NULL, 'b'},
			{ "hash-algorithm", required_argument, NULL, 'a'},
			{ "limit-hash", required_argument, NULL, 'l'},
			{ "limit-memory", required_argument, NULL, 'm'},
			{ "force", required_argument, NULL, 'f'},
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "s:b:a:l:m:", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 's':
			if (!strcasecmp("inmemory", optarg))
				backend = BTRFS_DEDUPE_BACKEND_INMEMORY;
			else {
				error("unsupported dedupe backend: %s", optarg);
				exit(1);
			}
			break;
		case 'b':
			blocksize = parse_size(optarg);
			break;
		case 'a':
			if (strcmp("sha256", optarg)) {
				error("unsupported dedupe hash algorithm: %s",
				      optarg);
				return 1;
			}
			break;
		case 'l':
			limit_nr = parse_size(optarg);
			if (limit_nr == 0) {
				error("limit should be larger than 0");
				return 1;
			}
			break;
		case 'm':
			limit_mem = parse_size(optarg);
			/*
			 * Make sure at least one hash is allocated
			 * 1024 should be good enough though.
			 */
			if (limit_mem < 1024) {
				error("memory limit should be larger than or equal to 1024");
				return 1;
			}
			break;
		case 'f':
			force = 1;
			break;
		default:
			usage(cmd_dedupe_ib_enable_usage);
			return 1;
		}
	}

	path = argv[optind];
	if (check_argc_exact(argc - optind, 1))
		usage(cmd_dedupe_ib_enable_usage);

	/* Validation check */
	if (!is_power_of_2(blocksize) ||
	    blocksize > BTRFS_DEDUPE_BLOCKSIZE_MAX ||
	    blocksize < BTRFS_DEDUPE_BLOCKSIZE_MIN) {
		error("invalid dedupe blocksize: %llu, not in range [%u,%u] or power of 2",
		      blocksize, BTRFS_DEDUPE_BLOCKSIZE_MIN,
		      BTRFS_DEDUPE_BLOCKSIZE_MAX);
		return 1;
	}
	if ((limit_nr || limit_mem) && backend != BTRFS_DEDUPE_BACKEND_INMEMORY) {
		error("limit is only valid for 'inmemory' backend");
		return 1;
	}
	if (limit_nr && limit_mem) {
		error("limit-memory and limit-hash can't be given at the same time");
		return 1;
	}

	ret = sysinfo(&info);
	if (ret < 0)
		warning("failed to determine system total ram size: %s",
			strerror(errno));
	else
		sys_mem = info.totalram;

	/*
	 * TODO: Add check for limit_nr against current system
	 * memory to avoid wrongly set limit.
	 */
	if (!force && limit_mem && sys_mem && sys_mem < limit_mem * 4)
		goto mem_check;

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		error("failed to open file or directory: %s", path);
		return 1;
	}
	memset(&dargs, -1, sizeof(dargs));
	dargs.cmd = BTRFS_DEDUPE_CTL_ENABLE;
	dargs.blocksize = blocksize;
	dargs.hash_algo = hash_algo;
	dargs.limit_nr = limit_nr;
	dargs.limit_mem = limit_mem;
	dargs.backend = backend;
	if (force)
		dargs.flags |= BTRFS_DEDUPE_FLAG_FORCE;
	else
		dargs.flags = 0;

	memcpy(&backup, &dargs, sizeof(dargs));
	ret = ioctl(fd, BTRFS_IOC_DEDUPE_CTL, &dargs);
	if (ret < 0) {
		error("failed to enable inband deduplication: %s",
		      strerror(errno));
		report_parameter_error(&dargs, &backup);
		ret = 1;
		goto out;
	}
	ret = 0;
mem_check:
	if (!force && dargs.limit_mem > sys_mem / 4) {
		ret = 1;
		error("memory limit %llu is too large compared to system memory: %llu",
		      limit_mem, sys_mem);
		error("recommened memory limit is no more than %llu",
		      sys_mem / 4);
		error("use --force option if you know what you are doing");
	}
out:
	close_file_or_dir(fd, dirstream);
	return ret;
}

const struct cmd_group dedupe_ib_cmd_group = {
	dedupe_ib_cmd_group_usage, dedupe_ib_cmd_group_info, {
		{ "enable", cmd_dedupe_ib_enable, cmd_dedupe_ib_enable_usage,
		  NULL, 0},
		NULL_CMD_STRUCT
	}
};

int cmd_dedupe_ib(int argc, char **argv)
{
	return handle_command_group(&dedupe_ib_cmd_group, argc, argv);
}