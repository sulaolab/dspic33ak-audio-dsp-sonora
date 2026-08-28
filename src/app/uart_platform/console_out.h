/* SPDX-License-Identifier: MIT-0 */
#ifndef CONSOLE_OUT_H
#define CONSOLE_OUT_H

/*
 * console_out.h -- the dsPIC33A side of the console output seam a portable
 * application prints through.
 *
 * WHY THIS EXISTS HERE
 * --------------------
 * It is not this project's console. This project prints with printf(), retargeted
 * through uart_platform/uart_platform_stdio.c, and every module in it does so
 * directly. This header exists because an application that came from the other
 * NORA family prints through console_out_*(), and the contract review's
 * conformance gate is "that application moves here unchanged"
 * ([internal] nora_dma_tdm_contract_draft_2026-08-10.md, section 4).
 * Without the seam the moving application would have to be edited on arrival, and
 * the edit would be in the output calls -- which proves nothing about the HAL
 * contract and hides what the gate is trying to measure.
 *
 * So: three functions, the ones a moved application actually calls, mapped onto
 * printf. Deliberately `static inline` rather than a .c file, because on this side
 * they are an adapter and not a transport -- there is no second output path to
 * keep consistent, and an unreferenced inline costs nothing when no such
 * application is built in.
 *
 * WHY IT IS NOT THE WHOLE SEAM
 * ----------------------------
 * The originating family's console_out.h is larger: console_out_char() for its
 * command parser's echo, console_out_idle() for its reset acknowledgement, and
 * console_in_*() for input. All four exist because that family's console IS this
 * seam. Here the console is app_console over stdio, so those halves have no
 * counterpart and inventing them would be inventing a concept this side does not
 * have (contract rule R0, second consequence). Add a function here when an actual
 * moving consumer calls it.
 *
 * LINE ENDINGS
 * ------------
 * The originating family translates a bare '\n' into CR LF inside this seam and
 * forbids '\r' in source strings. Here the same translation happens one layer
 * down, in uart_platform_stdio.c's write() hook, so a moved string prints
 * identically without this header doing anything about it. Do not add a second
 * translation: it would emit CR CR LF, which is invisible in a terminal and
 * therefore the kind of defect you grep for instead of seeing.
 */

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NUL-terminated. Where the lines break is the caller's business; the line
 * ending is handled by the stdio write() hook (see above). */
static inline void console_out_str(const char *s)
{
    printf("%s", s);
}

/* Unsigned decimal, no padding. */
static inline void console_out_u32(uint32_t value)
{
    printf("%lu", (unsigned long)value);
}

/* Fixed-width four-digit hex, for register evidence. Fixed width is the point:
 * register values get read bit by bit, and a variable-width field moves the bit
 * positions between lines, which is where a misread becomes a wrong diagnosis. */
static inline void console_out_hex16(uint16_t value)
{
    printf("%04X", (unsigned int)value);
}

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_OUT_H */
