#ifndef SONORA_TRANSPORT_LEG_COUNT_H
#define SONORA_TRANSPORT_LEG_COUNT_H

/*
 * How many transport leg slots this build allocates.
 *
 * This is the single number that widens when a build grows from the current
 * two-leg topology (16 ch A <-> 16 ch B, SPI1 + SPI2) to a four-leg one
 * (4 x 8 ch on SPI1..SPI4).  It lives in a header with no dependencies so
 * that the static configuration struct and the telemetry snapshot struct can
 * both size themselves from the same fact without either having to know about
 * application-level APP_* configuration.
 *
 * A wider build defines TRANSPORT_LEG_MAX ahead of this header (compiler
 * command line or preset header).  Legs past the count are *not* allocated:
 * that is deliberate.  It is why one binary cannot serve both topologies, and
 * equally why the narrow build pays nothing -- not a byte, not an
 * instruction -- for the wide build's existence.  Every array indexed by leg
 * is therefore exactly as long as this build needs, and every leg index at a
 * hot-path call site stays a compile-time constant.
 */
#ifndef TRANSPORT_LEG_MAX
#define TRANSPORT_LEG_MAX  (2u)
#endif

#if ( TRANSPORT_LEG_MAX < 1u ) || ( TRANSPORT_LEG_MAX > 4u )
#error "TRANSPORT_LEG_MAX must be 1..4 -- the device has SPI1..SPI4 and no more"
#endif

#endif /* SONORA_TRANSPORT_LEG_COUNT_H */
