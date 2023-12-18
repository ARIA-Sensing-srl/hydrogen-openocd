/* SPDX-License-Identifier: GPL-2.0-or-later */

/***************************************************************************
 *   Copyright (C) 2023 by Cover Sistemi srl                               *
 ***************************************************************************/

#ifndef OPENOCD_FLASH_NOR_HYDROGEN_H
#define OPENOCD_FLASH_NOR_HYDROGEN_H


/* Common hydrogen flash and memory parameters */
#define HYDROGEN_FLASH_BASE_ADDR   0x00000000
#define HYDROGEN_FLASH_SIZE_INFO   0x4003002c
#define HYDROGEN_SRAM_SIZE_INFO    0x40082250
#define HYDROGEN_ALGO_BASE_ADDRESS 0x00ff0000

/* hydrogen v1 version specific parameters */
#define HYDROGEN_V1_MAX_SECTORS   256
#define HYDROGEN_V1_SECTOR_LENGTH 0x10000
#define HYDROGEN_V1_ALGO_BUFFER_0 0x20001c00
#define HYDROGEN_V1_ALGO_BUFFER_1 0x20002c00
#define HYDROGEN_V1_ALGO_PARAMS_0 0x20001bd8
#define HYDROGEN_V1_ALGO_PARAMS_1 0x20001bec
#define HYDROGEN_V1_WORKING_SIZE  (HYDROGEN_V1_ALGO_BUFFER_1 + HYDROGEN_V1_SECTOR_LENGTH - \
							 HYDROGEN_ALGO_BASE_ADDRESS)

/* HYDROGEN flash helper algorithm buffer flags */
#define HYDROGEN_BUFFER_EMPTY 0x00000000
#define HYDROGEN_BUFFER_FULL  0xffffffff

/* HYDROGEN flash helper algorithm commands */
#define HYDROGEN_CMD_NO_ACTION                     0
#define HYDROGEN_CMD_ERASE_ALL                     1
#define HYDROGEN_CMD_PROGRAM                       2
#define HYDROGEN_CMD_ERASE_AND_PROGRAM             3
#define HYDROGEN_CMD_ERASE_AND_PROGRAM_WITH_RETAIN 4
#define HYDROGEN_CMD_ERASE_SECTORS                 5

/* hydrogen and hydrogen/2  device types */
#define HYDROGEN_NO_TYPE 0 /* Device type not determined yet */
#define HYDROGEN_V1_TYPE      1 /*  device */

/* Flash helper algorithm parameter block struct */
#define HYDROGEN_STATUS_OFFSET 0x0c
struct hydrogen_algo_params {
	uint8_t address[4];
	uint8_t length[4];
	uint8_t command[4];
	uint8_t status[4];
};

#endif /* OPENOCD_FLASH_NOR_HYDROGEN_H */
