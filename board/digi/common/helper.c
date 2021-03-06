/*
 *  Copyright (C) 2017 by Digi International Inc.
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version2  as published by
 *  the Free Software Foundation.
*/
#include <common.h>
#include <asm/errno.h>
#include <malloc.h>
#include <nand.h>
#include <version.h>
#include <watchdog.h>
#ifdef CONFIG_OF_LIBFDT
#include <fdt_support.h>
#endif
#include <otf_update.h>
#include "helper.h"
#include "hwid.h"
DECLARE_GLOBAL_DATA_PTR;
#if defined(CONFIG_CMD_UPDATE_MMC) || defined(CONFIG_CMD_UPDATE_NAND)
#define CONFIG_CMD_UPDATE
#endif

#if defined(CONFIG_CMD_UPDATE) || defined(CONFIG_CMD_DBOOT)
enum {
	FWLOAD_NO,
	FWLOAD_YES,
	FWLOAD_TRY,
};

static const char *src_strings[] = {
	[SRC_TFTP] =	"tftp",
	[SRC_NFS] =	"nfs",
	[SRC_NAND] =	"nand",
	[SRC_USB] =	"usb",
	[SRC_MMC] =	"mmc",
	[SRC_RAM] =	"ram",
	[SRC_SATA] =	"sata",
};

/* hook for on-the-fly update and register function */
static int (*otf_update_hook)(otf_data_t *data) = NULL;
/* Data struct for on-the-fly update */
static otf_data_t otfd;
#endif

#ifdef CONFIG_AUTO_BOOTSCRIPT
#define AUTOSCRIPT_TFTP_MSEC		100
#define AUTOSCRIPT_TFTP_CNT		15
#define AUTOSCRIPT_START_AGAIN		100
extern ulong TftpRRQTimeoutMSecs;
extern int TftpRRQTimeoutCountMax;
extern unsigned long NetStartAgainTimeout;
int DownloadingAutoScript = 0;
int RunningAutoScript = 0;
#endif

int confirm_msg(char *msg)
{
#ifdef CONFIG_AUTO_BOOTSCRIPT
        /* From autoscript we shouldn't expect user's confirmations.
         * Assume yes is the correct answer here to avoid halting the script.
         */
	if (RunningAutoScript)
		return 1;
#endif

	printf(msg);
	if (confirm_yesno())
		return 1;

	puts("Operation aborted by user\n");
	return 0;
}

#if defined(CONFIG_CMD_UPDATE) || defined(CONFIG_CMD_DBOOT)
int get_source(int argc, char * const argv[], struct load_fw *fwinfo)
{
	int i;
	char *src;
#ifdef CONFIG_CMD_MTDPARTS
	struct mtd_device *dev;
	u8 pnum;
	char *partname;
#endif

	if (argc < 3) {
		fwinfo->src = SRC_TFTP;	/* default to TFTP */
		return 0;
	}

	src = argv[2];
	for (i = 0; i < ARRAY_SIZE(src_strings); i++) {
		if (!strncmp(src_strings[i], src, strlen(src))) {
			if (1 << i & CONFIG_SUPPORTED_SOURCES) {
				break;
			} else {
				fwinfo->src = SRC_UNSUPPORTED;
				goto _err;
			}
		}
	}

	if (i >= ARRAY_SIZE(src_strings)) {
		fwinfo->src = SRC_UNDEFINED;
		goto _err;
	}

	switch (i) {
	case SRC_USB:
	case SRC_MMC:
	case SRC_SATA:
		/* Get device:partition and file system */
		if (argc > 3)
			fwinfo->devpartno = (char *)argv[3];
		if (argc > 4)
			fwinfo->fs = (char *)argv[4];
		break;
	case SRC_NAND:
#ifdef CONFIG_CMD_MTDPARTS
		/* Initialize partitions */
		if (mtdparts_init()) {
			printf("Cannot initialize MTD partitions\n");
			goto _err;
		}

		/*
		 * Use partition name if provided, or else search for a
		 * partition with the same name as the OS.
		 */
		if (argc > 3)
			partname = argv[3];
		else
			partname = argv[1];
		if (find_dev_and_part(partname, &dev, &pnum, &fwinfo->part)) {
			printf("Cannot find '%s' partition\n", partname);
			goto _err;
		}
#endif
		break;
	}

	fwinfo->src = i;
	return 0;

_err:
	if (fwinfo->src == SRC_UNSUPPORTED)
		printf("Error: '%s' is not supported as source\n", argv[2]);
	else if (fwinfo->src == SRC_UNDEFINED)
		printf("Error: undefined source\n");

	return -1;
}

