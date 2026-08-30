#ifndef NORA_SPI_I2S_TDM_H
#define NORA_SPI_I2S_TDM_H

//===========================================================
// nora_spi_i2s_tdm.h = the public native SPI/I2S/TDM transport contract. Its dsPIC33AK
// implementation lives in nora_spi_i2s_tdm_dspic33ak.*. The same source is published standalone
// and vendored into the starter project and the CMSIS-SAI driver.
// It does: SPI framed-mode (I2S/TDM) setup, DMA + ping-pong buffers, block callback,
// start/stop/configure/get_status, and deadline-miss + load diagnostics. It is RATE-AGNOSTIC
// (no sample-rate state/API -- runs at the configured BRG / external clock). It does NOT do DSP,
// codec setup, or app config -- a client registers a block callback and a board/clock port.
// Pin/PPS + external-clock concerns are reached through the registered port (set_port()), with
// ONE exception: for a TDM MASTER with a 50%-duty frame sync (fs_shape = FS_50PCT) the _fs_clc
// unit drives CLC10 + a virtual pin (RPV8) directly to shape the FS pin; all other pin/PPS routing
// stays in the port.
// Compile-time stream geometry + topology come from a project-supplied
// nora_spi_i2s_tdm_conf.h. The HAL core only depends on NORA_TDM_* macros from that
// header -- it does not read app symbols. The publishable example config (conf.h_example in the
// HAL folder) is self-contained and app-independent; an integrator may instead supply a conf.h
// that derives NORA_TDM_* from its own build config (that is the integrator's choice, not the
// HAL's requirement). The core owns a dense logical leg table. Each descriptor row stores an
// explicit physical SPI instance plus its RX/TX DMA allocation. The default bank maps logical
// legs 0/1 to SPI1/SPI2; the explicit SPI34 test bank maps those same rows to SPI3/SPI4. Dense
// inst(i) accessors address logical rows, while spiN() searches the table for literal physical
// SPIn. conf.h also supplies geometry (SLOTS_PER_FS / BLOCK_FRAMES) and initial SYNC_DOMAIN seeds.
// The core's leg enum, buffers, leg table, and _DMA<rx>Interrupt vectors are explicit C keyed off
// those build facts (no generator macro). Supported-device limitation: the dsPIC33AK backend
// currently has silicon facts for AK512 and AK128 only; other parts need their facts added.
// Known sibling-HAL dependencies: nora_dma (required),
// and the load monitor's use of the NORA high-resolution timer public API (nora_high_res_timer_*,
// runtime-gated via is_initialized()) -- a clean sibling-HAL dependency, like nora_dma. The
// debug-only deps (<stdio.h>, nora_tick_timer.h for timestamps, board_dbg_pins.h scope pins)
// are included only under ENA_TDM_DBG, so the default (debug-off) core pulls none of them.
//===========================================================

//===========================================================
// INCLUDES
//===========================================================
#include <stdint.h>
#include <stdbool.h>
#include "nora_spi_i2s_tdm_conf.h"   // HAL compile-time config (geometry/topology); exposes NORA_TDM_* to consumers



//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================

// Block-ISR load/time monitor: execution-time stats of one instance's RX-block ISR,
// in raw timer counts and 0.1us units. (Named after the load it measures, not a fixed
// DMA channel -- each instance's RX DMA channel is configurable in conf.h.)
typedef struct
{
    uint32_t last_count;
    uint32_t min_count;
    uint32_t max_count;
    uint32_t event_count;

    // us10 means 0.1 us unit.
    // Example: 1234 means 123.4 us.
    uint32_t last_us10;
    uint32_t min_us10;
    uint32_t max_us10;
} nora_spi_i2s_tdm_load_t;


// The engine-wide TDMsum combined-occupancy snapshot that stood here was removed on
// 2026-08-26. Engine CPU load is now reported by hal_timer/nora_cpu_load_prof.h
// (nora_cpu_load_snapshot_t): a fixed measurement window independent of the block period,
// and EXCLUSIVE per-owner CPU time instead of TDM-active wall time. The old figure counted a
// preemptor's execution inside the leg it preempted, so it grew when the interrupt priorities
// changed even though the work had not. Past measurements labelled 'TDMsum' in this tree were
// taken with the old instrument and are not comparable with a 'DSPload' figure.



//===========================================================
// Runtime SPI/I2S/TDM stream configuration.
//
// inst_configure() / configure_system() validate and store this configuration while stopped.
// open() then runs the platform pin/clock port hooks; a subsequent start (inst_start /
// start_domain / start_all_domains) applies the stored configuration to the SPI peripheral and
// DMA engine. The integrator supplies it (e.g. from a board config table) or builds it directly.
// Field comments map to the SPIxCON1 bit each one drives.
//===========================================================

// Frame format. Selects FRMCNT + the conventional FRMPOL.
//   I2S : FS every 2 slots, FRMPOL active-low.
//   TDM : FS every slots_per_fs slots (TDM4/8/16/32; FRMCNT is derived from slots_per_fs, so it
//         is not fixed to 8), FRMPOL active-high.
typedef enum {
    NORA_SPI_I2S_TDM_FORMAT_I2S = 0,
    NORA_SPI_I2S_TDM_FORMAT_TDM = 1,
} nora_spi_i2s_tdm_format_t;

// Bit-clock / frame-sync role.
//   SLAVE  : MSTEN=0, FRMSYNC=1 (FS input)   -- external BCLK/FS drives the engine.
//   MASTER : MSTEN=1, FRMSYNC=0 (FS output)  -- the engine generates BCLK/FS.
typedef enum {
    NORA_SPI_I2S_TDM_CLOCK_SLAVE  = 0,
    NORA_SPI_I2S_TDM_CLOCK_MASTER = 1,
} nora_spi_i2s_tdm_clock_role_t;

