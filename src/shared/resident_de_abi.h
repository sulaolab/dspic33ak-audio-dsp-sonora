#ifndef RESIDENT_DE_ABI_H
#define RESIDENT_DE_ABI_H

//===========================================================
// shared/ -- the ABI between the resident download engine and the
// application. ABI, not API: the two are compiled separately, linked separately,
// and updated separately in the field, so what binds them is a run-time agreement
// on bytes and addresses rather than a set of function signatures.
//
// WHY THIS IS SHARED RATHER THAN DUPLICATED
//
// A common practice is to give the boot image its own copy of everything, so that
// updating the application cannot change the boot image. The goal is right; copying
// is the wrong mechanism *for this folder specifically*.
//
//   - For an ABI, divergence is the failure. Two copies that drift no longer agree
//     on where the mailbox is or what a manifest field means, and nothing reports
//     it -- the handover simply stops working, or worse, half works. Sharing one
//     definition makes divergence impossible by construction.
//
//   - For implementation code (a UART driver, a clock sequence) the argument is the
//     opposite way round, and there copying is exactly right. Since 2026-08-14 the
//     boot image compiles its OWN copy, boot/hal_*, and nothing out of app/: an
//     image pinned at fixed addresses under a hard 32 KiB cap must not move because
//     the application's HAL moved. buildtools/check_hal_drift.ps1 reports where the
//     two copies differ, and deliberately never fails a build.
//     See [internal] resident_serial_bootloader.md part 2.
//
//     So this folder is the one place where sharing is right and copying is wrong,
//     and boot/hal_* is the mirror case. The distinction is agreement-on-bytes
//     versus implementation, not "shared code is good".
//
// WHAT REPLACES THE COPY: versioned records, checked at run time.
//
//   manifest (Flash)  resident_de_manifest.h. Carries format_version; the engine
//                     validates it before trusting any other field, so an image
//                     built against a different manifest layout is refused rather
//                     than launched.
//
//   pipe (SRAM)       resident_de_pipe.h. Carries version + complement. A mismatch
//                     re-establishes the container instead of misreading it, so an
//                     old reader meeting a new writer degrades to "no message"
//                     rather than to a wrong message.
//
// This header is the umbrella over those two. Its only mechanism is the assertions
// below: change either record's version and the build stops here, which is the
// reminder to bump this umbrella too and to state the pairing in the release notes.
// A number nobody is forced to maintain is worse than no number at all.
//===========================================================

#include "resident_de_manifest.h"
#include "resident_de_pipe.h"

// Bump whenever either record below changes. Both images print it, so a field log
// answers "were these two built against the same agreement?" without inference.
#define RESIDENT_DE_ABI_VERSION_N 1
#define RESIDENT_DE_ABI_VERSION   UINT16_C(RESIDENT_DE_ABI_VERSION_N)

// String form for the engine, which has no printf -- one source of truth, so the
// two forms cannot disagree.
#define RESIDENT_DE_ABI_STRINGIFY_(x) #x
#define RESIDENT_DE_ABI_STRINGIFY(x)  RESIDENT_DE_ABI_STRINGIFY_(x)
#define RESIDENT_DE_ABI_VERSION_STR   RESIDENT_DE_ABI_STRINGIFY(RESIDENT_DE_ABI_VERSION_N)

// The record versions RESIDENT_DE_ABI_VERSION was last reconciled against.
#define RESIDENT_DE_ABI_MANIFEST_VERSION_EXPECTED UINT16_C(1)
#define RESIDENT_DE_ABI_PIPE_VERSION_EXPECTED     UINT16_C(1)

_Static_assert(RESIDENT_MANIFEST_FORMAT_VERSION ==
               RESIDENT_DE_ABI_MANIFEST_VERSION_EXPECTED,
               "manifest format changed: bump RESIDENT_DE_ABI_VERSION and "
               "RESIDENT_DE_ABI_MANIFEST_VERSION_EXPECTED together");

_Static_assert(RESIDENT_BOOT_PIPE_VERSION ==
               RESIDENT_DE_ABI_PIPE_VERSION_EXPECTED,
               "cross-reset container layout changed: bump RESIDENT_DE_ABI_VERSION "
               "and RESIDENT_DE_ABI_PIPE_VERSION_EXPECTED together");

#endif
