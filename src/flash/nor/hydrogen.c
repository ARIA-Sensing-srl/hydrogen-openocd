/* SPDX-License-Identifier: GPL-2.0-or-later */

/***************************************************************************
 *   Copyright (C) 2023 by Cover Sistemi srl                               *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"


#include <helper/binarybuffer.h>
//#include <helper/time_support.h>
#include <target/algorithm.h>
#include <target/image.h>
#include "hydrogen.h"
#include <helper/time_support.h>
#include <target/riscv/riscv.h>
#include <target/armv7m.h>



//#define FLASH_TIMEOUT 2000
#define FLASH_DEFAULT_SECTOR_SiZE 4096
#define DEFAULT_TIMEOUT_ms 2000
#define MASS_ERASE_TIMEOUT_ms 60000
#define HYDROGEN_DRIVER_DEBUG 2
#define HYDROGEN_FLASH_COMMAND_IDLE 0x00
#define HYDROGEN_FLASH_COMMAND_READ_FLASHID 0x01
#define HYDROGEN_FLASH_COMMAND_WRITE_PAGE 0x02
#define HYDROGEN_FLASH_COMMAND_READ_PAGE 0x03
#define HYDROGEN_FLASH_COMMAND_ERASE_SECTOR 0x0A
#define HYDROGEN_FLASH_COMMAND_ERASE_ALL 0xAA
#define HYDROGEN_FLASH_COMMAND_VERIFY_ALL_BLANK 0x55
#define HYDROGEN_FLASH_COMMAND_VERIFY_SECTOR_AFTER_ERASE 0xA0
#define HYDROGEN_FLASH_PAGE_SIZE 256
#define HYDROGEN_FLASH_NUM_SECTORS 256
#define HYDROGEN_FLASH_SECTORS_LENGTH 256

#define HYDROGEN_RAM_ADDRESS_BUFFER 		0x1C018000
#define HYDROGEN_RAM_ADDRESS_BUSY 		(HYDROGEN_RAM_ADDRESS_BUFFER)
#define HYDROGEN_RAM_ADDRESS_COMMAND 		(HYDROGEN_RAM_ADDRESS_BUFFER+0x04)
#define HYDROGEN_RAM_ADDRESS_CMD_DATA 	(HYDROGEN_RAM_ADDRESS_BUFFER+0x08)
#define HYDROGEN_RAM_ADDRESS_CMD_SIZE 	(HYDROGEN_RAM_ADDRESS_BUFFER+0x0C)
#define HYDROGEN_RAM_ADDRESS_IMG_BUF		(HYDROGEN_RAM_ADDRESS_BUFFER+0x10)
#define HYDROGEN_RAM_SIZE_IMG_BUF		256

#define FLASH_ID_SIZE 8

#define HYDROGEN_DEVICE_TYPE1 0x01170117
#define HYDROGEN_DEVICE_TYPE2 0x9d169d16

#undef HYDROGEN_ALGO_BASE_ADDRESS
#define HYDROGEN_ALGO_BASE_ADDRESS 0x1c000000
#define HYDROGEN_ALGO_ENTRY_ADDRESS 0x1c000080

#define HYDROGEN_WORKING_SIZE 0x1A000
#define HYDROGEN_ALGO_BUFFER (HYDROGEN_RAM_ADDRESS_BUFFER)
#define HYDROGEN_ALGO_PARAMS (HYDROGEN_RAM_ADDRESS_BUFFER+0x101)
	


struct hydrogen_bank {
	const char *family_name;
	struct riscv_info riscv_algo_info;
	uint32_t user_id;
	uint32_t device_type;
	uint32_t sector_length;
	bool probed;
	struct working_area *working_area;
	const uint8_t *algo_code;
	uint32_t algo_size;
	uint32_t algo_working_size;
	uint32_t buffer_addr;
	uint32_t params_addr;
};

///* Flash helper algorithm for hydrogen_v1 targets */
//static const uint8_t hydrogen_v1_algo[] = {
//#include "../../../contrib/loaders/flash/hydrogen/hydrogen_v1_algo.inc"
//};
static const uint8_t hydrogen_algo[] = {
#include "../../../contrib/loaders/flash/hydrogen/hydrogen_algo.inc"
};


static int hydrogen_auto_probe(struct flash_bank *bank);