// External frame-sync (FS/LRCK) waveform shape. This is the user-facing INTENT; the HAL
// picks the hardware mechanism, so the application never deals with FRMSYPW/FRMCNT/CLC:
//   FS_PULSE   : short frame sync, one BCLK wide at the frame start (DSP/TDM "short sync").
//                FRMSYPW=0, FRMCNT=slots_per_fs. No CLC.
//   FS_50PCT   : 50%-duty FS, I2S LRCLK style.
//                - I2S (2 slots): native -- FRMSYPW=1 (a one-word pulse IS 50% of a 2-word
//                  frame). No CLC.
//                - TDM (>=4 slots), MASTER: the SPI emits a 1-BCLK half-frame marker
//                  (FRMSYPW=0, FRMCNT=slots_per_fs/2) that CLC10 toggles into a 50%-duty FS
//                  on the same FS pin. The HAL owns CLC10 + virtual pin RPV8 (see
//                  nora_spi_i2s_tdm_dspic33ak_fs_clc.*).
//                - TDM SLAVE: FS is an INPUT, so fs_shape is accepted but has no
//                  generated-waveform effect (treated as normal slave framing). The CLC10
//                  50%-duty FS is generated only in master mode.
// NOTE: there is intentionally no "one-word-wide TDM" shape -- a word-wide TDM pulse is a
// niche non-50% long-frame sync and was dropped in favor of these two common intents.
typedef enum {
    NORA_SPI_I2S_TDM_FS_PULSE = 0,   // short frame sync, ~1 BCLK (FRMSYPW=0)
    NORA_SPI_I2S_TDM_FS_50PCT = 1,   // 50%-duty FS (I2S: native; TDM master: via CLC10)
} nora_spi_i2s_tdm_fs_shape_t;

typedef struct {
    nora_spi_i2s_tdm_format_t format;          // I2S vs TDM (FRMCNT/FRMPOL)
    nora_spi_i2s_tdm_clock_role_t   clock_role;      // master vs slave (MSTEN/FRMSYNC)
    uint8_t  slots_per_fs;                          // NORA_TDM_SLOTS_PER_FS: I2S=2 / TDM=4,8,16,32
    uint8_t  word_bits;                             // 32 (MODE32); only 32 validated
    nora_spi_i2s_tdm_fs_shape_t fs_shape;      // FS waveform intent (see enum). HAL derives
                                                    // FRMSYPW/FRMCNT and engages CLC10 as needed.
    uint16_t block_frames;                          // NORA_TDM_BLOCK_FRAMES: frames per ping/pong half
    uint32_t brg;                                   // SPIxBRG (master only; ignored as slave)
    bool     mclk_enable;                           // MCLKEN (CLKGEN9 reference)
    bool     fs_coincides_first_bclk;               // SPIFE: 1=no delay, 0=1-bit delayed (ENA_1_BIT_DELAY)
    bool     bclk_idle_high;                        // CKP
    bool     bclk_change_on_active_to_idle;         // CKE
    // NOTE: IGNROV and IGNTUR are NOT exposed here on purpose. Continuous DMA audio keeps both set
    // so a secondary FIFO error cannot critical-stop the SPI leg and hide the primary failure.
    // This is a continuity/containment policy, NOT a claim that data loss is benign. RX
    // DMAxSTAT.OVERRUN is captured separately as the RX-DMA request-overrun signal. SPIROV may
    // follow stalled RX service; SPITUR independently reports TX starvation; FRMERR tracks framing.
} nora_spi_i2s_tdm_config_t;

// Clock-change event reported by the registered port's external-clock detector (if the board
// provides one). STOPPED = the external bit/frame clock has stopped (e.g. the source is switching
// sample rate) -> the app should mute + stop. RESUMED = the clock is back -> the app should measure
// the rate, reconfigure, then restart. NONE = nothing pending. Consumed (read-and-clear) via
// nora_spi_i2s_tdm_consume_clock_event(); always NONE when the port provides no external-clock
// detector.
typedef enum {
    NORA_SPI_I2S_TDM_CLOCK_EVENT_NONE = 0,
    NORA_SPI_I2S_TDM_CLOCK_EVENT_STOPPED, // external clock stopped (rate switch begun)
    NORA_SPI_I2S_TDM_CLOCK_EVENT_RESUMED, // external clock back
} nora_spi_i2s_tdm_clock_event_t;

//===========================================================
// TDM SLOT -- the element type of the block callback's buffers, and of the TX fill pointer.
//
// One name, one representation per family, and the representation belongs to the backend:
//   AK: int32_t, transparent. MODE32 gives a single 32-bit SPIxBUF and 32-bit DMA
//       elements, so one sample IS one DMA element and there is nothing to pack.
//   CK: struct { uint16_t wire[2]; }. The DMA element there is a 16-bit wire word; the
//       struct is what keeps that visible. Hiding it behind an int32_t buffer is how a
//       half-swap defect stayed hidden once already (observed on the dsPIC33CK backend).
//
// This is the DMA contract's typed-value rule (nora_dma_status_t) applied one level up: the
// TYPE is shared, the LAYOUT is not, and a consumer touches the value only through the
// accessors below. It is not a portability facade -- no extra header, no conversion layer,
// no copy, no runtime cost. What it buys is that the block-callback signature is textually
// identical in both families while a portable consumer that writes `dst[i] = sample`
// still FAILS TO COMPILE on CK, at build time, loudly.
//
// The struct is NOT ported to this family and must not be: `int32_t` is the honest
// description of an AK DMA element. Sharing the NAME is not sharing the layout.
//
// THREE RULES FOR CODE THAT IS MEANT TO MOVE BETWEEN FAMILIES. All three are contract,
// not style, and all three exist because THIS family cannot detect a violation:
//
//  1. Go through the accessors. This family's typedef is transparent, so `dst[i] = sample`
//     compiles here and only fails on a family whose slot is a struct. The build that
//     catches the mistake is the OTHER one -- writing portable code here and discovering
//     it in someone else's port is the failure mode this rule exists to prevent. (An
//     AK-only consumer is not forced to convert; `audio_transport.c` does not.)
//
//  2. A slot is not a byte sequence. Both families are exactly 4 bytes and both assert it,
//     so memcpy'ing slot buffers between families, persisting them, or reinterpreting them
//     as int32_t COMPILES EVERYWHERE AND IS WRONG (this family stores a plain int32_t; a
//     wire-word family stores most-significant half first). Cross-family transfer,
//     storage and reinterpretation of the raw bytes are outside this contract. The DMA
//     status word needed no such prohibition because nothing tempts a consumer to move it;
//     an audio buffer is tempting, so the prohibition is written down.
//
//  3. Fold the conversion into the store/load the DSP already performs; do not add a
//     conversion pass. `for (i) encode(&dst[i], out[i])` is the shape the accessor
//     vocabulary invites, and it is the shape that costs: on a wire-word family a separate
//     pass measured 3-4x a conversion folded into the DSP's own final store. Here
//     encode/decode are the identity and cost nothing, which is exactly why an author on
//     this family cannot see the trap -- hence it is stated in the shared contract rather
//     than in one backend's comments.
//===========================================================
typedef int32_t nora_tdm_slot_t;

