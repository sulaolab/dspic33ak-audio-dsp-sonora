#ifndef RESIDENT_BOOT_XMODEM_H
#define RESIDENT_BOOT_XMODEM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RESIDENT_XMODEM_OK = 0,
    RESIDENT_XMODEM_ERR_TIMEOUT,
    RESIDENT_XMODEM_ERR_CANCELLED,
    RESIDENT_XMODEM_ERR_SINK,
    RESIDENT_XMODEM_ERR_SYNC
} resident_xmodem_status_t;

typedef bool (*resident_xmodem_read_fn)(uint8_t *data, uint32_t timeout_ms);
typedef void (*resident_xmodem_write_fn)(uint8_t data);
typedef void (*resident_xmodem_flush_fn)(void);
typedef int (*resident_xmodem_sink_fn)(uint32_t offset,
                                       const uint8_t *data,
                                       uint16_t length,
                                       void *context);

typedef struct {
    resident_xmodem_read_fn read;
    resident_xmodem_write_fn write;
    resident_xmodem_flush_fn flush;
} resident_xmodem_io_t;

resident_xmodem_status_t resident_xmodem_receive(
    const resident_xmodem_io_t *io,
    resident_xmodem_sink_fn sink,
    void *context,
    uint32_t *bytes_received);

#endif