const char *get_source_string(int src)
{
	if (SRC_UNDEFINED != src && src < ARRAY_SIZE(src_strings))
		return src_strings[src];

	return "";
}

int get_fw_filename(int argc, char * const argv[], struct load_fw *fwinfo)
{
	switch (fwinfo->src) {
	case SRC_TFTP:
	case SRC_NFS:
		if (argc > 3) {
			fwinfo->filename = argv[3];
			return 0;
		}
		break;
	case SRC_MMC:
	case SRC_USB:
	case SRC_SATA:
		if (argc > 5) {
			fwinfo->filename = argv[5];
			return 0;
		}
		break;
	case SRC_NAND:
		if (argc > 4) {
			fwinfo->filename = argv[4];
			return 0;
		}
		break;
	case SRC_RAM:
		return 0;	/* No file is needed */
	default:
		return -1;
	}

	return -1;
}

char *get_default_filename(char *partname, int cmd)
{
	switch(cmd) {
	case CMD_DBOOT:
		if (!strcmp(partname, "linux") ||
		    !strcmp(partname, "android")) {
			return "$" CONFIG_DBOOT_DEFAULTKERNELVAR;
		}
		break;

	case CMD_UPDATE:
		if (!strcmp(partname, "uboot")) {
			return "$uboot_file";
		} else {
			/* Read the default filename from a variable called
			 * after the partition name: <partname>_file
			 */
			char varname[100];

			sprintf(varname, "%s_file", partname);
			return getenv(varname);
		}
		break;
	}

	return NULL;
}

int get_default_devpartno(int src, char *devpartno)
{
	char *dev, *part;

	switch (src) {
	case SRC_MMC:
		dev = getenv("mmcdev");
		if (dev == NULL)
			return -1;
		part = getenv("mmcpart");
		/* If mmcpart not defined, default to 1 */
		if (part == NULL)
			sprintf(devpartno, "%s:1", dev);
		else
			sprintf(devpartno, "%s:%s", dev, part);
		break;
	case SRC_USB:	// TODO
	case SRC_SATA:	// TODO
	default:
		return -1;
	}

	return 0;
}

#ifdef CONFIG_DIGI_UBI
bool is_ubi_partition(struct part_info *part)
{
	struct mtd_info *nand = &nand_info[0];
	size_t rsize = nand->writesize;
	unsigned char *page;
	unsigned long ubi_magic = 0x23494255;	/* "UBI#" */
	bool ret = false;

	/*
	 * Check if the partition is UBI formatted by reading the first word
	 * in the first page, which should contain the UBI magic "UBI#".
	 * Then verify it contains a UBI volume and get its name.
	 */
	page = malloc(rsize);
	if (page) {
		if (!nand_read_skip_bad(nand, part->offset, &rsize, NULL,
					part->size, page)) {
			unsigned long *magic = (unsigned long *)page;

			if (*magic == ubi_magic)
				ret = true;
		}
		free(page);
	}

	return ret;
}
#endif /* CONFIG_DIGI_UBI */
#endif /* CONFIG_CMD_UPDATE || CONFIG_CMD_DBOOT */

#ifdef CONFIG_CMD_UPDATE
void register_fs_otf_update_hook(int (*hook)(otf_data_t *data),
				 disk_partition_t *partition)
{
	otf_update_hook = hook;
	/* Initialize data for new transfer */
	otfd.buf = NULL;
	otfd.part = partition;
	otfd.flags = OTF_FLAG_INIT;
	otfd.offset = 0;
}

void unregister_fs_otf_update_hook(void)
{
	otf_update_hook = NULL;
}

/* On-the-fly update for files in a filesystem on mass storage media
 * The function returns:
 *	0 if the file was loaded successfully
 *	-1 on error
 */