_Static_assert( sizeof(nora_tdm_slot_t) == 4u,
                "a TDM slot must be exactly 32 bits with no padding" );

// int32_t sample -> slot. Use at the DSP's final store.
static inline void nora_tdm_slot_encode_s32( nora_tdm_slot_t* dst, int32_t sample )
{
    *dst = sample;
}

// slot -> int32_t sample. Use where the DSP loads its input.
static inline int32_t nora_tdm_slot_decode_s32( const nora_tdm_slot_t* src )
{
    return *src;
}

// Unsigned Q15 gain applied to one slot, saturation-free by precondition.
//
// CONTRACT (identical in every family):
//   gain_q15 <= 0x8000, i.e. unity is 0x8000 and gains above unity are NOT this function's
//   job -- |y| <= |x| follows, so no overflow is possible and no saturation is performed.
//   gain_q15 == 0x8000 -> y == x in every audio bit.  gain_q15 == 0 -> y == 0, branchless.
//   Truncating, not rounding.  src == dst is safe.
//
// The two families differ in the LEAST SIGNIFICANT BIT of the 32-bit word and nowhere else,
// and the difference is ONE-SIDED, which is the part worth pinning: a wire-word family
// computing ((x*g) >> 16) << 1 from two 16x16 products produces exactly
//     y_wire == y_here - bit15( lo16(x) * g )
// so y_wire <= y_here always, bit 0 is always clear, and the difference is a truncation
// toward negative infinity -- not a rounding-mode disagreement that could go either way.
// Unity (0x8000) and zero are bit-exact in both; only intermediate gains differ. That bit
// is below the audio LSB of a 24-bit-in-32 wire word on both parts. Stated rather than
// hidden: a consumer comparing two families bit-for-bit will find this, and one-sidedness
// is what lets it conclude "declared difference" instead of "possible bug".
//
// Off the ISR path by construction. A consumer that measures this in a hot loop takes the
// backend's *_fast.h twin (the `_hot` naming rule), it does not rewrite this one.
static inline void nora_tdm_slot_scale_q15( const nora_tdm_slot_t* src,
                                            nora_tdm_slot_t*       dst,
                                            uint16_t               gain_q15 )
{
    *dst = (int32_t)( ( (int64_t)*src * (int64_t)(uint32_t)gain_q15 ) >> 15 );
}

// One-completed-block callback (event hook), registered PER SPI instance. The
// instance's RX-block ISR calls this for each completed block: src = the RX ping/pong
// half just captured by THIS instance; dst = the TX ping/pong half of THIS instance
// to fill; user = opaque context. One callback handles exactly one physical SPI's
// RX/TX block -- there is no dst_b / "second output": when multiple instances run,
// each has its own callback. Cross-instance routing is application policy. It may use
// an application-owned handoff buffer, or the candidate co-clock mirror helpers below
// when one callback directly produces a co-clocked sibling's TX block.
// Contract: register it (set_block_callback) BEFORE start(); do NOT clear it while
// running. If no callback is registered for an instance, that instance runs no
// app/DSP path (its zeroed TX half stays silent).
// Contract: when this callback is invoked, src and dst are both non-NULL. If the core
// cannot resolve either half-buffer (reload boundary / just-stopped / first block /
// fault), it skips the block instead of calling the callback -- the callee never
// NULL-checks src/dst.
// src and dst are SLOT buffers (nora_tdm_slot_t, above), not raw int32_t arrays. On this
// family a slot IS an int32_t, so `dst[i] = sample` compiles and costs the same as it
// always did; a consumer that intends to be portable goes through
// nora_tdm_slot_decode_s32() / _encode_s32() instead, because on a family whose DMA
// element is a 16-bit wire word the raw store is a half-swap bug.
typedef void (*nora_spi_i2s_tdm_block_cb_t)( const nora_tdm_slot_t* src,
                                                  nora_tdm_slot_t*       dst,
                                                  void*                  user );

// Opaque per-leg instance handle. The engine exposes the built dense logical leg table, backed
// by up to four physical SPI instances on AK512. Legs sharing a sync_domain are co-clocked and
// started phase-locked as a group; legs in different domains are started/rolled-back separately
// and need not share BCLK/FS -- but this is NOT full independence: source-readiness is
// engine-wide/primary-gated and some board resources (CLC10, the clock port) are shared. Use the
// logical inst(i) or literal physical spiN() accessors below, then pass the handle to
// inst_configure()/inst_start()/inst_stop()/set_block_callback()/inst_get_status() to
// drive or query that one instance. The shared board/clock port is brought up once via
// open()/close(). The application selects the topology and lifecycle operation; the HAL
// owns per-domain arm/go ordering and rollback.
typedef struct nora_spi_i2s_tdm_inst_s nora_spi_i2s_tdm_inst_t;

