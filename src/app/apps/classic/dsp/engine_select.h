#ifndef ENGINE_SELECT_H
#define ENGINE_SELECT_H

/* The engine synth's public face, whichever model is compiled.
 *
 * Two models define the same app_engine_synth_* entry points:
 *
 *   engine_v8.c      the resynthesised model (sections 39-45). The shipping sound.
 *   engine_synth.c   the hand-built model it replaced. ENA_ENGINE_SYNTH_LEGACY.
 *
 * Each file compiles its body only for its own setting of ENA_ENGINE_SYNTH_LEGACY,
 * so exactly one is ever linked even though both are registered in the MPLAB
 * project. Consumers include THIS header rather than either model's, so adding or
 * swapping a model touches this file and not the four call sites
 * (classic_console.c, classic_controls.c, classic_demo_app.c, fx_domain_48k.c).
 *
 * The two APIs are not identical: engine_v8 adds the `*cy` bring-up ladder
 * (app_engine_synth_set_stage / get_stage) and app_engine_synth_report(). The
 * legacy model predates all three, so callers guard those on
 * ENA_ENGINE_SYNTH_LEGACY. The shared core -- init_48k, process_sample_48k,
 * enable, is_enable, blip_start -- is common and needs no guard.
 *
 * engine_synth.h states its types (biquad_t and friends) without including what
 * declares them, so app_utils.h has to come first. That is why this header pulls it
 * in rather than leaving each caller to remember; classic_demo_app.c carried a
 * comment about exactly this trap. */

#include "app_specific_config_defs.h"
#include "app_utils.h"

#if defined(ENA_ENGINE_SYNTH_LEGACY)
#include "engine_synth.h"
#else
#include "engine_v8.h"
#endif

#endif // ENGINE_SELECT_H
