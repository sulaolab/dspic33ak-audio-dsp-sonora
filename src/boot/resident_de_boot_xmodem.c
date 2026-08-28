#include "resident_de_boot_xmodem.h"

#include <stddef.h>

/* The handshake window and try count are quoted by the application when it accepts
 * *fu5A, so they live where both images can see them rather than here. */
#include "resident_de_arm_timing.h"

#define XM_SOH              UINT8_C(0x01)
#define XM_STX              UINT8_C(0x02)
#define XM_EOT              UINT8_C(0x04)
#define XM_ACK              UINT8_C(0x06)
#define XM_NAK              UINT8_C(0x15)
#define XM_CAN              UINT8_C(0x18)
#define XM_CRCCHR           UINT8_C(0x43)
#define XM_BLOCK_128        128u
#define XM_BLOCK_1024       1024u
#define XM_CHAR_TIMEOUT_MS  UINT32_C(1000)
#define XM_HANDSHAKE_MS     ((uint32_t)RESIDENT_DE_ARM_HANDSHAKE_MS)
#define XM_HANDSHAKE_TRIES  ((uint8_t)RESIDENT_DE_ARM_HANDSHAKE_TRIES)
#define XM_MAX_ERRORS       10u

static uint8_t s_block[XM_BLOCK_1024];

static uint16_t crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0u;
    uint16_t index;

    for (index = 0u; index < length; index++) {
        uint8_t bit;
        crc ^= (uint16_t)((uint16_t)data[index] << 8);
        for (bit = 0u; bit < 8u; bit++) {
            crc = ((crc & 0x8000u) != 0u)
                ? (uint16_t)((crc << 1) ^ 0x1021u)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void cancel(const resident_xmodem_io_t *io)
{
    io->write(XM_CAN);
    io->write(XM_CAN);
    io->write(XM_CAN);
}

static void purge(const resident_xmodem_io_t *io)
{
    uint8_t byte;
    while (io->read(&byte, XM_CHAR_TIMEOUT_MS)) {
        /* discard through one complete idle interval */
    }
}

resident_xmodem_status_t resident_xmodem_receive(
    const resident_xmodem_io_t *io,
    resident_xmodem_sink_fn sink,
    void *context,
    uint32_t *bytes_received)
{
    uint8_t expected = 1u;
    uint8_t header = 0u;
    uint32_t offset = 0u;
    uint32_t errors = 0u;
    bool started = false;
    bool have_header = false;

    if ((io == NULL) || (io->read == NULL) || (io->write == NULL) ||
        (io->flush == NULL) || (sink == NULL)) {
        return RESIDENT_XMODEM_ERR_SINK;
    }

    io->flush();
    {
        uint8_t tries;
        for (tries = 0u; (tries < XM_HANDSHAKE_TRIES) && !have_header; tries++) {
            io->write(XM_CRCCHR);
            have_header = io->read(&header, XM_HANDSHAKE_MS);
        }
    }
    if (!have_header) {
        return RESIDENT_XMODEM_ERR_TIMEOUT;
    }

    for (;;) {
        uint16_t block_length;
        uint8_t number;
        uint8_t complement;
        uint8_t crc_high;
        uint8_t crc_low;
        uint16_t index;
        bool read_ok;

        if (!have_header && !io->read(&header, XM_CHAR_TIMEOUT_MS)) {
            if (++errors > XM_MAX_ERRORS) {
                cancel(io);
                return RESIDENT_XMODEM_ERR_TIMEOUT;
            }
            io->write(XM_NAK);
            continue;
        }
        have_header = false;

        if (header == XM_EOT) {
            io->write(XM_ACK);
            if (bytes_received != NULL) {
                *bytes_received = offset;
            }
            return started ? RESIDENT_XMODEM_OK : RESIDENT_XMODEM_ERR_TIMEOUT;
        }
        if (header == XM_CAN) {
            uint8_t second;
            if (io->read(&second, XM_CHAR_TIMEOUT_MS) && (second == XM_CAN)) {
                return RESIDENT_XMODEM_ERR_CANCELLED;
            }
            if (++errors > XM_MAX_ERRORS) {
                cancel(io);
                return RESIDENT_XMODEM_ERR_CANCELLED;
            }
            purge(io);
            io->write(XM_NAK);
            continue;
        }
        if (header == XM_SOH) {
            block_length = XM_BLOCK_128;
        } else if (header == XM_STX) {
            block_length = XM_BLOCK_1024;
        } else {
            if (++errors > XM_MAX_ERRORS) {
                cancel(io);
                return RESIDENT_XMODEM_ERR_SYNC;
            }
            purge(io);
            io->write(XM_NAK);
            continue;
        }

        read_ok = io->read(&number, XM_CHAR_TIMEOUT_MS) &&
                  io->read(&complement, XM_CHAR_TIMEOUT_MS);
        for (index = 0u; read_ok && (index < block_length); index++) {
            read_ok = io->read(&s_block[index], XM_CHAR_TIMEOUT_MS);
        }
        read_ok = read_ok && io->read(&crc_high, XM_CHAR_TIMEOUT_MS) &&
                  io->read(&crc_low, XM_CHAR_TIMEOUT_MS);
        if (!read_ok) {
            if (++errors > XM_MAX_ERRORS) {
                cancel(io);
                return RESIDENT_XMODEM_ERR_TIMEOUT;
            }
            purge(io);
            io->write(XM_NAK);
            continue;
        }

        if (((uint8_t)(number ^ complement) != UINT8_C(0xFF)) ||
            (((uint16_t)crc_high << 8) | crc_low) != crc16(s_block, block_length)) {
            if (++errors > XM_MAX_ERRORS) {
                cancel(io);
                return RESIDENT_XMODEM_ERR_SYNC;
            }
            purge(io);
            io->write(XM_NAK);
            continue;
        }
        if (number == (uint8_t)(expected - 1u)) {
            io->write(XM_ACK);
            errors = 0u;
            continue;
        }
        if (number != expected) {
            cancel(io);
            return RESIDENT_XMODEM_ERR_SYNC;
        }
        if (sink(offset, s_block, block_length, context) != 0) {
            cancel(io);
            return RESIDENT_XMODEM_ERR_SINK;
        }

        offset += block_length;
        expected = (uint8_t)(expected + 1u);
        started = true;
        errors = 0u;
        io->write(XM_ACK);
    }
}