// Stream status snapshot. block_count is the number of completed audio
// blocks delivered since the last start(); one block = block_frames (NORA_TDM_BLOCK_FRAMES)
// frames per direction. load is the block-ISR load monitor (same data as
// get_load()). block_deadline_miss_count is the number of
// times the RX-block ISR fell a full block behind (HALF+DONE conflict) since start()
// -- the real-time/stream-health metric for this zero-copy engine. It is DISTINCT
// from SPI HW FIFO over/underrun flags.
// `running` is the true stream-running state -- set by start(), cleared by stop().
// It is DISTINCT from `active`: `active`
// (is_active()) is the clock/source-readiness gate (e.g. an external bit/frame clock
// present) that a client's main loop can use to decide whether streaming *should*
// run, and it can read true while the stream is stopped. Read `running` for "is
// the engine actually streaming", `active` for "is the clock source ready".
typedef struct {
    bool                        active;       // is_active(): clock/source readiness (NOT running)
    bool                        running;      // is_running(): stream actually started (start..stop)
    uint32_t                    block_count;  // completed blocks since start()
    uint32_t                     block_deadline_miss_count; // HALF+DONE conflicts since start()
    uint32_t                    rx_dma_overrun_count;      // RX IRQ snapshots with DMAxSTAT.OVERRUN
    uint32_t                    rx_dma_other_irq_count;    // RX IRQ snapshots with neither HALF nor DONE
    uint32_t                    rx_dma_last_status;        // raw DMAxSTAT from the latest RX IRQ
    uint32_t                    err_rov_block_count;       // RX blocks where SPIROV was observed, since start()
    uint32_t                    err_tur_block_count;       // RX blocks where SPITUR was observed set, since start()
    uint32_t                    err_frm_block_count;       // RX blocks where FRMERR was observed, since start()
    uint32_t                    frmerr_consecutive_blocks; // consecutive RX blocks with FRMERR observed (0 when clean)
    nora_spi_i2s_tdm_load_t load;         // block-ISR load/time monitor
} nora_spi_i2s_tdm_status_t;


//===========================================================
// Board/clock PORT (optional hooks). The HAL core is board-free: instead of
// calling the board adapter directly, it routes pin routing + external-clock
// concerns through this fn-pointer table, which the integrator's board/platform layer
// registers via set_port(). Most of these hooks are consumed by open();
// clock_source_ready() is ALSO re-checked by the start paths (inst_start/start_domain/
// start_all_domains) immediately before arming. Every field is optional; the fallible hooks return bool
// (false => open() aborts and returns false) and take the role open() derived from the
// committed primary leg, so the platform can act differently for master vs slave:
//   - configure_pins(role)  : PPS/GPIO routing for the role. false => unsupported
//                             pin config (e.g. a role this board cannot drive) =>
//                             open() fails. NULL => core does no pin routing.
//   - clc_passthrough(role) : CLC bypass route (slave clock fan-out). false =>
//                             open() fails. NULL => skipped.
//   - clock_source_init(role): bring up an external (e.g. USB-audio) clock. false
//                             => open() fails. NULL => no external clock to bring up.
//   - clock_source_ready(role): external-clock readiness; drives is_active() and a
//                             non-blocking check in OPEN() (open() does NOT wait -- it returns
//                             false if not ready, leaving retry to the platform/app). It is ALSO
//                             re-checked once by each start path (inst_start/start_domain/
//                             start_all_domains) immediately before arming, so a source that
//                             drops between open() and start fails the start (ERR_CLOCK_NOT_READY)
//                             rather than entering a dead stream. NULL => always ready (no gate).
//   - consume_clock_event() : read-and-clear the ext-clock stop/resume edge; NULL
//                             => always NONE.
// With NO port registered the core behaves as a self-clocked transport with no
// readiness gate (is_active()==true, no events).
//===========================================================
typedef struct {
    bool (*configure_pins)( nora_spi_i2s_tdm_clock_role_t role );
    bool (*clc_passthrough)( nora_spi_i2s_tdm_clock_role_t role );
    bool (*clock_source_init)( nora_spi_i2s_tdm_clock_role_t role );
    bool (*clock_source_ready)( nora_spi_i2s_tdm_clock_role_t role );
    nora_spi_i2s_tdm_clock_event_t (*consume_clock_event)( void );
} nora_spi_i2s_tdm_port_t;


//===========================================================
// HAL API. The transport's entry points:
//   - set_port()         : register the board/clock port (above). Call before
//                          inst_configure()/open(); NULL reverts to the self-clocked,
//                          no-gate default.
//   - spi1()...spi4()    : literal per-physical-SPI instance handles.
//   - inst_configure()   : validate + store a config_t for one instance (no HW write).
//   - set_block_callback(): register one instance's per-block event callback.
//   - open()/close()     : shared board/clock port bring-up/teardown (once for the
//                          engine, role-aware) -- see the per-instance lifecycle block
//                          below.
//   - inst_start()/inst_stop(): SINGLE-mode, PRIMARY-only per-instance lifecycle (a single-leg
//                          driver): configure the primary -> open() -> inst_start(primary) ->
//                          ... -> inst_stop(primary) -> close(). inst_start() returns true only
//                          if it actually started (false, instance stopped, if the wrong
//                          config-mode/leg, not configured, already running, the clock is not
//                          ready, or DMA setup fails) and never blocks. A co-clocked group uses
//                          the domain API below.
//   - is_active()        : clock/source readiness gate (NOT running).
//   - is_running()       : primary-leg running state (start..stop).
//   - get_load()/get_status()       : primary logical leg (default index 0) load / status.
//   - inst_get_load()/inst_get_status(): a specific instance's load / status.
//   - inst_get_setup()   : read a specific instance's committed {stream, sync_domain}.
//   (DMA interrupt vectors: default (NORA_TDM_DEFINE_DMA_VECTORS=1) the HAL owns
//    the _DMAnInterrupt vectors -- the integrator writes no ISR code. Opt-out (=0): the
//    integrator owns the IVT and calls nora_spi_i2s_tdm_inst_rx_isr() from their
//    own vector. TX is interrupt-less. Each RX vector is bound to its leg's conf.h RX-DMA
//    channel by a compile-time assert; the explicit vector name does NOT auto-follow, so changing
//    the channel in conf.h fails the build until the matching _DMA<rx>Interrupt in the core is
//    updated (or use DEFINE_DMA_VECTORS=0 + your own ISR). See the vector note near the end.)
//===========================================================

