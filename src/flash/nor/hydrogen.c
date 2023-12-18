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



#define FLASH_TIMEOUT 2000
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
#define HYDROGEN_RAM_ADDRESS_BUFFER 0x1C018000
#define HYDROGEN_RAM_ADDRESS_BUSY HYDROGEN_RAM_ADDRESS_BUFFER+HYDROGEN_FLASH_PAGE_SIZE
#define HYDROGEN_RAM_ADDRESS_COMMAND2EXEC HYDROGEN_RAM_ADDRESS_BUSY+1
#define HYDROGEN_RAM_ADDRESS_COMMAND_DATA HYDROGEN_RAM_ADDRESS_COMMAND2EXEC+1
#define HYDROGEN_RAM_ADDRESS_OUTPUT_INFO HYDROGEN_RAM_ADDRESS_COMMAND_DATA+2
#define HYDROGEN_RAM_COMMAND_OUTPUT_LENGTH 8
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

static int hydrogen_wait_algo_done(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct hydrogen_bank *hydrogen_bank = bank->driver_priv;

	uint8_t status = 1;
	long long start_ms;
	long long elapsed_ms;

	int retval = ERROR_OK;

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen_wait_algo_done \n");

	start_ms = timeval_ms();
	while (status == 1) {
		retval = target_read_u8(target, HYDROGEN_RAM_ADDRESS_BUSY, &status);
		if (retval != ERROR_OK)
			return retval;

		elapsed_ms = timeval_ms() - start_ms;
		if (elapsed_ms > 500)
			keep_alive();
		if (elapsed_ms > FLASH_TIMEOUT)
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
	retval = target_wait_algorithm(target, 0, NULL, 0, NULL, 0, FLASH_TIMEOUT,
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

	target_write_u8(target,HYDROGEN_RAM_ADDRESS_COMMAND2EXEC,HYDROGEN_FLASH_COMMAND_ERASE_ALL);
	retval = hydrogen_wait_algo_done(bank);

		/* Regardless of errors, try to close down algo */
		(void)hydrogen_quit(bank);

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Exit from hydrogen_mass_erase \n");

	return retval;
}

FLASH_BANK_COMMAND_HANDLER(hydrogen_flash_bank_command)
{
	struct hydrogen_bank *hydrogen_bank;

	if(HYDROGEN_DRIVER_DEBUG==2) LOG_INFO("Enter in hydrogen FLASH_BANK_COMMAND_HANDLER  \n");

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	hydrogen_bank = malloc(sizeof(struct hydrogen_bank));
	if (!hydrogen_bank)
		return ERROR_FAIL;

	/* Initialize private flash information */
	memset((void *)hydrogen_bank, 0x00, sizeof(struct hydrogen_bank));
	hydrogen_bank->family_name = "hydrogen";
	hydrogen_bank->device_type = HYDROGEN_NO_TYPE;
	hydrogen_bank->sector_length = HYDROGEN_FLASH_SECTORS_LENGTH;

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
	    target_write_u16(target,HYDROGEN_RAM_ADDRESS_COMMAND_DATA,(uint16_t)i);
	    target_write_u8(target,HYDROGEN_RAM_ADDRESS_COMMAND2EXEC,HYDROGEN_FLASH_COMMAND_ERASE_SECTOR);
	    //read the busy signal until it is 0
	    retval=hydrogen_wait_algo_done(bank);	    
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
	struct hydrogen_bank *hydrogen_bank = bank->driver_priv;

	uint32_t size = 0;
	long long start_ms;
	long long elapsed_ms;

	int retval;
	uint16_t page_number=0;
	uint8_t cpu_ram_value_to_write[HYDROGEN_FLASH_PAGE_SIZE];

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
		page_number=0;
		while (count > 0) {
		  if(HYDROGEN_DRIVER_DEBUG)
		    {
		      LOG_INFO("Prog page Num=%d, count=%d\n", page_number, count);
		    }
		  //reset the RAM buffer into CPU
		  retval = target_write_buffer(target, HYDROGEN_RAM_ADDRESS_BUFFER,
					       HYDROGEN_FLASH_PAGE_SIZE, cpu_ram_value_to_write);
		  if (retval != ERROR_OK) {
		    LOG_ERROR("Unable to write all data 0 to RAM buffer target memory");
		    break;
		  }
	
		  if (count > hydrogen_bank->sector_length)
		    size = hydrogen_bank->sector_length;
		  else
		    size = count;
		  
		  /* Put next block of data to flash into RAM buffer */
		  retval = target_write_buffer(target, HYDROGEN_RAM_ADDRESS_BUFFER,
					       size, buffer);
		  if (retval != ERROR_OK) {
		    LOG_ERROR("Unable to write data to target memory");
		    break;
		  }

		  //fill command with number of page to write and send write operation
		  target_write_u16(target,HYDROGEN_RAM_ADDRESS_COMMAND_DATA,page_number);
		  target_write_u8(target,HYDROGEN_RAM_ADDRESS_COMMAND2EXEC,HYDROGEN_FLASH_COMMAND_WRITE_PAGE);
		  retval = hydrogen_wait_algo_done(bank);
		  if (retval != ERROR_OK)
		    break;
	
		  /* Wait for next ping pong buffer to be ready */
		  retval = hydrogen_wait_algo_done(bank);
		  if (retval != ERROR_OK)
		    break;
		  
		  count -= size;
		  buffer += size;
		  page_number=page_number+1;
		  
		  elapsed_ms = timeval_ms() - start_ms;
		  if (elapsed_ms > 500)
		    keep_alive();
		}
	
		/* If no error yet, wait for last buffer to finish */
		if (retval == ERROR_OK) {
			retval = hydrogen_wait_algo_done(bank);
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
	struct hydrogen_bank *hydrogen_bank = bank->driver_priv;

	uint32_t size = 0;
	long long start_ms;
	long long elapsed_ms;

	int retval;
	uint16_t page_number=0;
	uint8_t cpu_ram_value_loaded[HYDROGEN_FLASH_PAGE_SIZE];


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
		page_number=0;
		while (count > 0) {
		  if(HYDROGEN_DRIVER_DEBUG)  LOG_INFO("Read page %d",page_number);
		  //fill command with number of page to read and send read_page command operation
		  target_write_u16(target,HYDROGEN_RAM_ADDRESS_COMMAND_DATA,page_number);
		  target_write_u8(target,HYDROGEN_RAM_ADDRESS_COMMAND2EXEC,HYDROGEN_FLASH_COMMAND_READ_PAGE);
		  hydrogen_wait_algo_done(bank);
		  for(int yy=0;yy<HYDROGEN_FLASH_PAGE_SIZE;yy=yy+1) cpu_ram_value_loaded[yy]=0;
		  retval = target_read_buffer(target, HYDROGEN_RAM_ADDRESS_BUFFER,
					       HYDROGEN_FLASH_PAGE_SIZE, cpu_ram_value_loaded);		  

		  if (retval != ERROR_OK) {
		    LOG_ERROR("Unable to read data from ibex RAM buffer ");
		    break;
		  }
	
		  if (count > hydrogen_bank->sector_length)
		    size = hydrogen_bank->sector_length;
		  else
		    size = count;
		  
		  // Put data loaded into buffer
		  if(HYDROGEN_DRIVER_DEBUG)  LOG_INFO("count=%d page_num=%d",count,page_number);
		  for(uint32_t kkk=0;kkk<size;kkk=kkk+1)
		    {
		      buffer[kkk+HYDROGEN_FLASH_PAGE_SIZE*page_number]=cpu_ram_value_loaded[kkk];
		      //		      LOG_INFO("Global index number=%d   buffer val stored=0x%x  ",
		      //			       kkk+HYDROGEN_FLASH_PAGE_SIZE*page_number,
		      //			       buffer[kkk+HYDROGEN_FLASH_PAGE_SIZE*page_number]);
		    }

		  count -= size;
		  page_number=page_number+1;
		  
		  elapsed_ms = timeval_ms() - start_ms;
		  if (elapsed_ms > 500)
		    keep_alive();
		}
	
		/* If no error yet, wait for last buffer to finish */
		if (retval == ERROR_OK) {
			retval = hydrogen_wait_algo_done(bank);
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

	uint32_t sector_length;
	uint8_t  value8;
	int num_sectors;
	int max_sectors;
	uint8_t  command_output[HYDROGEN_RAM_COMMAND_OUTPUT_LENGTH];

     	int i;

	if(HYDROGEN_DRIVER_DEBUG)
	  {
	    LOG_INFO("Enter in hydrogen_probe \n");
	    LOG_INFO("Ram Address buffer               = 0x%x",HYDROGEN_RAM_ADDRESS_BUFFER);
	    LOG_INFO("Ram Address busy                 = 0x%x",HYDROGEN_RAM_ADDRESS_BUSY);
	    LOG_INFO("Ram Address command to exec      = 0x%x",HYDROGEN_RAM_ADDRESS_COMMAND2EXEC);
	    LOG_INFO("Ram Address commad data          = 0x%x",HYDROGEN_RAM_ADDRESS_COMMAND_DATA);
	    LOG_INFO("Ram Address output info          = 0x%x",HYDROGEN_RAM_ADDRESS_OUTPUT_INFO);
	    LOG_INFO("Ram Address buffer= 0x%x",HYDROGEN_RAM_ADDRESS_BUFFER);
	  }

	// if the algo is not copied to CPU yet, do it
	
	
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
	
	
//	/* Set up appropriate flash helper algorithm */
//	switch (hydrogen_bank->jtag_id & JTAG_ID_MASK) {
//		case HYDROGEN_V1_JTAG_ID:
//			/* v1 family device */
//			hydrogen_bank->algo_code = hydrogen_v1_algo;
//			hydrogen_bank->algo_size = sizeof(hydrogen_v1_algo);
//			hydrogen_bank->algo_working_size = HYDROGEN_V1_WORKING_SIZE;
//			hydrogen_bank->buffer_addr[0] = HYDROGEN_V1_ALGO_BUFFER_0;
//			hydrogen_bank->buffer_addr[1] = HYDROGEN_V1_ALGO_BUFFER_1;
//			hydrogen_bank->params_addr[0] = HYDROGEN_V1_ALGO_PARAMS_0;
//			hydrogen_bank->params_addr[1] = HYDROGEN_V1_ALGO_PARAMS_1;
//			max_sectors = HYDROGEN_V1_MAX_SECTORS;
//			break;
//	}
//
//	retval = target_read_u32(target, HYDROGEN_FLASH_SIZE_INFO, &value);
//	if (retval != ERROR_OK)
//		return retval;
//	num_sectors = value & 0xff;
	
	retval = target_write_u8(target, HYDROGEN_RAM_ADDRESS_COMMAND2EXEC, HYDROGEN_FLASH_COMMAND_READ_FLASHID);//command to read flash info
	if (retval){
		(void)hydrogen_quit(bank);
		return ERROR_FAIL;
	}
	//wait until command executed:
	hydrogen_wait_algo_done(bank);

	for(i=0 ; i< HYDROGEN_RAM_COMMAND_OUTPUT_LENGTH; i=i+1)
	  {
	    target_read_u8(target, HYDROGEN_RAM_ADDRESS_OUTPUT_INFO+i, &value8);//output info
	    if(HYDROGEN_DRIVER_DEBUG) LOG_INFO("Value read from memory address 0x%x=0x%x", HYDROGEN_RAM_ADDRESS_OUTPUT_INFO+i, value8);
	    command_output[i]=value8;
	  } 

	/*if(command_output[0]==0x01 && command_output[1]==0x17 && command_output[2]==0x01 && command_output[3]==0x17 )
	  {
	    //condition 1
	    if(HYDROGEN_DRIVER_DEBUG) LOG_INFO("hydrogen_probe: assigned page size and num sectors");
	    sector_length=HYDROGEN_FLASH_PAGE_SIZE;
	    num_sectors=HYDROGEN_FLASH_NUM_SECTORS;
	    max_sectors=num_sectors;
	  }
	else
	  {
	    sector_length=0;
	    num_sectors=0;
	    max_sectors=0;
	  }*/
	 sector_length=HYDROGEN_FLASH_PAGE_SIZE;
    	num_sectors=HYDROGEN_FLASH_NUM_SECTORS;
   	max_sectors=num_sectors;

	hydrogen_bank->device_type=0;
	hydrogen_bank->device_type = (command_output[0]<<24) + (command_output[1]<<16) + (command_output[2]<<8) + command_output[3];

	if(HYDROGEN_DRIVER_DEBUG) LOG_INFO("Assigned device_type num= 0x%x 0x%x 0x%x 0x%x 0x%x\n", hydrogen_bank->device_type, command_output[0],command_output[1], command_output[2],command_output[3]);
	


	if (num_sectors > max_sectors)
		num_sectors = max_sectors;


	bank->sectors = malloc(sizeof(struct flash_sector) * num_sectors);
	if (!bank->sectors){
		(void)hydrogen_quit(bank);
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
	}

	
	/* If we fall through to here, then all went well */
	if(HYDROGEN_DRIVER_DEBUG) LOG_INFO("Exit from hydrogen_probe with sector_length=%d, num_sectors=%d, bank->size=%d\n",sector_length,num_sectors,bank->size);
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

	target_write_u8(target,HYDROGEN_RAM_ADDRESS_COMMAND2EXEC,HYDROGEN_FLASH_COMMAND_VERIFY_ALL_BLANK);
	retval = hydrogen_wait_algo_done(bank);

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