static int write_file_fs_otf(int src, char *filename, char *devpartno)
{
	char cmd[CONFIG_SYS_CBSIZE] = "";
	unsigned long filesize;
	unsigned long remaining;
	unsigned long offset = 0;

	/* Obtain file size */
	sprintf(cmd, "size %s %s %s", src_strings[src], devpartno, filename);
	if (run_command(cmd, 0)) {
		printf("Couldn't determine file size\n");
		return -1;
	}
	filesize = getenv_ulong("filesize", 16, 0);
	remaining = filesize;

	/* Init otf data */
	otfd.loadaddr = getenv_ulong("loadaddr", 16, 0);

	while (remaining > 0) {
		debug("%lu remaining bytes\n", remaining);
		/* Determine chunk length to write */
		if (remaining > CONFIG_OTF_CHUNK) {
			otfd.len = CONFIG_OTF_CHUNK;
		} else {
			otfd.flags |= OTF_FLAG_FLUSH;
			otfd.len = remaining;
		}

		/* Load 'len' bytes from file[offset] into RAM */
		sprintf(cmd, "load %s %s $loadaddr %s %x %x", src_strings[src],
			devpartno, filename, otfd.len, (unsigned int)offset);
		if (run_command(cmd, 0)) {
			printf("Couldn't load file\n");
			return -1;
		}

		/* Write chunk */
		if (otf_update_hook(&otfd)) {
			printf("Error writing on-the-fly. Aborting\n");
			return -1;
		}

		/* Update local target offset */
		offset += otfd.len;
		/* Update remaining bytes */
		remaining -= otfd.len;
	}

	return 0;
}

/* A variable determines if the file must be loaded.
 * The function returns:
 *	LDFW_LOADED if the file was loaded successfully
 *	LDFW_NOT_LOADED if the file was not loaded, but isn't required
 *	LDFW_ERROR on error
 */
int load_firmware(struct load_fw *fwinfo)
{
	char cmd[CONFIG_SYS_CBSIZE] = "";
	char def_devpartno[] = "0:1";
	int ret;
	int fwload = FWLOAD_YES;

	/* 'fwinfo->varload' determines if the file must be loaded:
	 * - yes|NULL: the file must be loaded. Return error otherwise.
	 * - try: the file may be loaded. Return ok even if load fails.
	 * - no: skip the load.
	 */
	if (NULL != fwinfo->varload) {
		if (!strcmp(fwinfo->varload, "no"))
			return LDFW_NOT_LOADED;	/* skip load and return ok */
		else if (!strcmp(fwinfo->varload, "try"))
			fwload = FWLOAD_TRY;
	}

	/* Use default values if not provided */
	if (NULL == fwinfo->devpartno) {
		if (get_default_devpartno(fwinfo->src, def_devpartno))
			strcpy(def_devpartno, "0:1");
		fwinfo->devpartno = def_devpartno;
	}

	switch (fwinfo->src) {
	case SRC_TFTP:
		sprintf(cmd, "tftpboot %s %s", fwinfo->loadaddr,
			fwinfo->filename);
		break;
	case SRC_NFS:
		sprintf(cmd, "nfs %s $rootpath/%s", fwinfo->loadaddr,
			fwinfo->filename);
		break;
	case SRC_MMC:
	case SRC_USB:
	case SRC_SATA:
		if (otf_update_hook) {
			ret = write_file_fs_otf(fwinfo->src, fwinfo->filename,
						fwinfo->devpartno);
			goto _ret;
		} else {
			sprintf(cmd, "load %s %s %s %s", src_strings[fwinfo->src],
				fwinfo->devpartno, fwinfo->loadaddr,
				fwinfo->filename);
		}
		break;
	case SRC_NAND:
#ifdef CONFIG_DIGI_UBI
		/*
		 * If the partition is UBI formatted, use 'ubiload' to read
		 * a file from the UBIFS file system. Otherwise use a raw
		 * read using 'nand read'.
		 */
		if (is_ubi_partition(fwinfo->part)) {
			sprintf(cmd,
				"if ubi part %s;then "
					"if ubifsmount ubi0:%s;then "
						"ubifsload %s %s;"
						"ubifsumount;"
					"fi;"
				"fi;",
				fwinfo->part->name, fwinfo->part->name,
				fwinfo->loadaddr, fwinfo->filename);
		} else
#endif
		{
			sprintf(cmd, "nand read %s %s %x", fwinfo->part->name,
				fwinfo->loadaddr, (u32)fwinfo->part->size);
		}
		break;
	case SRC_RAM:
		ret = LDFW_NOT_LOADED;	/* file is already in RAM */
		goto _ret;
	default:
		return -1;
	}

	ret = run_command(cmd, 0);
_ret:
	if (FWLOAD_TRY == fwload) {
		if (ret)
			return LDFW_NOT_LOADED;
		else
			return LDFW_LOADED;
	}

	if (ret)
		return LDFW_ERROR;

	return LDFW_LOADED;	/* ok, file was loaded */
}
#endif /* CONFIG_CMD_UPDATE */