// Register the board/clock port (fn-pointer hooks above). Pass NULL to clear it
// (revert to the self-clocked, no-gate default). Call before inst_configure()/open()
// (e.g. from the platform layer at init). The pointer is stored, not copied -- it
// must outlive the stream (use a static/const table). Returns false (port unchanged,
// ERR_ALREADY_OPEN) if the port is already open()'d or any leg is running -- open()
// consumes the hooks, so the port must be fixed before open().
extern bool nora_spi_i2s_tdm_set_port( const nora_spi_i2s_tdm_port_t* port );

// Instance handles. The leg count is configurable (NORA_TDM_USE_SPI2/3/4). instance_count()
// returns how many instances this build has; inst(i) returns the i-th handle in leg-table
// order (0 = the default primary logical leg) or NULL if i is out of range. Together they
// let a caller enumerate instances (for i in 0 .. instance_count()-1: inst(i)). spiN() searches
// those descriptors for literal physical SPIn and returns NULL when SPIn is not present in this
// build. Thus SPI34_TEST has inst(0)/inst(1) == codec A/B while spi1()/spi2() return NULL and
// spi3()/spi4() return those physical rows. Use the handle with
// set_block_callback(). (E.g. a CMSIS-SAI wrapper can map Driver_SAI0 -> spi1().)
extern uint8_t                       nora_spi_i2s_tdm_instance_count( void );
extern nora_spi_i2s_tdm_inst_t* nora_spi_i2s_tdm_inst( uint8_t index );
extern nora_spi_i2s_tdm_inst_t* nora_spi_i2s_tdm_spi1( void );
extern nora_spi_i2s_tdm_inst_t* nora_spi_i2s_tdm_spi2( void );
extern nora_spi_i2s_tdm_inst_t* nora_spi_i2s_tdm_spi3( void );
extern nora_spi_i2s_tdm_inst_t* nora_spi_i2s_tdm_spi4( void );

//===========================================================
// CO-CLOCKED BLOCK (dual-codec, single-producer). The four functions below serve one
// logical leg's callback filling a sibling leg's TX, plus the phase probes that measure
// their alignment. A single-leg or independent-instance consumer never calls them.
//
// "A consumer here does not call it" is NOT a licence to omit: every NORA backend whose
// silicon can co-clock two legs implements all four, under these names, with these
// semantics -- that is what lets a co-clocked application move between families. Unused,
// they cost zero bytes (per-function sections + section GC; see the DMA declaration's
// R0.1 and its measured 128-bytes-emitted-then-dropped result).
//
// What the banner above used to say -- "may change or move" -- is withdrawn as a
// stability statement. What remains true is the USAGE note: these are not part of the
// minimal single-leg transport surface, so a consumer that calls them is declaring a
// co-clocked topology, and the phase probes are diagnostics, not a control loop.
//===========================================================

// Return one instance's current writable TX ping-pong half (the half NOT being
// transmitted), or NULL if inst is NULL/stopped/unresolved. Lets an app produce one
// instance's output from ANOTHER instance's block callback so two co-clocked codecs
// stay sample-aligned (call at a block boundary; co-clocked siblings share the phase).
// NULL-check before writing.
extern nora_tdm_slot_t* nora_spi_i2s_tdm_inst_tx_fill_ptr( nora_spi_i2s_tdm_inst_t* inst );

// Result of inst_tx_fill_mirror() -- lets the caller distinguish a transient "position not yet
// resolvable" (reload boundary / just-started) from a genuine "target half is being transmitted"
// so it can tolerate the former (skip one block, resync only if persistent) but fault on the latter.
typedef enum {
    NORA_TDM_MIRROR_OK = 0,                   // *dst set to the safe (non-transmitting) target half
    NORA_TDM_MIRROR_UNSAFE_ACTIVE_HALF,       // target half == the half inst is transmitting NOW; *dst=NULL
    NORA_TDM_MIRROR_UNRESOLVED_DMA_POSITION,  // inst's live TX-DMA address is out of buffer range; *dst=NULL
    NORA_TDM_MIRROR_BAD_ARGUMENT,             // NULL arg / stopped inst / ref_fill_half outside ref buffer; *dst=NULL
} nora_spi_i2s_tdm_mirror_result_t;

// Mirror a reference instance's fill half onto THIS instance's TX buffer (target selected
// DETERMINISTICALLY from ref_fill_half -- valid for the whole block; a live-DMA read is used only
// as a secondary safety veto). For the co-clocked single-producer dual-codec path: pass ref = the
// producing/reference leg and ref_fill_half = the `dst` its block callback received; on OK,
// *dst = the same-index (not-transmitting, full-block-valid) half of the target `inst`. Returns a typed result:
// on OK *dst is the writable half; on UNSAFE_ACTIVE_HALF / UNRESOLVED_DMA_POSITION / BAD_ARGUMENT
// *dst is NULL and the caller must NOT write B this block (UNSAFE = fault now; UNRESOLVED = a
// transient the caller tolerates for a few blocks then resyncs). Keeps co-clocked siblings
// sample-aligned and race-free.
extern nora_spi_i2s_tdm_mirror_result_t nora_spi_i2s_tdm_inst_tx_fill_mirror(
        nora_spi_i2s_tdm_inst_t*       inst,
        const nora_spi_i2s_tdm_inst_t* ref,
        const nora_tdm_slot_t*              ref_fill_half,
        nora_tdm_slot_t**                   dst );

// Phase probe: which TX ping-pong half is this instance's DMA transmitting NOW?
// 0 = ping, 1 = pong, -1 = unresolved. For measuring co-clocked sibling alignment.
extern int nora_spi_i2s_tdm_inst_tx_active_half( nora_spi_i2s_tdm_inst_t* inst );

