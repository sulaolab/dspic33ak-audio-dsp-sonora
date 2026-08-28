#ifndef NORA_SPI_I2S_TDM_CONF_H
#define NORA_SPI_I2S_TDM_CONF_H

//===========================================================
// board/audio/nora_spi_i2s_tdm_conf.h  (project-supplied HAL config)
//
// This is the PROJECT's HAL config (bring-your-own-config pattern, like
// FreeRTOSConfig.h / lwipopts.h). The HAL folder ships only the standalone
// template (nora_spi_i2s_tdm_conf.h_example); the project supplies this file.
//
// This in-tree file includes resolved_transport_config.h and derives the
// NORA_TDM_* settings from neutral transport facts, so any geometry/topology
// change selected by the transitional resolver is automatically reflected here --
// no manual sync needed.
//
// Dependency direction:
//   conf.h -> resolved_transport_config.h -> transitional app config resolver
//   app-specific headers MUST NOT include conf.h (no reverse dependency)
//
// Each NORA_TDM_* is still -D-overridable (#ifndef-guarded). Cross-checks at
// the end detect any -D override that conflicts with the resolved transport facts.
//
// Compile-time integration settings:
//   NORA_TDM_SLOTS_PER_FS   slots per frame-sync: TDM8 = 8, I2S = 2.
//   NORA_TDM_BLOCK_FRAMES   frames per ping/pong half (DMA block size).
//   NORA_TDM_USE_SPI2      1 = SPI2 Audio transport is part of this build.
//   NORA_TDM_USE_SPI3/4    1 = experimental second TDM8 pair is built.
// (Sample rate is NOT a setting here -- the transport is rate-agnostic.)
// The core's static DMA ping-pong buffers are sized 2 * APP_SLOTS_PER_FS *
// BLOCK_FRAMES, and inst_configure() / configure_system() reject a config_t whose slots_per_fs /
// block_frames do not match these compile-time values.
//===========================================================

// Pull in the neutral facts. NORA_TDM_* will be derived from them below.
#include "resolved_transport_config.h"

// Verify the transitional resolver completed successfully.
#ifndef RESOLVED_TRANSPORT_CONFIG_READY
  #error "resolved_transport_config.h did not complete."
#endif

// --- HAL geometry / topology: derived from neutral facts (-D still wins) ---
#ifndef NORA_TDM_SLOTS_PER_FS
#define NORA_TDM_SLOTS_PER_FS           RESOLVED_TRANSPORT_SLOTS_PER_FRAME
#endif
#ifndef NORA_TDM_BLOCK_FRAMES
#define NORA_TDM_BLOCK_FRAMES           RESOLVED_TRANSPORT_BLOCK_FRAMES
#endif
#ifndef NORA_TDM_USE_SPI2
#define NORA_TDM_USE_SPI2               RESOLVED_TRANSPORT_LEG_B_PRESENT
#endif
#ifndef NORA_TDM_USE_SPI3
#define NORA_TDM_USE_SPI3               RESOLVED_TRANSPORT_USE_SPI34_EXPANSION
#endif
#ifndef NORA_TDM_USE_SPI4
#define NORA_TDM_USE_SPI4               RESOLVED_TRANSPORT_USE_SPI34_EXPANSION
#endif

#ifndef NORA_TDM_BASE_ON_SPI34
#define NORA_TDM_BASE_ON_SPI34          RESOLVED_TRANSPORT_BASE_ON_SPI34
#endif