static int hydrogen_wait_algo_done(struct flash_bank *bank, int timeout_ms)
{
	struct target *target = bank->target;
	struct hydrogen_bank *hydrogen_bank = bank->driver_priv;

	uint32_t status = 1;
	long long start_ms;
	long long elapsed_ms;

	int retval = ERROR_OK;

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen_wait_algo_done \n");

	start_ms = timeval_ms();
	while (status) {
		retval = target_read_u32(target, HYDROGEN_RAM_ADDRESS_COMMAND, &status); //wait until idle
		if (retval != ERROR_OK)
			return retval;

		elapsed_ms = timeval_ms() - start_ms;
		if (elapsed_ms > 500)
			keep_alive();
		if (elapsed_ms > timeout_ms)
			break;
	};

	if (status != HYDROGEN_BUFFER_EMPTY) {
		LOG_ERROR("%s: Flash operation failed", hydrogen_bank->family_name);
		return ERROR_FAIL;
	}
	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from hydrogen_wait_algo_done \n");

	return ERROR_OK;
}

static int hydrogen_init(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct hydrogen_bank *hydrogen_bank = bank->driver_priv;

	int retval;

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen_init: copy the algorith into CPU \n");
	
	
	if (target->state != TARGET_HALTED) {
		(void)target_halt(target);
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}


	/* Make sure we've probed the flash to get the device and size */
	retval = hydrogen_auto_probe(bank);
	if (retval != ERROR_OK)
		return retval;

	/* Check for working area to use for flash helper algorithm */
	target_free_working_area(target, hydrogen_bank->working_area);
	hydrogen_bank->working_area = NULL;

	retval = target_alloc_working_area(target, hydrogen_bank->algo_working_size,
				&hydrogen_bank->working_area);
	if (retval != ERROR_OK){
		if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("target_alloc_working_area fail\n");
		return retval;
	}

	/* Confirm the defined working address is the area we need to use */
	if (hydrogen_bank->working_area->address != HYDROGEN_ALGO_BASE_ADDRESS){
		if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Working area not match error\n");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	/* Write flash helper algorithm into target memory */
	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Start writing loader helper\n");
	retval = target_write_buffer(target, HYDROGEN_ALGO_BASE_ADDRESS,
				hydrogen_bank->algo_size, hydrogen_bank->algo_code);
	if (retval != ERROR_OK) {
		LOG_ERROR("%s: Failed to load flash helper algorithm",
			hydrogen_bank->family_name);
		target_free_working_area(target, hydrogen_bank->working_area);
		hydrogen_bank->working_area = NULL;
		return retval;
	}
	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("End writing loader helper\n");

//	/* Initialize the ARMv7 specific info to run the algorithm */
//		hydrogen_bank->armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
//		hydrogen_bank->armv7m_info.core_mode = ARM_MODE_THREAD;

	/* Begin executing the flash helper algorithm */
	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Start loader helper algo\n");
	//retval = target_start_algorithm(target, 0, NULL, 0, NULL,
	//			HYDROGEN_ALGO_ENTRY_ADDRESS, 0, &hydrogen_bank->riscv_algo_info);
	target_resume(target, 0 , HYDROGEN_ALGO_ENTRY_ADDRESS, 1, 1);
	
	retval=ERROR_OK;
	if (retval != ERROR_OK) {
		LOG_ERROR("%s: Failed to start flash helper algorithm",
			hydrogen_bank->family_name);
		target_free_working_area(target, hydrogen_bank->working_area);
		hydrogen_bank->working_area = NULL;
		return retval;
	}

	/*
	 * At this point, the algorithm is running on the target and
	 * ready to receive commands and data to flash the target
	 */

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from hydrogen_init \n");

	return retval;
}

static int hydrogen_quit(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct hydrogen_bank *hydrogen_bank = bank->driver_priv;

	int retval;
	if(HYDROGEN_DRIVER_DEBUG) LOG_INFO("Enter in hydrogen_quit \n");

	/* Regardless of the algo's status, attempt to halt the target */
	(void)target_halt(target);

	/* Now confirm target halted and clean up from flash helper algorithm */
	retval = target_wait_algorithm(target, 0, NULL, 0, NULL, 0, DEFAULT_TIMEOUT_ms,
				&hydrogen_bank->riscv_algo_info);

	target_free_working_area(target, hydrogen_bank->working_area);
	hydrogen_bank->working_area = NULL;

	if(HYDROGEN_DRIVER_DEBUG) LOG_INFO("Exit from hydrogen_quit \n");
	return retval;
}


static int hydrogen_mass_erase(struct flash_bank *bank)
{
	struct target *target = bank->target;

	int retval;

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen_mass_erase \n");

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = hydrogen_init(bank);
	if (retval != ERROR_OK)
		return retval;

	target_write_u32(target,HYDROGEN_RAM_ADDRESS_COMMAND,HYDROGEN_FLASH_COMMAND_ERASE_ALL);
	retval = hydrogen_wait_algo_done(bank, MASS_ERASE_TIMEOUT_ms);

		/* Regardless of errors, try to close down algo */
		(void)hydrogen_quit(bank);

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from hydrogen_mass_erase \n");

	return retval;
}

FLASH_BANK_COMMAND_HANDLER(hydrogen_flash_bank_command)
{
	struct hydrogen_bank *hydrogen_bank;

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen FLASH_BANK_COMMAND_HANDLER  \n");

/*
//standard options (6 args)
0:driver name
1:flash base
2:flash size
3:chip_width
4:bus_width
5:target
//extended (7 args)
6:erase_sector size
*/
	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	hydrogen_bank = malloc(sizeof(struct hydrogen_bank));
	if (!hydrogen_bank)
		return ERROR_FAIL;
	uint32_t baseaddress;
	uint32_t flashsize;
	uint32_t sectorsize;
	
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], baseaddress);
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], flashsize);
	
	if (CMD_ARGC == 7){
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[6], sectorsize);
	}else{
		
		sectorsize = FLASH_DEFAULT_SECTOR_SiZE;
		LOG_INFO("Use default sector size %d\n", sectorsize);
	}
	
	
	
	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Args %d, baseaddress %d, flashsize %d, sectorsize %d\n",CMD_ARGC, baseaddress, flashsize, sectorsize);
	

	/* Initialize private flash information */
	memset((void *)hydrogen_bank, 0x00, sizeof(struct hydrogen_bank));
	hydrogen_bank->family_name = "hydrogen";
	hydrogen_bank->device_type = HYDROGEN_NO_TYPE;
	hydrogen_bank->sector_length = sectorsize;
	
	//init memory
	
	int sector_length=sectorsize;
	int num_sectors=flashsize/sectorsize;
   	int max_sectors=num_sectors;

	if (num_sectors > max_sectors)
		num_sectors = max_sectors;


	bank->sectors = malloc(sizeof(struct flash_sector) * num_sectors);
	if (!bank->sectors){
		//(void)hydrogen_quit(bank);
		return ERROR_FAIL;
	}

	bank->base = baseaddress;
	bank->num_sectors = num_sectors;
	bank->size = num_sectors * sector_length;
	bank->write_start_alignment = 0;
	bank->write_end_alignment = 0;
	hydrogen_bank->sector_length = sector_length;

	for (int i = 0; i < num_sectors; i++) {
		bank->sectors[i].offset = i * sector_length;
		bank->sectors[i].size = sector_length;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 0;
	}

	/* Finish initialization of bank */
	bank->driver_priv = hydrogen_bank;
	bank->next = NULL;

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from hydrogen FLASH_BANK_COMMAND_HANDLER  \n");

	return ERROR_OK;
}

