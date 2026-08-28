#ifndef RESIDENT_DE_ARM_TIMING_H
#define RESIDENT_DE_ARM_TIMING_H

//===========================================================
// shared/ -- how long the update wait lasts, in ONE place.
//
// WHY THIS IS SHARED (the same argument resident_de_abi.h makes for records)
//
// The application prints this figure when *fu5A is accepted, and the boot image is
// what actually enforces it. Those are two separately compiled images, so a copy in
// each is a message that can quietly start lying: the operator is told "about 60 s",
// waits, and the board boots away at 30 s -- or does not boot away at all. Nothing
// reports the drift. One definition makes the drift impossible.
//
// This is not an ABI record (no bytes cross the reset because of it) and it does not
// belong in resident_de_abi.h's version pairing. It is here because this folder is
// the one both images already compile, and because the failure mode is the same one
// that folder exists to prevent.
//
// SHAPE OF THE WAIT, so the numbers below are readable:
//
//   resident_xmodem_receive() sends 'C' (XMODEM-CRC handshake) and waits
//   HANDSHAKE_MS for a header, up to HANDSHAKE_TRIES times -- one "round", about
//   30 s of nobody sending. resident_de_boot_main.c counts consecutive idle rounds
//   and launches the installed application after IDLE_ROUNDS of them.
//
//   A round that FAILS rather than idles (bad CRC, cancel, corrupt header) resets
//   the idle count on purpose: an operator is clearly present, so the wait keeps
//   waiting for their retry. The timeout below is therefore the "nobody is there"
//   figure, not a cap on a transfer that is going wrong.
//===========================================================

// One handshake attempt: send 'C', then wait this long for the sender's header.
#define RESIDENT_DE_ARM_HANDSHAKE_MS      3000u
// Attempts per round.
#define RESIDENT_DE_ARM_HANDSHAKE_TRIES   10u
// Consecutive idle rounds before the wait gives up and launches the application.
// Only consulted when there IS a valid application to fall back to and it is not
// itself the suspect -- otherwise the wait is deliberately endless.
//
// 4 rounds (120 s total), not 2 (60 s): an operator driving a GUI XMODEM sender
// (Tera Term's File > Transfer dialog, browsing to the .sfb) routinely spends
// part of the window just navigating menus before the first byte goes out, and
// 60 s measured too tight in practice (2026-08-16).
#define RESIDENT_DE_ARM_IDLE_ROUNDS       4u

// The total, as a bare literal. It has to be a literal because the boot image has no
// printf and can only put this on the wire by stringifying it. The assertion below is
// what keeps the literal honest, so editing any constant above without editing this
// one stops the build instead of shipping a wrong number.
#define RESIDENT_DE_ARM_TIMEOUT_S_N       120
#define RESIDENT_DE_ARM_TIMEOUT_S         ((unsigned)RESIDENT_DE_ARM_TIMEOUT_S_N)

_Static_assert((RESIDENT_DE_ARM_TIMEOUT_S_N * 1000) ==
               (int)(RESIDENT_DE_ARM_HANDSHAKE_MS *
                     RESIDENT_DE_ARM_HANDSHAKE_TRIES *
                     RESIDENT_DE_ARM_IDLE_ROUNDS),
               "resident arm timeout literal no longer matches the constants it "
               "summarizes: update RESIDENT_DE_ARM_TIMEOUT_S_N");

// String forms for the boot image, which has no printf. Deliberately named apart from
// resident_de_abi.h's stringify pair so neither header has to be included first.
#define RESIDENT_DE_ARM_STR_(x) #x
#define RESIDENT_DE_ARM_STR(x)  RESIDENT_DE_ARM_STR_(x)
#define RESIDENT_DE_ARM_TIMEOUT_S_STR   RESIDENT_DE_ARM_STR(RESIDENT_DE_ARM_TIMEOUT_S_N)
#define RESIDENT_DE_ARM_HANDSHAKE_S_STR "3"   /* HANDSHAKE_MS as whole seconds, for prose */

_Static_assert(RESIDENT_DE_ARM_HANDSHAKE_MS == 3000u,
               "RESIDENT_DE_ARM_HANDSHAKE_S_STR says 3 s; keep it with HANDSHAKE_MS");

#endif
