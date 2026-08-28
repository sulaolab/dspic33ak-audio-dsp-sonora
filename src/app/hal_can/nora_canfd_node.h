/**
 * @file    nora_canfd_node.h
 * @brief   dsPIC33AK CAN FD HAL - node (transmit/receive) role API.
 *
 * Phase 1: blocking transmit via the TX queue and blocking/polled receive via
 * RX FIFO 1 with an accept-all filter. Interrupt-driven operation is added in a
 * later phase. The API is intentionally object/FIFO oriented so a CMSIS-Driver
 * CAN wrapper can map onto it without ARM_CAN_* types leaking into the HAL.
 */
#ifndef NORA_CANFD_NODE_H
#define NORA_CANFD_NODE_H

#include "nora_canfd.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Node configuration. The caller owns the message-RAM region. */
typedef struct {
    uint32_t can_clk_hz;    /**< FCAN feeding the module (e.g. 20000000) */
    uint32_t nominal_bps;   /**< arbitration phase bit rate (e.g. 500000) */
    uint32_t data_bps;      /**< data phase bit rate (e.g. 2000000) */
    uint8_t  sample_pct;    /**< sample point %, e.g. 80 (0 => default 80) */
    bool     brs;           /**< enable bit-rate switching for FD frames */
    nora_canfd_mode_t mode;       /**< mode entered after init */
    uint32_t timeout_ms;    /**< blocking-op timeout (0 with get_ms => none) */
    nora_canfd_get_ms_fn get_ms;  /**< ms tick (NULL => spin, no timeout) */
    void    *msg_ram;       /**< CAN message RAM, >= nora_canfd_msg_ram_size() bytes, 4-byte aligned */
    uint16_t msg_ram_size;  /**< size of msg_ram in bytes */
} nora_canfd_config_t;

/** One CAN / CAN FD frame. */
typedef struct {
    uint32_t id;        /**< 11-bit standard or 29-bit extended identifier */
    bool     extended;  /**< true => 29-bit extended id (IDE) */
    bool     fd;        /**< true => CAN FD frame (FDF) */
    bool     brs;       /**< true => switch to data bit rate (FD only) */
    uint8_t  len;       /**< payload length (classic: 0..8, CAN FD: 0..64) */
    uint8_t  data[64];
} nora_canfd_frame_t;

/**
 * Minimum message-RAM size (bytes) required by this HAL's fixed geometry
 * (TX queue + RX FIFO 1, 4 objects each, 64-byte payload). Exposed as a
 * compile-time constant so callers can size the static region they pass in:
 *
 *   static uint32_t can_ram[NORA_CANFD_MSG_RAM_WORDS] __attribute__((aligned(4)));
 *
 * nora_canfd_msg_ram_size() returns the same value at runtime.
 */
#define NORA_CANFD_MSG_RAM_BYTES   576u
#define NORA_CANFD_MSG_RAM_WORDS   ((NORA_CANFD_MSG_RAM_BYTES + 3u) / 4u)

uint16_t nora_canfd_msg_ram_size(void);

/**
 * Initialize and bring the instance up in config->mode.
 *
 * The caller must first perform the board-specific bring-up:
 *   1) enable the module with nora_canfd_module_enable(),
 *   2) start the CAN module clock so config->can_clk_hz is real (FCAN),
 *   3) assign the PPS. The CAN RX PPS input MUST be mapped even for INTERNAL
 *      loopback - otherwise the module cannot integrate and init returns
 *      ERR_TIMEOUT (it never leaves Configuration mode).
 */
nora_canfd_status_t nora_canfd_init(nora_canfd_instance_t inst,
                                              const nora_canfd_config_t *config);

/**
 * Re-apply nominal + data-phase bit timing after init, without a full re-init.
 * Bit-timing registers are writable only in Configuration mode, so this briefly
 * drops the instance to Config, writes NBTCFG/DBTCFG/TDC computed from the given
 * clock and bit rates, then restores the previous operating mode. @p can_clk_hz
 * is the live FCAN (the caller owns the clock, as for init); @p sample_pct 0 =>
 * default 80%. Mirrors nora_i2c_set_bus_speed / nora_uart_set_baudrate
 * so a CMSIS-Driver CAN SetBitrate maps straight onto it.
 */
nora_canfd_status_t nora_canfd_set_bitrate(nora_canfd_instance_t inst,
                                                     uint32_t can_clk_hz,
                                                     uint32_t nominal_bps,
                                                     uint32_t data_bps,
                                                     uint8_t  sample_pct);

/**
 * Blocking transmit of one frame via the TX queue.
 *
 * CAN FD encodes only a fixed set of payload sizes (0..8, 12, 16, 20, 24, 32,
 * 48, 64). frame->len may be any value 0..64: it is rounded UP to the next valid
 * size and the added bytes are transmitted as zero. A receiver therefore sees
 * the rounded length (e.g. len=9 is sent as a 12-byte frame). Pass an exact
 * CAN FD size if you need the received length to match byte-for-byte.
 */
nora_canfd_status_t nora_canfd_transmit(nora_canfd_instance_t inst,
                                                  const nora_canfd_frame_t *frame);

/** True if at least one received frame is waiting in RX FIFO 1. */
bool nora_canfd_rx_available(nora_canfd_instance_t inst);

/** Blocking receive of one frame from RX FIFO 1 (ERR_NO_MESSAGE on timeout). */
nora_canfd_status_t nora_canfd_receive(nora_canfd_instance_t inst,
                                                 nora_canfd_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CANFD_NODE_H */