static int hydrogen_erase(struct flash_bank *bank, unsigned int first,
		unsigned int last)
{
	struct target *target = bank->target;

	int retval=ERROR_OK;
	
	if(HYDROGEN_DRIVER_DEBUG==2)
	  {
	    LOG_INFO("Enter in hydrogen_erase with first=%d and last=%d\n", first, last);
	    if(first == 0 && last == 1)
	      {
		LOG_INFO("Default autoerase in running");
	      }
	  }

	/*if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}*/

	
	/* Do a mass erase if user requested all sectors of flash */
	if ((first == 0) && (last == (bank->num_sectors - 1))) {
	  /* Request mass erase of flash */
	  return hydrogen_mass_erase(bank);
	}
	
	(void)hydrogen_init(bank);

	for(unsigned int i=first; i<last;i=i+1)
	  {
	    //write to register which sector has to be deleted
	    target_write_u32(target,HYDROGEN_RAM_ADDRESS_CMD_DATA,(uint32_t)i);
	    target_write_u32(target,HYDROGEN_RAM_ADDRESS_COMMAND,HYDROGEN_FLASH_COMMAND_ERASE_SECTOR);
	    //read the busy signal until it is 0
	    retval=hydrogen_wait_algo_done(bank, DEFAULT_TIMEOUT_ms);	    
	  }
	  
		/* Regardless of errors, try to close down algo */
		(void)hydrogen_quit(bank);

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from in hydrogen_erase \n");

	return retval;
}