//===========================================================
// DMA channel allocation (single source of truth for the SPI<->DMA binding)
//
// Each built physical SPI leg owns one RX and one TX DMA channel. These macros are read
// by the HAL core for its s_spi_legs[] table. The default HAL-owned RX vectors are explicit
// C entries for DMA0/2/4/6, each bound to its leg by a compile-time assert; vector names do
// NOT auto-follow channel changes. Each channel macro is -D overridable.
//
// dsPIC33A lets ANY DMA channel be triggered by ANY SPI (the channel's DMAxSEL CHSEL selects
// the peripheral), so any distinct, free channel works electrically -- BUT changing a leg's
// RX-DMA number here alone fails the build (the explicit vector's assert no longer matches);
// to actually move an RX channel you must also update the matching _DMA<rx>Interrupt in the
// core (or set NORA_TDM_DEFINE_DMA_VECTORS=0 and own the vector yourself). TX channels
// have no vector and move freely.
//
// CHIP-WIDE DMA CHANNEL MAP (maintain by hand -- the HAL cannot see other subsystems'
// DMA usage, so this comment is the conflict-avoidance reference, not a code guard):
//   DMA0/1 : TDM SPI1 RX/TX     (defaults below)
//   DMA2/3 : TDM SPI2 RX/TX     (when NORA_TDM_USE_SPI2)
//   DMA4/5 : TDM SPI3 RX/TX     (when NORA_TDM_USE_SPI3)
//   DMA6/7 : TDM SPI4 RX/TX     (when NORA_TDM_USE_SPI4)
//   DMA4-7 : PWM audio output   (when ENA_PWM_AUDIO) -- mutually exclusive with SPI3/4
// The HAL self-checks its OWN consistency: topology validation rejects channel duplication
// across all built legs. Cross-subsystem clashes remain the integrator's responsibility. A
// duplicate RX binding is also caught by the explicit-vector compile-time assertions.
//===========================================================
#ifndef NORA_TDM_SPI1_RX_DMA
#define NORA_TDM_SPI1_RX_DMA   0
#endif
#ifndef NORA_TDM_SPI1_TX_DMA
#define NORA_TDM_SPI1_TX_DMA   1
#endif
#ifndef NORA_TDM_SPI2_RX_DMA
#define NORA_TDM_SPI2_RX_DMA   2
#endif
#ifndef NORA_TDM_SPI2_TX_DMA
#define NORA_TDM_SPI2_TX_DMA   3
#endif
#ifndef NORA_TDM_SPI3_RX_DMA
#define NORA_TDM_SPI3_RX_DMA   4
#endif
#ifndef NORA_TDM_SPI3_TX_DMA
#define NORA_TDM_SPI3_TX_DMA   5
#endif
#ifndef NORA_TDM_SPI4_RX_DMA
#define NORA_TDM_SPI4_RX_DMA   6
#endif
#ifndef NORA_TDM_SPI4_TX_DMA
#define NORA_TDM_SPI4_TX_DMA   7
#endif


//===========================================================
// DMA interrupt-vector ownership.
//
//   1 (default) : TURNKEY -- the HAL DEFINES explicit _DMA<rx>Interrupt vectors itself
//                 (one per built physical leg; they are not generated from channel macros). Link the HAL and the IVT
//                 slots are filled; the integrator writes no interrupt/DMA code.
//   0           : the HAL defines NO vectors. The integrator owns the IVT and, from
//                 their own _DMA<rx>Interrupt, calls
//                 nora_spi_i2s_tdm_inst_rx_isr(spiN()) for each instance's RX
//                 channel. Use this when the project already manages the DMA IVT or
//                 must share vector ownership with another subsystem.
//
// Set to 0 with -D or before including this header. Either way the RX channel still
// raises the interrupt and TX stays interrupt-less (fire-and-forget ping-pong).
//===========================================================
#ifndef NORA_TDM_DEFINE_DMA_VECTORS
#define NORA_TDM_DEFINE_DMA_VECTORS   1
#endif




