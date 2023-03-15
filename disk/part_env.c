// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2023
 * Guoxin Pu, pugokushin@gmail.com
 */

/*
 * Support for partitions declared in environment.
 *
 * The format is similar to mtdparts= and blkdevparts= in linux but without the leading blkdevname
 * 
 * Example: envparts_mmc1=880K(bootloader),80K(dtb),64K(env),15M(initramfs),50M(kernel),-(data)
 * 
 * It's then very easy to pass it to kernel like 'setenv bootargs blkdevparts=mmcblk2:${envparts_mmc1} root=/dev/mmcblk2p6'
 */

#include <common.h>
#include <blk.h>
#include <env.h>
#include <part.h>
#include <string.h>

#define PART_ENV_KEY_PREFIX		"envparts_"
#define PART_ENV_KEY_PREFIX_LEN 9
#define PART_ENV_KEY_MAX_LEN	32
#define PART_DEFINITION_PART_MAX_LEN	32
#define PART_ENV_KEY_SUFFIX_MAX_LEN	PART_ENV_KEY_MAX_LEN - PART_ENV_KEY_PREFIX_LEN
#define PART_ENV_SECTOR_SIZE	512

struct part_env {
	ulong offset; /* Offset in bytes */
	ulong size; /* Size in bytes */
	uchar name[PART_NAME_LEN];
};

static char *part_get_definition_env(struct blk_desc *dev_desc)
{
	char const *uclass_name;
	char env_key[PART_ENV_KEY_MAX_LEN] = "envparts_";
	char *env_definition;
	/* For now we only support mmc */
	switch (dev_desc->uclass_id) {
	case UCLASS_MMC:
		if (dev_desc->hwpart) {
			pr_err("Refuse to parse eMMC boot partitions\n");
			return NULL;
		}
		break;
	default:
		pr_debug("Skipped dev uclass_id %d devnum %d since it's not mmc\n", dev_desc->uclass_id, dev_desc->devnum);
		return NULL;
	}
	uclass_name = blk_get_uclass_name(dev_desc->uclass_id);
	if (!uclass_name) {
		pr_err("Failed to get uclass name for uclass_id %d devnum %d\n", dev_desc->uclass_id, dev_desc->devnum);
		return NULL;
	}
	if (uclass_name[0] == '(') {
		pr_err("uclass name '%s' for uclass_id %d devnum %d is not acceptable\n", uclass_name, dev_desc->uclass_id, dev_desc->devnum);
		return NULL;
	}
	snprintf(env_key + PART_ENV_KEY_PREFIX_LEN, PART_ENV_KEY_SUFFIX_MAX_LEN, "%s%d", uclass_name, dev_desc->devnum);
	pr_debug("Parsing parts from u-boot env '%s'\n", env_key);
	env_definition = env_get(env_key);
	if (!env_definition) {
		pr_err("Failed to get environment '%s'\n", env_key);
		return NULL;
	}
	return env_definition;
}