static int hydrogen_write(struct flash_bank *bank, const uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct hydrogen_bank *hydrogen_bank __attribute__((unused))  = bank->driver_priv;
	

	uint32_t size = 0;
	long long start_ms;
	long long elapsed_ms;

	int retval;

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen_write with *buffer=0x%x, offset=0x%x, count=%d\n", *buffer, offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = hydrogen_init(bank);
	if (retval != ERROR_OK)
		return retval;

	if(count<=0) retval=ERROR_OK;
	else
	  {
	    LOG_INFO(" Write requested data, ping-ponging between two buffers ");
	    //reset the data buffer
	      
		start_ms = timeval_ms();
		while (count > 0) {
		  
		  
		  
	
		  if (count > HYDROGEN_RAM_SIZE_IMG_BUF){
		    size = HYDROGEN_RAM_SIZE_IMG_BUF;
		    }
		  else{
		    size = count;
		    }
		    
		    if(HYDROGEN_DRIVER_DEBUG)
		    {
		      LOG_INFO("Prog address=%d, count=%d\n", offset + (uint32_t)bank->base, size);
		    }
		  
		  /* Put next block of data to flash into RAM buffer */
		  retval = target_write_buffer(target, HYDROGEN_RAM_ADDRESS_IMG_BUF,
					       size, buffer);
		  if (retval != ERROR_OK) {
		    LOG_ERROR("Unable to write data to target memory");
		    break;
		  }

		  //fill command with number of page to write and send write operation
		  //set buffer
		  target_write_u32(target,HYDROGEN_RAM_ADDRESS_CMD_DATA,offset + (uint32_t)bank->base);
  		  target_write_u32(target,HYDROGEN_RAM_ADDRESS_CMD_SIZE,size);
		  target_write_u32(target,HYDROGEN_RAM_ADDRESS_COMMAND,HYDROGEN_FLASH_COMMAND_WRITE_PAGE);
		  
	
		  /* Wait for next ping pong buffer to be ready */
		  retval = hydrogen_wait_algo_done(bank,DEFAULT_TIMEOUT_ms);
		  if (retval != ERROR_OK)
		    break;
		  
		  count -= size;
		  buffer += size;
		  offset += size;
		  
		  
		  elapsed_ms = timeval_ms() - start_ms;
		  if (elapsed_ms > 500){
		    keep_alive();
		    start_ms = timeval_ms();
		   }
		}
	
		/* If no error yet, wait for last buffer to finish */
		if (retval == ERROR_OK) {
			retval = hydrogen_wait_algo_done(bank,DEFAULT_TIMEOUT_ms);
		}
	
			/* Regardless of errors, try to close down algo */
			(void)hydrogen_quit(bank);
	 }
	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from hydrogen_write \n");

	return retval;
}

static int hydrogen_read(struct flash_bank *bank, uint8_t *buffer,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct hydrogen_bank *hydrogen_bank __attribute__((unused)) = bank->driver_priv;

	uint32_t size = 0;
	long long start_ms;
	long long elapsed_ms;

	int retval;


	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen_read with *buffer=0x%x, offset=0x%x, count=%d\n", *buffer, offset, count);

	//clean the calues on buffer:
	for(uint32_t xx=0;xx<count;xx=xx+1) buffer[xx]=255;
	
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = hydrogen_init(bank);
	if (retval != ERROR_OK)
		return retval;

	if(count<=0) retval=ERROR_OK;
	else
	  {
	    LOG_INFO(" Read requested data, ping-ponging between two buffers ");
	    //reset the data buffer
	      
		start_ms = timeval_ms();
		while (count > 0) {
		  if(HYDROGEN_DRIVER_DEBUG)  LOG_INFO("Read offset %d",offset);
		  //fill command with number of page to read and send read_page command operation
		  
		  
		  if (count > HYDROGEN_RAM_SIZE_IMG_BUF)
		    size = HYDROGEN_RAM_SIZE_IMG_BUF;
		  else
		    size = count;
		  
		  target_write_u32(target,HYDROGEN_RAM_ADDRESS_CMD_DATA,offset + (uint32_t)bank->base);
		  target_write_u32(target,HYDROGEN_RAM_ADDRESS_CMD_SIZE,size);
		  target_write_u32(target,HYDROGEN_RAM_ADDRESS_COMMAND,HYDROGEN_FLASH_COMMAND_READ_PAGE);
		  hydrogen_wait_algo_done(bank,DEFAULT_TIMEOUT_ms);
		  
		  retval = target_read_buffer(target, HYDROGEN_RAM_ADDRESS_IMG_BUF, size, buffer);		  

		  if (retval != ERROR_OK) {
		    LOG_ERROR("Unable to read data from ibex RAM buffer ");
		    break;
		  }

		  count -= size;
		  buffer += size;
		  offset += size;
		  
		  elapsed_ms = timeval_ms() - start_ms;
		  if (elapsed_ms > 500){
		    keep_alive();
		    start_ms =  timeval_ms();
		    }
		}
	
		/* If no error yet, wait for last buffer to finish */
		if (retval == ERROR_OK) {
			retval = hydrogen_wait_algo_done(bank,DEFAULT_TIMEOUT_ms);
		}
	
			/* Regardless of errors, try to close down algo */
			(void)hydrogen_quit(bank);
	 }
//	for(uint32_t aaa=0;aaa<count;aaa=aaa+1)
//	  {
//	    LOG_INFO("count=%d index=%d valBuffer=0x%x ",count, aaa,buffer[aaa]);
//	  }

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from hydrogen_read \n");
	return retval;
}