//===========================================================
// Instance count + physical assignment.
//
// The transport core no longer generates its leg table from an X-macro list -- it defines
// the leg enum, per-instance ping-pong buffers, the s_spi_legs[] table, and the explicit
// _DMA<rx>Interrupt vectors directly in C, keyed off the per-instance channel #defines
// above (NORA_TDM_SPIn_RX/TX_DMA) and the stream geometry (NORA_TDM_SLOTS_PER_FS /
// _BLOCK_FRAMES). By default, logical rows 0/1 map to physical SPI1/SPI2.
// NORA_TDM_BASE_ON_SPI34 explicitly remaps those same two rows to SPI3/SPI4;
// NORA_TDM_USE_SPI3/4 instead add physical SPI3/SPI4 rows after SPI1/SPI2.
//
// The per-leg clock role, sync domain, and (rate-agnostic) stream shape are NOT set here;
// they are resolved at runtime from the board TDM topology table
// (board/audio/tdm.c) and applied via nora_spi_i2s_tdm_configure_system().
//===========================================================

// Per-leg SYNC DOMAIN id. NOTE: since configure_system() now commits each leg's sync_domain
// from the board topology table at runtime, these macros are only the s_spi_legs[] default
// (the pre-configure seed); the topology table is the authoritative source once the app
// configures. Legs sharing a domain are co-clocked and started phase-locked as a group
// (start_domain arms all, then releases SPIEN back-to-back -> one FS edge); legs in different
// domains are started/rolled-back separately and need not share BCLK/FS -- but this is NOT full
// independence (source-readiness is engine-wide/primary-gated; CLC10 + the clock port are shared).
// This is NOT the clock role (config_t.clock_role).
//   - SPI1: domain 0 (the demo's co-clocked group anchor).
//   - SPI2: domain 0 when co-clocked with SPI1 (rides SPI1's BCLK/FS); its OWN domain 1 when leg B is
//           an independent ASRC domain -- either the dsPIC SPI2 is the master (INDEPENDENT_MASTER) or
//           the WM8904-B chip is (B_CODEC_MASTER). Async to A: ASRC is a data-path relation, not clock
//           sync. (This conf.h value is only the initial seed; configure_system re-applies the value
//           from the leg table -- keep both keyed on the same resolved topology.)
#ifndef NORA_TDM_SPI1_SYNC_DOMAIN
#define NORA_TDM_SPI1_SYNC_DOMAIN   (0)
#endif
#ifndef NORA_TDM_SPI2_SYNC_DOMAIN
#define NORA_TDM_SPI2_SYNC_DOMAIN \
    ((RESOLVED_TRANSPORT_TOPOLOGY == \
      TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE) ? 1 : 0)
#endif
#ifndef NORA_TDM_SPI3_SYNC_DOMAIN
#define NORA_TDM_SPI3_SYNC_DOMAIN   (2)
#endif
#ifndef NORA_TDM_SPI4_SYNC_DOMAIN
#define NORA_TDM_SPI4_SYNC_DOMAIN   (3)
#endif

// sync_domain must be 0..31 (start_all_domains()'s dedup/rollback mask range). Reject negatives
// too: a negative literal would cast to a large uint8_t at runtime.
#if ((NORA_TDM_SPI1_SYNC_DOMAIN) < 0) || ((NORA_TDM_SPI1_SYNC_DOMAIN) >= 32)
#error "NORA_TDM_SPI1_SYNC_DOMAIN must be in 0..31."
#endif
#if NORA_TDM_USE_SPI2 && (((NORA_TDM_SPI2_SYNC_DOMAIN) < 0) || ((NORA_TDM_SPI2_SYNC_DOMAIN) >= 32))
#error "NORA_TDM_SPI2_SYNC_DOMAIN must be in 0..31."
#endif
#if NORA_TDM_USE_SPI3 && (((NORA_TDM_SPI3_SYNC_DOMAIN) < 0) || ((NORA_TDM_SPI3_SYNC_DOMAIN) >= 32))
#error "NORA_TDM_SPI3_SYNC_DOMAIN must be in 0..31."
#endif
#if NORA_TDM_USE_SPI4 && (((NORA_TDM_SPI4_SYNC_DOMAIN) < 0) || ((NORA_TDM_SPI4_SYNC_DOMAIN) >= 32))
#error "NORA_TDM_SPI4_SYNC_DOMAIN must be in 0..31."
#endif