static int part_get_definition_part(struct blk_desc *dev_desc, int part, struct part_env *part_env)
{
	char *env_definition = part_get_definition_env(dev_desc);
	ulong const size_total = dev_desc->lba * dev_desc->blksz;
	char *part_definition_start = NULL;
	char *bracket_left = NULL;
	char *bracket_right = NULL;
	char *offset_at = NULL;
	unsigned part_id = 0;
	bool parse_this = false;
	ulong size_this = 0;
	ulong offset_this = 0;
	ulong end_last = 0;
	char *size_end;
	if (!env_definition) {
		return -1;
	}
	for (char *c = env_definition;; ++c) {
		switch (*c) {
			case ',':
			case '\0':
				switch (part_definition_start[0]) {
					case '-':
						size_this = size_total - end_last;
						break;
					case '0'...'9':
						size_this = simple_strtoul(part_definition_start, &size_end, 10);
						switch (*size_end) {
						case 'K':
						case 'k':
							size_this *= 0x400UL;
							break;
						case 'M':
						case 'm':
							size_this *= 0x100000UL;
							break;
						case 'G':
						case 'g':
							size_this *= 0x40000000UL;
							break;
						case 'T':
						case 't':
							size_this *= 0x10000000000UL;
							break;
						case 'P':
						case 'p':
							size_this *= 0x4000000000000UL;
							break;
						case 'E':
						case 'e':
							size_this *= 0x1000000000000000UL;
							break;
						case '@':
						case ',':
						case '(':
						case '\0':
							break;
						default:
							pr_err("Partition %d has a unrecognized suffix '%c' in size in env '%s'\n", part_id, *size_end, env_definition);
							return -1;
						}
						break;
					default:
						pr_err("Partition %d does not have valid size definition in env: '%s'\n", part_id, env_definition);
						return -1;
				}
				if (offset_at) {
					offset_this = simple_strtoul(offset_at + 1, &size_end, 10);
					switch (*size_end) {
						case 'K':
						case 'k':
							size_this *= 0x400UL;
							break;
						case 'M':
						case 'm':
							size_this *= 0x100000UL;
							break;
						case 'G':
						case 'g':
							size_this *= 0x40000000UL;
							break;
						case 'T':
						case 't':
							size_this *= 0x10000000000UL;
							break;
						case 'P':
						case 'p':
							size_this *= 0x4000000000000UL;
							break;
						case 'E':
						case 'e':
							size_this *= 0x1000000000000000UL;
							break;
						case ',':
						case '(':
						case '\0':
							break;
						default:
							pr_err("Partition %d has a unrecognized suffix '%c' in offset in env '%s'\n", part_id, *size_end, env_definition);
							return -1;
						}
						break;
				} else {
					offset_this = end_last;
				}
				if (offset_this > size_total) {
					pr_warn("Partition %d's offset %lu exceeds disk's end in env '%s', shrink to %lu\n", part_id, offset_this, env_definition, size_total);
					offset_this = size_total;
				}
				end_last = size_this + offset_this;
				if (end_last > size_total) {
					pr_warn("Partition %d's end exceeds disk's end in env '%s', shrink size down\n", part_id, env_definition);
					size_this -= (end_last - size_total);
					end_last = size_total;
				}
				if (size_this % PART_ENV_SECTOR_SIZE) {
					pr_err("Partition %d's size %lu is not a multiply of base sector size %d in env '%s'\n", part_id, size_this, PART_ENV_SECTOR_SIZE, env_definition);
					return -1;
				}
				if (offset_this % PART_ENV_SECTOR_SIZE) {
					pr_err("Partition %d's offset %lu is not a multiply of base sector size %d in env '%s'\n", part_id, offset_this, PART_ENV_SECTOR_SIZE, env_definition);
					return -1;
				}
				if (parse_this) {
					if (bracket_left) {
						if (bracket_right) {
							ulong const part_name_len = bracket_right - bracket_left - 1;
							if (part_name_len >= PART_NAME_LEN) {
								pr_err("Partition %d's name is too long in env '%s'\n", part_id, env_definition);
								return -1;
							}
							strncpy(part_env->name, bracket_left + 1, part_name_len);
							part_env->name[part_name_len] = '\0';
						} else {
							pr_err("Partition %d has unpaired bracket in env '%s'\n", part_id, env_definition);
							return -1;
						}
					} else if (bracket_right) {
						pr_err("Partition %d has unpaired bracket in env '%s'\n", part_id, env_definition);
						return -1;
					}
					part_env->size = size_this;
					part_env->offset = offset_this;
					return 0;
				}
				if (*c == '\0') {
					/* Not found at the end */
					return -1;
				}
				part_definition_start = NULL;
				break;
			case '(':
				if (parse_this) {
					if (bracket_left) {
						pr_err("Multiple occurance of '(' in single entry: '%s'\n", env_definition);
						return -1;
					}
					bracket_left = c;
				}
				break;
			case ')':
				if (parse_this) {
					if (bracket_right) {
						pr_err("Multiple occurance of ')' in single entry: '%s'\n", env_definition);
						return -1;
					}
					bracket_right = c;
				}
				break;
			case '@':
				if (offset_at) {
					if (offset_at) {
						pr_err("Multiple occurance of '@' in single entry: '%s'\n", env_definition);
						return -1;
					}
					offset_at = c;
				}
				break;
			default:
				if (!part_definition_start) {
					if (++part_id > part) {
						pr_err("Partition number '%d' does not exist in env '%s'\n", part_id, env_definition);
						return -1;
					}
					if (part_id == part) {
						parse_this = true;
						bracket_left = NULL;
						bracket_right = NULL;
					} else {
						parse_this = false;
					}
					part_definition_start = c;
					size_this = 0;
					offset_this = 0;
				}
				break;
		}
	}
}

static int __maybe_unused part_get_info_env(struct blk_desc *dev_desc, int part,
		      struct disk_partition *info)
{
	struct part_env part_env;
	if (part_get_definition_part(dev_desc, part, &part_env)) {
		return -1;
	}
	strncpy(info->name, part_env.name, PART_NAME_LEN);
	info->blksz = PART_ENV_SECTOR_SIZE;
	info->start = part_env.offset / PART_ENV_SECTOR_SIZE;
	info->size = part_env.size / PART_ENV_SECTOR_SIZE;
	return 0;
}

static void __maybe_unused part_print_env(struct blk_desc *dev_desc)
{
	struct part_env part_env;
	int part_id = 0;
	printf("Part\tStart Sector\tNum Sectors\tName\n");
	while (!part_get_definition_part(dev_desc, ++part_id, &part_env)) {
		printf("%3d\t%-10" LBAFlength "u\t%-10" LBAFlength "u\t%s\n", part_id, part_env.offset / 512, part_env.size / 512, part_env.name);
	}
}



static int part_test_env(struct blk_desc *dev_desc)
{
	if (part_get_definition_env(dev_desc))
		return 0;
	else
		return -1;
}


U_BOOT_PART_TYPE(env) = {
	.name		= "ENV",
	.part_type	= PART_TYPE_ENV,
	.max_entries	= GPT_ENTRY_NUMBERS,
	.get_info	= part_get_info_ptr(part_get_info_env),
	.print		= part_print_ptr(part_print_env),
	.test		= part_test_env,
};