static int hydrogen_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct hydrogen_bank *hydrogen_bank = bank->driver_priv;

	/*uint32_t sector_length;
	int num_sectors;
	int max_sectors;*/
	uint8_t  command_output[FLASH_ID_SIZE];

//     	int i;


	
	
	hydrogen_bank->algo_code = hydrogen_algo;
	hydrogen_bank->algo_size = sizeof(hydrogen_algo);
	hydrogen_bank->algo_working_size = HYDROGEN_WORKING_SIZE;
	hydrogen_bank->buffer_addr = HYDROGEN_ALGO_BUFFER;
	hydrogen_bank->params_addr = HYDROGEN_ALGO_PARAMS;
	
	/* We've successfully determined the stats on the flash bank */
	hydrogen_bank->probed = true;

	
	int retval = hydrogen_init(bank);
	if (retval != ERROR_OK)
		return retval;
	
	
	retval = target_write_u32(target, HYDROGEN_RAM_ADDRESS_COMMAND, HYDROGEN_FLASH_COMMAND_READ_FLASHID);//command to read flash info
	if (retval){
		(void)hydrogen_quit(bank);
		return ERROR_FAIL;
	}
	//wait until command executed:
	hydrogen_wait_algo_done(bank,DEFAULT_TIMEOUT_ms);

    	target_read_buffer(target, HYDROGEN_RAM_ADDRESS_IMG_BUF, FLASH_ID_SIZE, command_output);//output info
	   


	

	hydrogen_bank->device_type=0;
	hydrogen_bank->device_type = (command_output[0]<<24) + (command_output[1]<<16) + (command_output[2]<<8) + command_output[3];

	if(HYDROGEN_DRIVER_DEBUG) LOG_INFO("Assigned device_type num= 0x%x 0x%x 0x%x 0x%x 0x%x\n", hydrogen_bank->device_type, command_output[0],command_output[1], command_output[2],command_output[3]);
	

	/*sector_length=HYDROGEN_FLASH_PAGE_SIZE;
    	num_sectors=HYDROGEN_FLASH_NUM_SECTORS;
   	max_sectors=num_sectors;

	if (num_sectors > max_sectors)
		num_sectors = max_sectors;


	bank->sectors = malloc(sizeof(struct flash_sector) * num_sectors);
	if (!bank->sectors){
		//(void)hydrogen_quit(bank);
		return ERROR_FAIL;
	}

	bank->base = 0;
	bank->num_sectors = num_sectors;
	bank->size = num_sectors * sector_length;
	bank->write_start_alignment = 0;
	bank->write_end_alignment = 0;
	hydrogen_bank->sector_length = sector_length;

	for (i = 0; i < num_sectors; i++) {
		bank->sectors[i].offset = i * sector_length;
		bank->sectors[i].size = sector_length;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 0;
	}*/

	
	/* If we fall through to here, then all went well */
	//if(HYDROGEN_DRIVER_DEBUG) LOG_INFO("Exit from hydrogen_probe with sector_length=%d, num_sectors=%d, bank->size=%d\n",sector_length,num_sectors,bank->size);
	(void)hydrogen_quit(bank);

	return ERROR_OK;
}