// Phase probe (finer): TX DMA current read position as a SLOT offset into [0, 2*half),
// i.e. one unit = one sample of one wire slot -- not bytes, and not a family's DMA element
// (a backend whose element is a 16-bit wire word converts). -1 if unresolved. Diff of two
// co-clocked legs = their sub-block sample offset.
extern int32_t nora_spi_i2s_tdm_inst_tx_active_pos( nora_spi_i2s_tdm_inst_t* inst );

// Register the per-completed-block callback for one SPI instance. The callback
// receives that instance's RX half just completed and the TX half it may fill.
// Register BEFORE inst_start(). Returns false (and changes nothing) on a contract
// violation: NULL inst, or the instance is running and the (cb,user) pair would
// change -- the callback must not be swapped or cleared mid-stream. Re-registering the
// identical (cb,user) while running is a no-op and returns true.
extern bool nora_spi_i2s_tdm_set_block_callback( nora_spi_i2s_tdm_inst_t* inst,
                                                      nora_spi_i2s_tdm_block_cb_t cb,
                                                      void* user );

// ---- Lifecycle + configuration-ownership mode ----
// The two configure paths establish a mutually-exclusive OWNERSHIP mode that selects which
// start/stop API is legal (the mode is a property of the committed configuration, INDEPENDENT
// of open/close -- close() does NOT reset it):
//   * inst_configure()  -> SINGLE mode. The per-leg PRIMARY-only API is in force:
//                          inst_configure / inst_start / inst_stop, and ONLY on the primary
//                          leg. A non-primary leg or a SYSTEM-committed stream is rejected
//                          (ERR_CONFIG_MODE). For a single-leg driver (e.g. CMSIS-SAI).
//   * configure_system() -> SYSTEM mode. The whole-system domain API is in force:
//                          configure_system / start_domain / start_all_domains / stop_domain /
//                          stop_all_domains. inst_start/inst_stop/inst_configure are rejected
//                          (ERR_CONFIG_MODE). configure_system() may full-recommit from ANY
//                          mode while closed+stopped.
// open() brings up the SHARED board/clock port (external clock + pins + CLC) ONCE for the
// engine. It takes NO role argument: the HAL derives the clock role from the COMMITTED
// primary leg (primary_leg_index) and passes THAT to the port hooks, so the pin/clock
// direction can never disagree with the configured stream. Returns false (do not start any
// instance) if the primary leg is not configured, the external clock can't be brought up /
// isn't ready, or a pin/CLC hook rejects the role. With no port registered it is a no-op
// success. A second open() while already open is an idempotent success (hooks are NOT re-run).
// It never blocks and touches no SPI/DMA. close() is the symmetric teardown: it returns false
// (stays open, ERR_ALREADY_RUNNING) if any leg is still running -- stop first -- else clears
// the open state. It is otherwise a near-no-op: the HAL deliberately never tears down PPS/CLC
// or the clock (other peripherals may depend on them; reserved for a future clock-deinit hook),
// and it does NOT change the config mode. The start paths re-check clock readiness just before
// arming, so a source that drops between open() and start() fails the start (ERR_CLOCK_NOT_READY).
// Typical sequence: configure_system() (or inst_configure the primary) -> open() ->
// start_all_domains() (or, in SINGLE mode, inst_start the primary) -> ... -> stop -> close().
extern bool nora_spi_i2s_tdm_open( void );
extern bool nora_spi_i2s_tdm_close( void );
extern bool nora_spi_i2s_tdm_inst_configure( nora_spi_i2s_tdm_inst_t* inst,
                                                  const nora_spi_i2s_tdm_config_t* cfg );

// Per-leg setup for the whole-system configure below: the stream config PLUS the leg's
// sync domain, so BOTH become single-sourced from the caller's topology description (the
// sync domain is no longer taken only from the compile-time conf.h macro).
typedef struct {
    nora_spi_i2s_tdm_config_t stream;        // full per-leg transport config
    uint8_t                        sync_domain;   // co-clocked legs share an id; non-co-clocked legs use different ids
} nora_spi_i2s_tdm_leg_setup_t;

// Configure ALL legs in one TRANSACTIONAL call: setups[i] targets leg index i, and
// setup_count MUST equal the built leg count. Two passes with all-or-nothing semantics:
//   1. PREFLIGHT (zero side effects): every leg must be stopped, its stream must pass the
//      envelope check, its sync_domain must be < 32, each sync domain may contain at most one
//      clock MASTER, and legs sharing a sync domain must agree on the frame interpretation
//      (format/word_bits/slots/block/SPIFE/CKP/CKE/fs_shape). If ANY check fails the whole call
//      is rejected and NOT a single leg is touched. (start_domain re-checks these invariants at
//      start, so the per-leg inst_configure() path is guarded too.)
//   2. COMMIT: only after a fully clean preflight, every leg's config + sync_domain +
//      config_valid are stored together. There is thus no partially-configured state --
//      never SPI1 on the new config while SPI2 keeps the old.
// The caller owns the stop->configure->start contract (configure_system does NOT stop a
// running transport; it rejects one). Replaces per-leg inst_configure + any app-side role
// rewrite: the caller hands resolved per-leg setups and gets all-or-nothing.
extern bool nora_spi_i2s_tdm_configure_system( const nora_spi_i2s_tdm_leg_setup_t* setups,
                                                    uint8_t setup_count );

// Read one leg's COMMITTED setup (the config stored by inst_configure/configure_system, plus
// its sync domain) into *setup. Returns false -- and touches neither *setup nor the last-error
// (pure query) -- if inst is NULL, setup is NULL, or the leg is not configured (config_valid
// == false). Lets a board port hook route that leg's pins/CLC from the committed clock role
// with no side table, and lets a caller distinguish "unconfigured" from a valid SLAVE (role
// value 0). An optional leg left unconfigured (e.g. a single-instance CMSIS run that only
// configured the primary) returns false, so the caller can SKIP it rather than assume a role.
extern bool nora_spi_i2s_tdm_inst_get_setup( const nora_spi_i2s_tdm_inst_t* inst,
                                                  nora_spi_i2s_tdm_leg_setup_t* setup );
