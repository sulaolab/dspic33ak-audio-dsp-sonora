#include "audio_transport_client.h"

#include <stddef.h>

#include "resolved_transport_config.h"

static const audio_transport_client_t* s_client;

static bool client_hooks_match_topology( const audio_transport_client_t* client )
{
    if( RESOLVED_TRANSPORT_TOPOLOGY ==
        TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE )
    {
        return ( client->co_clock_process != NULL ) &&
               ( client->leg_a_process == NULL ) &&
               ( client->leg_b_process == NULL );
    }

    if( RESOLVED_TRANSPORT_TOPOLOGY ==
        TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE )
    {
        return ( client->co_clock_process == NULL ) &&
               ( client->leg_a_process != NULL ) &&
               ( client->leg_b_process != NULL );
    }

    return false;
}

static bool static_facts_require_clock_progress( void )
{
    /* Bind-time check, not a hot path: iterating the built legs is the form
     * that stays correct when a build allocates more than two. */
    for( unsigned leg = 0u; leg < TRANSPORT_LEG_MAX; ++leg )
    {
        const transport_leg_static_cfg_t* cfg =
            &g_resolved_transport_static_cfg.legs[leg];

        if( cfg->present &&
            ( cfg->clock_progress_source ==
              TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE ) )
        {
            return true;
        }
    }

    return false;
}

static audio_transport_client_bind_result_t client_capabilities_validate(
    const audio_transport_client_t* client )
{
    const uint32_t known = AUDIO_TRANSPORT_CLIENT_CAP_CLOCK_PROGRESS;
    const bool declares_progress =
        ( client->capabilities & AUDIO_TRANSPORT_CLIENT_CAP_CLOCK_PROGRESS ) != 0u;

    if( ( client->capabilities & ~known ) != 0u )
    {
        return AUDIO_TRANSPORT_CLIENT_BIND_ERR_CAPABILITY;
    }
    if( declares_progress != ( client->clock_progress != NULL ) )
    {
        return AUDIO_TRANSPORT_CLIENT_BIND_ERR_CAPABILITY;
    }
    if( declares_progress &&
        ( RESOLVED_TRANSPORT_TOPOLOGY !=
          TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE ) )
    {
        return AUDIO_TRANSPORT_CLIENT_BIND_ERR_CAPABILITY;
    }
    if( static_facts_require_clock_progress() && !declares_progress )
    {
        return AUDIO_TRANSPORT_CLIENT_BIND_ERR_MISSING_PROGRESS;
    }
    if( !static_facts_require_clock_progress() && declares_progress )
    {
        return AUDIO_TRANSPORT_CLIENT_BIND_ERR_CAPABILITY;
    }
    return AUDIO_TRANSPORT_CLIENT_BIND_OK;
}

audio_transport_client_bind_result_t audio_transport_client_bind(
    const audio_transport_client_t* client )
{
    if( client == NULL )
    {
        return AUDIO_TRANSPORT_CLIENT_BIND_ERR_NULL;
    }
    if( s_client != NULL )
    {
        return ( s_client == client )
            ? AUDIO_TRANSPORT_CLIENT_BIND_OK
            : AUDIO_TRANSPORT_CLIENT_BIND_ERR_ALREADY_BOUND;
    }
    if( ( client->prepare == NULL ) || ( client->reset_stream_state == NULL ) )
    {
        return AUDIO_TRANSPORT_CLIENT_BIND_ERR_REQUIRED_HOOK;
    }
    if( !client_hooks_match_topology( client ) )
    {
        return AUDIO_TRANSPORT_CLIENT_BIND_ERR_TOPOLOGY;
    }
    const audio_transport_client_bind_result_t capability_result =
        client_capabilities_validate( client );
    if( capability_result != AUDIO_TRANSPORT_CLIENT_BIND_OK )
    {
        return capability_result;
    }

    s_client = client;
    return AUDIO_TRANSPORT_CLIENT_BIND_OK;
}

const char* audio_transport_client_bind_result_name(
    audio_transport_client_bind_result_t result )
{
    switch( result )
    {
        case AUDIO_TRANSPORT_CLIENT_BIND_OK:                   return "ok";
        case AUDIO_TRANSPORT_CLIENT_BIND_ERR_NULL:             return "null-client";
        case AUDIO_TRANSPORT_CLIENT_BIND_ERR_ALREADY_BOUND:    return "already-bound";
        case AUDIO_TRANSPORT_CLIENT_BIND_ERR_REQUIRED_HOOK:    return "required-hook";
        case AUDIO_TRANSPORT_CLIENT_BIND_ERR_TOPOLOGY:         return "topology";
        case AUDIO_TRANSPORT_CLIENT_BIND_ERR_CAPABILITY:       return "capability";
        case AUDIO_TRANSPORT_CLIENT_BIND_ERR_MISSING_PROGRESS: return "missing-progress";
        default:                                               return "unknown";
    }
}

const audio_transport_client_t* audio_transport_client_get( void )
{
    return s_client;
}
