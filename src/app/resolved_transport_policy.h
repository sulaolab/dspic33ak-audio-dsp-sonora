#ifndef SONORA_RESOLVED_TRANSPORT_POLICY_H
#define SONORA_RESOLVED_TRANSPORT_POLICY_H

/*
 * Transitional compile-time adapter for transport runtime policy.
 *
 * The transport orchestrator consumes these application-neutral names.  The
 * existing APP_* configuration remains authoritative until final build
 * composition replaces this adapter with an explicit product policy object.
 */
#include "app_specific_config_defs.h"
#include "resolved_transport_config.h"

#define RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED \
    (APP_TDM_PHASE_PROBE)
#define RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED \
    (APP_TDM_SYNC_GUARD)
#define RESOLVED_TRANSPORT_SYNC_GUARD_TRIP_BLOCKS \
    (APP_TDM_SYNC_GUARD_TRIP)

#define RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED \
    (APP_TDM_STARTUP_PHASE_LOCK)
#define RESOLVED_TRANSPORT_PHASE_LOCK_MAX_RETRIES \
    (APP_TDM_LOCK_RETRIES)
#define RESOLVED_TRANSPORT_PHASE_LOCK_REQUIRED_BLOCKS \
    (APP_TDM_LOCK_CONSEC_OK)
#define RESOLVED_TRANSPORT_PHASE_LOCK_WAIT_MS \
    (APP_TDM_LOCK_WAIT_MS)
#define RESOLVED_TRANSPORT_PHASE_LOCK_TOLERANCE_WORDS \
    (APP_TDM_LOCK_TOL_WORDS)
#define RESOLVED_TRANSPORT_MIRROR_UNRESOLVED_TOLERANCE_BLOCKS  (3u)

#define RESOLVED_TRANSPORT_FRMERR_AUTORECOVERY_ENABLED \
    (APP_TDM_FRMERR_AUTORECOVER)
#define RESOLVED_TRANSPORT_FRMERR_TRIGGER_BLOCKS \
    (APP_TDM_FRMERR_RECOVER_BLOCKS)
#define RESOLVED_TRANSPORT_RECOVERY_CHECK_PERIOD_MS \
    (APP_TDM_FRMERR_CHECK_MS)
#define RESOLVED_TRANSPORT_RECOVERY_COOLDOWN_MS \
    (APP_TDM_FRMERR_COOLDOWN_MS)
#define RESOLVED_TRANSPORT_RECOVERY_QUALIFY_BLOCKS \
    (APP_TDM_FRMERR_RELOCK_BLOCKS)
#define RESOLVED_TRANSPORT_RECOVERY_QUALIFY_TIMEOUT_MS \
    (APP_TDM_FRMERR_RELOCK_TIMEOUT_MS)
#define RESOLVED_TRANSPORT_RECOVERY_MAX_RESTARTS \
    (APP_TDM_FRMERR_MAX_RESTART)
#define RESOLVED_TRANSPORT_LIVENESS_STALL_TIMEOUT_MS \
    (APP_TDM_LIVENESS_STALL_MS)

#define RESOLVED_TRANSPORT_DEBUG_PERIOD_MS  (APP_DBG_PERIOD_MS)

#define RESOLVED_TRANSPORT_DUAL_CLOCK_PROGRESS_ENABLED \
    ((RESOLVED_TRANSPORT_TOPOLOGY == \
      TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE) && \
     (RESOLVED_TRANSPORT_LEG_A_CLOCK_PROGRESS == \
      TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE_VALUE) && \
     (RESOLVED_TRANSPORT_LEG_B_CLOCK_PROGRESS == \
      TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE_VALUE))

_Static_assert( RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED == APP_TDM_PHASE_PROBE,
                "resolved phase probe policy must match app config" );
_Static_assert( RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED == APP_TDM_SYNC_GUARD,
                "resolved sync guard policy must match app config" );
_Static_assert( RESOLVED_TRANSPORT_SYNC_GUARD_TRIP_BLOCKS == APP_TDM_SYNC_GUARD_TRIP,
                "resolved sync guard threshold must match app config" );
_Static_assert( RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED == APP_TDM_STARTUP_PHASE_LOCK,
                "resolved startup lock policy must match app config" );
_Static_assert( RESOLVED_TRANSPORT_PHASE_LOCK_MAX_RETRIES == APP_TDM_LOCK_RETRIES,
                "resolved phase-lock retry budget must match app config" );
_Static_assert( RESOLVED_TRANSPORT_PHASE_LOCK_REQUIRED_BLOCKS == APP_TDM_LOCK_CONSEC_OK,
                "resolved phase-lock qualification must match app config" );
_Static_assert( RESOLVED_TRANSPORT_PHASE_LOCK_WAIT_MS == APP_TDM_LOCK_WAIT_MS,
                "resolved phase-lock wait must match app config" );
_Static_assert( RESOLVED_TRANSPORT_PHASE_LOCK_TOLERANCE_WORDS == APP_TDM_LOCK_TOL_WORDS,
                "resolved phase-lock tolerance must match app config" );
_Static_assert( RESOLVED_TRANSPORT_FRMERR_AUTORECOVERY_ENABLED == APP_TDM_FRMERR_AUTORECOVER,
                "resolved recovery policy must match app config" );
_Static_assert( RESOLVED_TRANSPORT_FRMERR_TRIGGER_BLOCKS == APP_TDM_FRMERR_RECOVER_BLOCKS,
                "resolved FRMERR threshold must match app config" );
_Static_assert( RESOLVED_TRANSPORT_RECOVERY_CHECK_PERIOD_MS == APP_TDM_FRMERR_CHECK_MS,
                "resolved recovery check period must match app config" );
_Static_assert( RESOLVED_TRANSPORT_RECOVERY_COOLDOWN_MS == APP_TDM_FRMERR_COOLDOWN_MS,
                "resolved recovery cooldown must match app config" );
_Static_assert( RESOLVED_TRANSPORT_RECOVERY_QUALIFY_BLOCKS == APP_TDM_FRMERR_RELOCK_BLOCKS,
                "resolved recovery qualification must match app config" );
_Static_assert( RESOLVED_TRANSPORT_RECOVERY_QUALIFY_TIMEOUT_MS == APP_TDM_FRMERR_RELOCK_TIMEOUT_MS,
                "resolved recovery timeout must match app config" );
_Static_assert( RESOLVED_TRANSPORT_RECOVERY_MAX_RESTARTS == APP_TDM_FRMERR_MAX_RESTART,
                "resolved recovery retry budget must match app config" );
_Static_assert( RESOLVED_TRANSPORT_LIVENESS_STALL_TIMEOUT_MS == APP_TDM_LIVENESS_STALL_MS,
                "resolved liveness timeout must match app config" );
_Static_assert( RESOLVED_TRANSPORT_DEBUG_PERIOD_MS == APP_DBG_PERIOD_MS,
                "resolved debug period must match app config" );

#define RESOLVED_TRANSPORT_POLICY_READY  (1)

#endif /* SONORA_RESOLVED_TRANSPORT_POLICY_H */