// SINGLE-mode, PRIMARY-only per-leg start/stop (a co-clocked group or non-primary leg must use
// start_domain/start_all_domains). inst_start() returns false (ERR_CONFIG_MODE) unless the stream
// was committed via inst_configure() AND inst is the primary leg; it also re-checks clock
// readiness before arming. inst_stop() likewise returns false (ERR_CONFIG_MODE) in SYSTEM mode or
// for a non-primary leg; true after the (idempotent) teardown.
extern bool nora_spi_i2s_tdm_inst_start( nora_spi_i2s_tdm_inst_t* inst );
extern bool nora_spi_i2s_tdm_inst_stop( nora_spi_i2s_tdm_inst_t* inst );
// Per-instance RX-block INTERRUPT mask, independent of the stream itself.
//
// The DMA keeps transferring either way: this masks only the RX-block ISR, so the leg's TX still
// streams and its RX buffer still fills -- what stops is the per-block callback and every
// diagnostic the ISR maintains (block_count, deadline_miss, FRMERR bookkeeping, load peaks).
//
// Its one legitimate use is a leg whose RX nobody consumes and whose TX is filled by ANOTHER
// leg's callback (co-clocked single-producer mirroring): there the ISR does nothing but cost CPU
// in the block period, and freeing it buys DSP headroom. Because that also freezes the leg's
// diagnostics, the caller owns the consequence -- do not read this leg's ISR-maintained counters,
// and do not gate any watchdog on them, while the mask is off.
//
// Re-enabling clears the pending flag first, so a request that arrived while masked does not
// fire an immediate spurious block. Returns false only for a NULL instance.
extern bool nora_spi_i2s_tdm_inst_set_block_irq_enabled( nora_spi_i2s_tdm_inst_t* inst,
                                                              bool enabled );
extern bool nora_spi_i2s_tdm_inst_block_irq_is_enabled( const nora_spi_i2s_tdm_inst_t* inst );

// NOTE: the internal arm/go split (program+arm DMA/SPI with the module OFF, then release SPIEN
// back-to-back so co-clocked legs latch one FS edge = phase-locked) is NOT public. It has no
// armed-state / open-gate of its own, so exposing it would let a caller enable SPI out of
// sequence. Phase-locked co-clocked startup is delivered through start_domain() /
// start_all_domains() (which arm then release internally); a single leg uses inst_start().

// Sync-domain group start/stop -- the SYSTEM-mode API (stream committed via configure_system()).
// A domain = the set of legs sharing sync_domain. start_domain arms all members then releases
// SPIEN back-to-back (non-master legs first, clock-master last) so co-clocked members latch one FS
// edge = phase-locked. start_all_domains starts every domain once. open() must run first. All four
// return false with ERR_CONFIG_MODE if the stream was committed via inst_configure() (SINGLE mode)
// -- a SINGLE-mode stream starts/stops through inst_start()/inst_stop(). stop_domain/stop_all_domains
// return true after teardown (idempotent on an already-stopped SYSTEM domain).
extern bool nora_spi_i2s_tdm_start_domain( uint8_t domain );
extern bool nora_spi_i2s_tdm_stop_domain( uint8_t domain );
extern bool nora_spi_i2s_tdm_start_all_domains( void );
extern bool nora_spi_i2s_tdm_stop_all_domains( void );

// Return the clock/source readiness gate. This can be true while the transport is
// stopped; use is_running() when the question is "is audio streaming now?"
extern bool nora_spi_i2s_tdm_is_active( void );

// Return true only after start() succeeds and before stop() begins.
extern bool nora_spi_i2s_tdm_is_running( void );   // true stream-running state (start..stop)

// Consume one external-clock stop/resume edge from the board port, or NONE when
// no event/hook exists.
extern nora_spi_i2s_tdm_clock_event_t nora_spi_i2s_tdm_consume_clock_event( void );  // external-clock stop/resume edge

// NOTE: the transport is RATE-AGNOSTIC -- there is intentionally NO sample-rate API
// (no notify / get / set-callback / is-supported / rate_state). The HAL runs at the
// configured BRG (master) or the incoming external clock (slave) and never derives
// anything from a sample-rate value. Sample-rate POLICY is NOT a HAL property: the
// product/board's supported-rate set lives in the app layer (APP_SAMPLE_RATE_IS_SUPPORTED),
// used by the CMSIS-SAI wrapper to validate ARM_SAI AUDIO_FREQ. Runtime rate DETECTION +
// the stop->reconfigure->start it drives live in the application.

// RATE-MONOTONIC LEG PRIORITIES.
//
// By default every leg's RX-DMA interrupt sits at the same priority, so the legs cannot
// preempt each other and each one's worst-case response time includes the other's whole
// block ISR as blocking. With mixed rates that blocking is what misses the deadline of the
// leg with the SHORTER period, even though that leg on its own fits comfortably.
//
// This gives the leg named first the base priority and the other one base-1, so the
// short-deadline leg preempts the long-deadline one. Pass the same instance twice (or NULL
// as `longer_deadline`) to put every configured leg back on the base priority, which is the
// power-on state. The HAL owns the numbers; the caller only decides which leg is which.
//
// TASK LEVEL ONLY, and intended for a moment when the legs are not streaming (a rate change
// commits through stop -> reconfigure -> start). Making the priorities asymmetric means the
// legs CAN nest, so anything shared between two leg callbacks has to be safe against that --
// on this core W0-W7/AccA/AccB/RCOUNT/CORCON are banked per IPL and so are F0-F7, but the
// modulo-addressing registers are not, and neither is application state.
// Validated on an AK512 16-channel mixed-rate configuration.
//
// Returns false and changes nothing on a bad instance, or if the base priority leaves no
// room below it.
bool nora_spi_i2s_tdm_set_rate_monotonic_priorities( nora_spi_i2s_tdm_inst_t* shorter_deadline,
                                                     nora_spi_i2s_tdm_inst_t* longer_deadline );