static int hydrogen_auto_probe(struct flash_bank *bank)
{
	struct hydrogen_bank *hydrogen_bank = bank->driver_priv;

	int retval = ERROR_OK;
	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen_auto_probe \n");

	if (!hydrogen_bank->probed)
		retval = hydrogen_probe(bank);

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from hydrogen_auto_probe \n");
	return retval;
}

static int get_hydrogen_info(struct flash_bank *bank, struct command_invocation *cmd)
{
	struct hydrogen_bank *hydrogen_bank = bank->driver_priv;
	const char *device;

	if(HYDROGEN_DRIVER_DEBUG)
	  {
	    LOG_INFO("Enter in get_hydrogen_info \n");
	    LOG_INFO("hydrogen_bank->device_type=0x%x HYDROGEN_DEVICE_TYPE1=0x%x", hydrogen_bank->device_type, HYDROGEN_DEVICE_TYPE1);
	  }
	
	switch (hydrogen_bank->device_type) {
		case HYDROGEN_DEVICE_TYPE1:
			device = "HYDROGEN_V1 0x01 0x17";
			break;
		case HYDROGEN_DEVICE_TYPE2:
			device = "HYDROGEN_V1 0x9d 0x16";
			break;
		default:
			device = "Unrecognized";
			break;
	}

	command_print_sameline(cmd,"%s device \n",device);
	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from get_hydrogen_info \n");

	return ERROR_OK;
}

//COMMAND_HANDLER(hydrogen_handle_flash_autoerase_command)
//{
//	if (CMD_ARGC < 1)
//		return ERROR_COMMAND_SYNTAX_ERROR;
//
//	struct flash_bank *bank;
//	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
//	if (retval != ERROR_OK)
//		return retval;
//
//	struct hydrogen_bank *hydrogen_info = bank->driver_priv;
//	bool enable = 0;
//
//	if (CMD_ARGC >= 2)
//		COMMAND_PARSE_ON_OFF(CMD_ARGV[1], enable);
//
//	if (enable) {
//	  		hydrogen_info->algo_size = 5;
//		LOG_INFO("Flash auto-erase enabled, non mass erase commands will be ignored.");
//	} else {
//	  		hydrogen_info->algo_size = 6;
//		LOG_INFO("Flash auto-erase disabled. Use mass_erase before flash programming.");
//	}
//
//	return retval;
//}

//static const struct command_registration hydrogen_exec_command_handlers[] = {
//	{
//		.name = "mass_erase",
//		.handler = hydrogen_mass_erase,
//		.mode = COMMAND_EXEC,
//		.usage = "bank_id",
//		.help = "Erase entire flash device.",
//	},
//	{
//		.name = "flash_autoerase",
//		.handler = hydrogen_handle_flash_autoerase_command,
//		.mode = COMMAND_EXEC,
//		.usage = "bank_id on|off",
//		.help = "Set autoerase mode for flash bank.",
//	},
//	COMMAND_REGISTRATION_DONE
//};
//
//static const struct command_registration hydrogen_command_handlers[] = {
//	{
//		.name = "hydrogen",
//		.mode = COMMAND_ANY,
//		.help = "hydrogen 4 flash command group",
//		.usage = "",
//		.chain = hydrogen_exec_command_handlers,
//	},
//	COMMAND_REGISTRATION_DONE
//};

static int hydrogen_flash_blank_check(struct flash_bank *bank)
{
  //verify that all flash data are =1
	struct target *target = bank->target;

	int retval;

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen_flash_blank_check \n");

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = hydrogen_init(bank);
	if (retval != ERROR_OK)
		return retval;

	target_write_u8(target,HYDROGEN_RAM_ADDRESS_COMMAND,HYDROGEN_FLASH_COMMAND_VERIFY_ALL_BLANK);
	retval = hydrogen_wait_algo_done(bank,DEFAULT_TIMEOUT_ms);

		/* Regardless of errors, try to close down algo */
		(void)hydrogen_quit(bank);

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from hydrogen_flash_blank_check \n");

	return retval;
}

const struct flash_driver hydrogen_flash = {
	.name = "hydrogen",
	.flash_bank_command = hydrogen_flash_bank_command,
	.erase = hydrogen_erase,
	.write = hydrogen_write,
	.read = hydrogen_read,
	.probe = hydrogen_probe,
	.auto_probe = hydrogen_auto_probe,
	.erase_check = hydrogen_flash_blank_check,
	//	.erase_check = default_flash_blank_check,
	.info = get_hydrogen_info,
	.free_driver_priv = default_flash_free_driver_priv,
};