#if defined(CONFIG_SOURCE) && defined(CONFIG_AUTO_BOOTSCRIPT)
void run_auto_bootscript(void)
{
#ifdef CONFIG_CMD_NET
	int ret;
	char *bootscript;
	/* Save original timeouts */
        ulong saved_rrqtimeout_msecs = TftpRRQTimeoutMSecs;
        int saved_rrqtimeout_count = TftpRRQTimeoutCountMax;
	ulong saved_startagain_timeout = NetStartAgainTimeout;
	unsigned long saved_flags = gd->flags;
	char *retrycnt = getenv("netretry");

	bootscript = getenv("bootscript");
	if (bootscript) {
		printf("Bootscript from TFTP... ");

		/* Silence console */
		gd->flags |= GD_FLG_SILENT;
		/* set timeouts for bootscript */
		TftpRRQTimeoutMSecs = AUTOSCRIPT_TFTP_MSEC;
		TftpRRQTimeoutCountMax = AUTOSCRIPT_TFTP_CNT;
		NetStartAgainTimeout = AUTOSCRIPT_START_AGAIN;
		/* set retrycnt */
		setenv("netretry", "no");

		/* Silence net commands during the bootscript download */
		DownloadingAutoScript = 1;
		ret = run_command("tftp ${loadaddr} ${bootscript}", 0);
		/* First restore original values of global variables
		 * and then evaluate the result of the run_command */
		DownloadingAutoScript = 0;
		/* Restore original timeouts */
		TftpRRQTimeoutMSecs = saved_rrqtimeout_msecs;
		TftpRRQTimeoutCountMax = saved_rrqtimeout_count;
		NetStartAgainTimeout = saved_startagain_timeout;
		/* restore retrycnt */
		if (retrycnt)
			setenv("netretry", retrycnt);
		else
			setenv("netretry", "");
		/* Restore flags */
		gd->flags = saved_flags;

		if (ret)
			goto error;

		printf("[ready]\nRunning bootscript...\n");
		RunningAutoScript = 1;
		/* Launch bootscript */
		run_command("source ${loadaddr}", 0);
		RunningAutoScript = 0;
		return;
error:
		printf( "[not available]\n" );
	}
#endif
}
#endif

int strtou32(const char *str, unsigned int base, u32 *result)
{
	char *ep;

	*result = simple_strtoul(str, &ep, base);
	if (ep == str || *ep != '\0')
		return -EINVAL;

	return 0;
}

int confirm_prog(void)
{
	puts("Warning: Programming fuses is an irreversible operation!\n"
			"         This may brick your system.\n"
			"         Use this command only if you are sure of "
					"what you are doing!\n"
			"\nReally perform this fuse programming? <y/N>\n");

	if (getc() == 'y') {
		int c;

		putc('y');
		c = getc();
		putc('\n');
		if (c == '\r')
			return 1;
	}

	puts("Fuse programming aborted\n");
	return 0;
}

#if defined(CONFIG_OF_BOARD_SETUP)
void fdt_fixup_mac(void *fdt, char *varname, char *node, char *property)
{
	char *tmp, *end;
	unsigned char mac_addr[6];
	int i;

	if ((tmp = getenv(varname)) != NULL) {
		for (i = 0; i < 6; i++) {
			mac_addr[i] = tmp ? simple_strtoul(tmp, &end, 16) : 0;
			if (tmp)
				tmp = (*end) ? end+1 : end;
		}
		do_fixup_by_path(fdt, node, property, &mac_addr, 6, 1);
	}
}