// Read back the CPU interrupt priority this leg's RX-block ISR currently runs at (0..7);
// 0 for a bad instance -- which is also the "disabled" encoding, so a caller reporting this
// cannot mistake a refused call for a programmed level.
//
// This exists so a caller that re-orders priorities on a LIVE stream can state what the
// hardware holds rather than what it asked for. The rate-monotonic invariant -- the leg with
// the higher sample rate holds the higher priority -- is only checkable with a read side.
uint8_t nora_spi_i2s_tdm_inst_irq_priority( const nora_spi_i2s_tdm_inst_t* inst );

// Snapshot the load monitor / status. The singleton forms report the PRIMARY leg
// (primary_leg_index, default logical leg 0); the inst forms report a specific instance.
// For the inst forms, block_count/deadline_miss/load AND running are that instance's;
// only active (the clock/source readiness gate) is engine-wide/shared.
// clear_peak resets that instance's min/max/event peaks after the snapshot.
extern bool nora_spi_i2s_tdm_get_load( nora_spi_i2s_tdm_load_t* monitor, bool clear_peak );
extern bool nora_spi_i2s_tdm_get_status( nora_spi_i2s_tdm_status_t* status, bool clear_peak );
extern bool nora_spi_i2s_tdm_inst_get_load( nora_spi_i2s_tdm_inst_t* inst,
                                                 nora_spi_i2s_tdm_load_t* monitor,
                                                 bool clear_peak );
extern bool nora_spi_i2s_tdm_inst_get_status( nora_spi_i2s_tdm_inst_t* inst,
                                                   nora_spi_i2s_tdm_status_t* status,
                                                   bool clear_peak );



// Last-error diagnostic. The bool-returning calls (set_port / open / close / inst_configure /
// configure_system / inst_start / inst_stop / start_domain / start_all_domains / stop_domain /
// stop_all_domains / set_block_callback) collapse several failure causes into one `false`;
// get_last_error() returns the most specific reason recorded by the most recent such call
// (ERR_NONE after a success). This is a DEBUG aid only -- NOT stream health: deadline misses /
// block counts live in get_status(), not here. It is the "last failed API reason", not a
// per-instance latch, and is intentionally not strictly interrupt/multi-core safe (a plain
// last-writer-wins store -- adequate as a 16-bit-MCU debug hint).
typedef enum {
    NORA_SPI_I2S_TDM_ERR_NONE = 0,
    NORA_SPI_I2S_TDM_ERR_BAD_INSTANCE,        // NULL / out-of-range instance handle
    NORA_SPI_I2S_TDM_ERR_BAD_ARGUMENT,        // NULL cfg / other bad argument
    NORA_SPI_I2S_TDM_ERR_NOT_CONFIGURED,      // start before a successful configure
    NORA_SPI_I2S_TDM_ERR_ALREADY_RUNNING,     // start/configure while running
    NORA_SPI_I2S_TDM_ERR_UNSUPPORTED_CONFIG,  // configure envelope rejected (format/slots/blk)
    NORA_SPI_I2S_TDM_ERR_TOPOLOGY,            // resource/domain topology: duplicate SPI/DMA, >1
                                                   // clock MASTER or a framing mismatch within a sync
                                                   // domain, bad primary index, or sync_domain >= 32
    NORA_SPI_I2S_TDM_ERR_CLOCK_INIT,          // port clock_source_init hook failed
    NORA_SPI_I2S_TDM_ERR_CLOCK_NOT_READY,     // port clock_source_ready hook not ready
    NORA_SPI_I2S_TDM_ERR_PIN_CONFIG,          // port configure_pins hook failed
    NORA_SPI_I2S_TDM_ERR_CLC,                 // port clc_passthrough hook failed
    NORA_SPI_I2S_TDM_ERR_DMA_CONFIG,          // DMA channel setup failed
    NORA_SPI_I2S_TDM_ERR_NOT_OPEN,            // start/arm attempted before a successful open()
    NORA_SPI_I2S_TDM_ERR_ALREADY_OPEN,        // configure/set_port attempted while open()'d
    NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,         // wrong configure-ownership mode for this call
                                                   // (e.g. inst_* under SYSTEM / a non-primary leg,
                                                   // or start_domain under SINGLE)
} nora_spi_i2s_tdm_error_t;

extern nora_spi_i2s_tdm_error_t nora_spi_i2s_tdm_get_last_error( void );

// DMA interrupt vectors: by default (NORA_TDM_DEFINE_DMA_VECTORS=1, conf.h) the HAL
// DEFINES the _DMAnInterrupt vectors itself (one RX vector per instance descriptor row),
// so the integrator writes NO interrupt/DMA code -- just registers a per-instance block
// callback. TX is interrupt-less (fire-and-forget ping-pong with auto-reload; hw.c
// enables the CPU IRQ on the RX channel only). RX/TX channel numbers come from conf.h and
// are baked in as compile-time constants so the DMA register access folds. The HAL-owned RX
// vectors are EXPLICIT (_DMA0Interrupt / _DMA2Interrupt for the default channels), each bound
// to its leg's conf.h RX-DMA channel by a compile-time assert -- so simply changing a leg's
// RX-DMA channel in conf.h fails the build until the matching explicit vector in the core
// source is updated too (the vector name does not auto-follow). To hand an IVT slot to another
// subsystem without touching the core, take full vector ownership: set
// NORA_TDM_DEFINE_DMA_VECTORS=0 (the HAL then defines no vectors) and call inst_rx_isr()
// below from your own _DMA<rx>Interrupt for each instance. (See conf.h_example for the same note.)

// RX-block ISR entry for one instance, for the NORA_TDM_DEFINE_DMA_VECTORS=0
// (vector-ownership opt-out) path: call it from your own _DMA<rx>Interrupt for that
// instance's RX channel (TX is interrupt-less -- never call it for a TX channel). It runs
// the same block work as the HAL's own explicit vector. A NULL inst is ignored. In the
// default turnkey build (=1) you do not call this -- the HAL's own vectors do the work.
extern void nora_spi_i2s_tdm_inst_rx_isr( nora_spi_i2s_tdm_inst_t* inst );



#endif // NORA_SPI_I2S_TDM_H
