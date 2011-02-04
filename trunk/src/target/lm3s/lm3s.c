/***************************************************************************
 *   Copyright (C) 2009 - 2010 by Simon Qian <SimonQian@SimonQian.com>     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "app_cfg.h"
#include "app_type.h"
#include "app_err.h"
#include "app_log.h"
#include "prog_interface.h"

#include "vsprog.h"
#include "programmer.h"
#include "target.h"
#include "scripts.h"

#include "lm3s.h"
#include "lm3s_internal.h"
#include "cm3.h"
#include "adi_v5p1.h"

#define CUR_TARGET_STRING			LM3S_STRING

struct program_area_map_t lm3s_program_area_map[] = 
{
	{APPLICATION_CHAR, 1, 0, 0, 0, AREA_ATTR_EWR},
	{0, 0, 0, 0, 0, 0}
};

const struct program_mode_t lm3s_program_mode[] = 
{
	{'j', SET_FREQUENCY, JTAG_HL},
	{'s', "", SWD},
	{0, NULL, 0}
};

struct program_functions_t lm3s_program_functions;

VSS_HANDLER(lm3s_help)
{
	VSS_CHECK_ARGC(1);
	printf("\
Usage of %s:\n\
  -C,  --comport <COMM_ATTRIBUTE>           set com port\n\
  -m,  --mode <MODE>                        set mode<j|s|i>\n\
  -x,  --execute <ADDRESS>                  execute program\n\
  -F,  --frequency <FREQUENCY>              set JTAG frequency, in KHz\n\n",
			CUR_TARGET_STRING);
	return ERROR_OK;
}

VSS_HANDLER(lm3s_mode)
{
	uint8_t mode;
	
	VSS_CHECK_ARGC(2);
	mode = (uint8_t)strtoul(argv[1], NULL,0);
		switch (mode)
		{
		case LM3S_JTAG:
		case LM3S_SWD:
			lm3s_program_area_map[0].attr |= AREA_ATTR_WNP;
			cm3_mode_offset = 0;
			vss_call_notifier(cm3_notifier, "chip", "cm3_lm3s");
			memcpy(&lm3s_program_functions, &cm3_program_functions, 
					sizeof(lm3s_program_functions));
			break;
		}
	return ERROR_OK;
}

const struct vss_cmd_t lm3s_notifier[] = 
{
	VSS_CMD(	"help",
				"print help information of current target for internal call",
				lm3s_help),
	VSS_CMD(	"mode",
				"set programming mode of target for internal call",
				lm3s_mode),
	VSS_CMD_END
};

RESULT lm3s_check_device(struct lm3s_device_info_t *device)
{
	uint32_t tmp;
	uint16_t sram_size, flash_size;
	char str_tmp[128];
	
	LOG_INFO(INFOMSG_REG_08X, "DID0", device->did0);
	LOG_INFO(INFOMSG_REG_08X, "DID1", device->did1);
	LOG_INFO(INFOMSG_REG_08X, "DC0", device->dc0);
	LOG_INFO(INFOMSG_REG_08X, "DC1", device->dc1);
	LOG_INFO(INFOMSG_REG_08X, "DC2", device->dc2);
	LOG_INFO(INFOMSG_REG_08X, "DC3", device->dc3);
	
	// check VER in did0
	tmp = (device->did0 >> 28) & 0x7;
	if ((tmp != 0) && (tmp != 1))
	{
		LOG_WARNING("unknown did0 version");
	}
	
	// check VER in did1
	tmp = (device->did1 >> 28) & 0xF;
	if ((tmp != 0) && (tmp != 1))
	{
		LOG_WARNING("unknown did1 version");
	}
	// check FAM in did1
	tmp = (device->did1 >> 24) & 0xF;
	if (tmp != 0)
	{
		LOG_ERROR(ERRMSG_INVALID_VALUE_MESSAGE, tmp, 
					"lm3s family", "should be 0");
		return ERROR_FAIL;
	}
	
	// TEMP
	tmp = (device->did1 >> 5) & 0x07;
	switch (tmp)
	{
	case 0:
		strcpy(str_tmp, "Commercial");
		break;
	case 1:
		strcpy(str_tmp, "Industrial");
		break;
	case 2:
		strcpy(str_tmp, "Extended");
		break;
	default:
		strcpy(str_tmp, "Unknown");
		break;
	}
	// check revision
	LOG_INFO("lm3s device: Revision%c.0x%02x(%s)", 
				((device->did0 >> 8) & 0xFF) + 'A',	// Revision(MAJOR), from A
				device->did0 & 0xFF, str_tmp);
	
	// sram_size
	tmp = (device->dc0 >> 16) & 0xFFFF;
	if (tmp < 7)
	{
		LOG_WARNING(ERRMSG_INVALID_VALUE, tmp, "LM3S sram_size code");
	}
	switch (tmp)
	{
	case 0x07:
		sram_size = 2;
		break;
	case 0x0F:
		sram_size = 4;
		break;
	case 0x17:
		sram_size = 6;
		break;
	case 0x1F:
		sram_size = 8;
		break;
	case 0x2F:
		sram_size = 12;
		break;
	case 0x3F:
		sram_size = 16;
		break;
	case 0x4F:
		sram_size = 20;
		break;
	case 0x5F:
		sram_size = 24;
		break;
	case 0x7F:
		sram_size = 32;
		break;
	case 0xBF:
		sram_size = 48;
		break;
	case 0xFF:
		sram_size = 64;
		break;
	case 0x017F:
		sram_size = 96;
		break;
	default:
		sram_size = 0;
		LOG_WARNING("unknown sram_size");
		break;
	}
	// flash_size
	tmp = (device->dc0 >> 0) & 0xFFFF;
	if (tmp < 3)
	{
		LOG_WARNING(ERRMSG_INVALID_VALUE, tmp, "LM3S flash_size code");
	}
	switch (tmp)
	{
	case 0x03:
		flash_size = 8;
		break;
	case 0x07:
		flash_size = 16;
		break;
	case 0x0F:
		flash_size = 32;
		break;
	case 0x1F:
		flash_size = 64;
		break;
	case 0x2F:
		flash_size = 96;
		break;
	case 0x3F:
		flash_size = 128;
		break;
	case 0x7F:
		flash_size = 256;
		break;
	default:
		flash_size = 0;
		LOG_WARNING("unknown flash_size");
		break;
	}
	
	LOG_INFO("ram_size = %dK, flash_size = %dK", sram_size, flash_size);
	
	return ERROR_OK;
}

