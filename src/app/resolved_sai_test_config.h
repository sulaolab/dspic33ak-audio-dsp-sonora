#ifndef SONORA_RESOLVED_SAI_TEST_CONFIG_H
#define SONORA_RESOLVED_SAI_TEST_CONFIG_H

/* Transitional compile-time adapter for the opt-in CMSIS-SAI verification harnesses. */
#include "app_specific_config_defs.h"
#include "resolved_transport_config.h"

#define RESOLVED_SAI_TEST_DRY_RUN_ENABLED     (APP_USE_SAI_WRAPPER_DRYTEST)
#define RESOLVED_SAI_TEST_LIVE_ENABLED        (APP_USE_SAI_WRAPPER_LIVE)
#define RESOLVED_SAI_TEST_TONE_ENABLED        (APP_USE_SAI_WRAPPER_LIVE_TONE)
#define RESOLVED_SAI_TEST_HYBRID_TX_ENABLED   (APP_USE_SAI_HYBRID_OUT_WRAP)
#define RESOLVED_SAI_TEST_KEEPALIVE_ENABLED   (APP_USE_SAI_LIVE_KEEPALIVE)

#define RESOLVED_SAI_TEST_BLOCK_FRAMES        (RESOLVED_TRANSPORT_BLOCK_FRAMES)
#define RESOLVED_SAI_TEST_SLOTS_PER_FRAME     (RESOLVED_TRANSPORT_SLOTS_PER_FRAME)
#define RESOLVED_SAI_TEST_SAMPLE_RATE_HZ      \
    (RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ)

_Static_assert( RESOLVED_SAI_TEST_DRY_RUN_ENABLED == APP_USE_SAI_WRAPPER_DRYTEST,
                "resolved SAI dry-test selection must match app config" );
_Static_assert( RESOLVED_SAI_TEST_LIVE_ENABLED == APP_USE_SAI_WRAPPER_LIVE,
                "resolved SAI live selection must match app config" );
_Static_assert( RESOLVED_SAI_TEST_TONE_ENABLED == APP_USE_SAI_WRAPPER_LIVE_TONE,
                "resolved SAI tone selection must match app config" );
_Static_assert( RESOLVED_SAI_TEST_HYBRID_TX_ENABLED == APP_USE_SAI_HYBRID_OUT_WRAP,
                "resolved SAI hybrid selection must match app config" );
_Static_assert( RESOLVED_SAI_TEST_KEEPALIVE_ENABLED == APP_USE_SAI_LIVE_KEEPALIVE,
                "resolved SAI keepalive selection must match app config" );

#define RESOLVED_SAI_TEST_CONFIG_READY  (1)

#endif /* SONORA_RESOLVED_SAI_TEST_CONFIG_H */