#if (NORA_TDM_SLOTS_PER_FS <= 0)
#error "NORA_TDM_SLOTS_PER_FS must be positive."
#endif

#if (NORA_TDM_SLOTS_PER_FS > 255)
#error "NORA_TDM_SLOTS_PER_FS must fit in uint8_t."
#endif

#if (NORA_TDM_BLOCK_FRAMES <= 0)
#error "NORA_TDM_BLOCK_FRAMES must be positive."
#endif

#if (NORA_TDM_BLOCK_FRAMES > 65535)
#error "NORA_TDM_BLOCK_FRAMES must fit in uint16_t."
#endif

#if ((NORA_TDM_USE_SPI2 != 0) && (NORA_TDM_USE_SPI2 != 1))
#error "NORA_TDM_USE_SPI2 must be 0 or 1."
#endif
#if ((NORA_TDM_USE_SPI3 != 0) && (NORA_TDM_USE_SPI3 != 1))
#error "NORA_TDM_USE_SPI3 must be 0 or 1."
#endif
#if ((NORA_TDM_USE_SPI4 != 0) && (NORA_TDM_USE_SPI4 != 1))
#error "NORA_TDM_USE_SPI4 must be 0 or 1."
#endif
#if ((NORA_TDM_BASE_ON_SPI34 != 0) && (NORA_TDM_BASE_ON_SPI34 != 1))
#error "NORA_TDM_BASE_ON_SPI34 must be 0 or 1."
#endif
#if (NORA_TDM_USE_SPI3 != NORA_TDM_USE_SPI4)
#error "The experimental second TDM8 transport requires SPI3 and SPI4 together."
#endif
#if NORA_TDM_BASE_ON_SPI34 && (NORA_TDM_USE_SPI3 || NORA_TDM_USE_SPI4)
#error "SPI34 test-bank mode and simultaneous four-leg mode are mutually exclusive."
#endif
#if NORA_TDM_USE_SPI3 && !NORA_TDM_USE_SPI2
#error "SPI3/SPI4 expansion currently requires the existing SPI1/SPI2 ASRC pair."
#endif

#if (NORA_TDM_SLOTS_PER_FS > (2147483647 / (2 * NORA_TDM_BLOCK_FRAMES)))
#error "SPI/I2S/TDM DMA buffer geometry overflows the static buffer element count."
#endif


// --- Resolved transport facts vs NORA_TDM_* cross-checks ---
// These are SAFE here: both sides are fully defined by this point.
// With the defaults above these checks are trivially true. They only fire when a
// -D override produces a conflict with the resolved static contract.
#if (NORA_TDM_SLOTS_PER_FS != RESOLVED_TRANSPORT_SLOTS_PER_FRAME)
  #error "NORA_TDM_SLOTS_PER_FS conflicts with the resolved slots/frame."
#endif
#if (NORA_TDM_BLOCK_FRAMES != RESOLVED_TRANSPORT_BLOCK_FRAMES)
  #error "NORA_TDM_BLOCK_FRAMES conflicts with the resolved block size."
#endif
#if (NORA_TDM_USE_SPI2 != RESOLVED_TRANSPORT_LEG_B_PRESENT)
  #error "NORA_TDM_USE_SPI2 conflicts with resolved leg-B presence."
#endif
#if (NORA_TDM_USE_SPI3 != RESOLVED_TRANSPORT_USE_SPI34_EXPANSION) || \
    (NORA_TDM_USE_SPI4 != RESOLVED_TRANSPORT_USE_SPI34_EXPANSION)
  #error "NORA_TDM_USE_SPI3/4 conflicts with the resolved SPI3/4 expansion."
#endif

#endif // NORA_SPI_I2S_TDM_CONF_H