void fdt_fixup_regulatory(void *fdt)
{
	unsigned int val;
	char *regdomain = getenv("regdomain");

	if (regdomain != NULL) {
		val = simple_strtoul(regdomain, NULL, 16);
		if (val < DIGI_MAX_CERT) {
			sprintf(regdomain, "0x%x", val);
			do_fixup_by_path(fdt, "/wireless",
					 "regulatory-domain", regdomain,
					 strlen(regdomain) + 1, 1);
		}
	}
}
#endif /* CONFIG_OF_BOARD_SETUP */

void fdt_fixup_uboot_info(void *fdt) {
	do_fixup_by_path(fdt, "/", "digi,uboot,version", version_string,
			 strlen(version_string), 1);
#ifdef CONFIG_DYNAMIC_ENV_LOCATION
	do_fixup_by_path(fdt, "/", "digi,uboot,dynamic-env", NULL, 0, 1);
#endif
}

const char *get_filename_ext(const char *filename)
{
	const char *dot;

	if (NULL == filename)
		return "";

	dot = strrchr(filename, '.');
	if (!dot || dot == filename)
		return "";

	return dot + 1;
}

#define STR_HEX_CHUNK			8
/*
 * Convert string with hexadecimal characters into a hex number
 * @in: Pointer to input string
 * @out Pointer to output number array
 * @len Number of elements in the output array
*/
void strtohex(char *in, unsigned long *out, int len)
{
	char tmp[] = "ffffffff";
	int i, j;

	for (i = 0, j = 0; j < len; i += STR_HEX_CHUNK, j++) {
		strncpy(tmp, &in[i], STR_HEX_CHUNK);
		out[j] = cpu_to_be32(simple_strtol(tmp, NULL, 16));
	}
}

/*
 * Verifies if a MAC address has a default value (dummy) and prints a warning
 * if so.
 * @var: Variable to check
 * @default_mac: Default MAC to check with (as a string)
 */
void verify_mac_address(char *var, char *default_mac)
{
	char *mac;

	mac = getenv(var);
	if (NULL == mac)
		printf("   WARNING: MAC not set in '%s'\n", var);
	else if (!strcmp(mac, default_mac))
		printf("   WARNING: Dummy default MAC in '%s'\n", var);
}

/*
 * Check if the storage media address/block is empty
 * @in: Address/offset in media
 * @in: Partition index, only applies for MMC
 * The function returns:
 *	1 if the block is empty
 *	0 if the block is not empty
 *	-1 on error
 */
int media_block_is_empty(u32 addr, uint hwpart)
{
	size_t len;
	int ret = -1;
	int i;
	uint64_t empty_pattern = 0;
	uint64_t *readbuf = NULL;

	if (strcmp(CONFIG_SYS_STORAGE_MEDIA, "nand") == 0)
		empty_pattern = ~0;

	len = media_get_block_size();
	if (!len)
		return ret;

	readbuf = malloc(len);
	if (!readbuf)
		return ret;

	if (media_read_block(addr, (unsigned char *)readbuf, hwpart))
		goto out_free;

	ret = 1;	/* media block empty */
	for (i = 0; i < len / 8; i++) {
		if (readbuf[i] != empty_pattern) {
			ret = 0;	/* media block not empty */
			break;
		}
	}
out_free:
	free(readbuf);
	return ret;
}

/**
 * Parses a string into a number. The number stored at ptr is
 * potentially suffixed with K (for kilobytes, or 1024 bytes),
 * M (for megabytes, or 1048576 bytes), or G (for gigabytes, or
 * 1073741824). If the number is suffixed with K, M, or G, then
 * the return value is the number multiplied by one kilobyte, one
 * megabyte, or one gigabyte, respectively.
 *
 * @param ptr where parse begins
 * @param retptr output pointer to next char after parse completes (output)
 * @return resulting unsigned int
 */
u64 memsize_parse(const char *const ptr, const char **retptr)
{
	u64 ret = simple_strtoull(ptr, (char **)retptr, 0);

	switch (**retptr) {
	case 'G':
	case 'g':
		ret <<= 10;
	case 'M':
	case 'm':
		ret <<= 10;
	case 'K':
	case 'k':
		ret <<= 10;
		(*retptr)++;
	default:
		break;
	}

	return ret;
}
