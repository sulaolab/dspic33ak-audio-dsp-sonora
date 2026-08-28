// Sonora board serial-flash device support.
#ifndef _SST26_DRV_H
#define	_SST26_DRV_H

#include "resolved_board_config.h"

#if RESOLVED_BOARD_USE_SST26

//===========================================================
// INCLUDES
//===========================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Variables
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

extern void     sst26_init(void);

extern bool     sst26_read_jedec_id(void);
extern uint8_t  sst26_read_status(void);   // Status Register (0x05)
extern uint8_t  sst26_read_config(void);   // Configuration Register (0x35)
extern void     sst26_read_fast(uint32_t addr, uint8_t *buf, size_t len);
extern void     sst26_write_enable(void);
extern void     sst26_sector_erase_4k(uint32_t addr);
extern void     sst26_chip_erase(void);
extern bool     sst26_verify(uint32_t addr, const uint8_t *data, size_t len);
extern void     sst26_page_program(uint32_t addr, const uint8_t *data, size_t nbytes);
extern uint32_t sst26_write_next(uint32_t addr, const uint8_t *data, uint32_t size);
extern void     sst26_unprotect_all(void);
extern void     sst26_protect_all(void);

#endif // RESOLVED_BOARD_USE_SST26

#endif //!_SST26_DRV_H
