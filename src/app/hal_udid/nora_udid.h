#ifndef NORA_UDID_H
#define NORA_UDID_H

//===========================================================
// nora_udid.h -- read the dsPIC33A-family Unique Device Identifier (UDID).
//
// The UDID is a 128-bit, factory-programmed, read-only value that is UNIQUE per
// physical die. It is NOT the device type (DEVID, same for every part of a given
// variant) and NOT the PKOB4 debugger USB serial number -- it identifies the
// individual dsPIC33AK on the board, so it can be used to tell one physical board
// apart from another (board individual ID).
//
// It is four 32-bit read-only words. On dsPIC33A the program/UDID space is in the
// unified (linear) address space, so the words are read with a plain volatile
// pointer -- no PSV / table-read setup (those are for classic dsPIC33C/E/F).
//
//   Name   Address       Meaning
//   UDID1  0x007F2BE0     UDID bits 31:0
//   UDID2  0x007F2BE4     UDID bits 63:32
//   UDID3  0x007F2BE8     UDID bits 95:64
//   UDID4  0x007F2BEC     UDID bits 127:96
//
// Same addresses across the dsPIC33AK128MC106 and dsPIC33AK512MC510 /
// dsPIC33AK512MPS512 families. Reference: the device Programming Specification,
// "Unique Device ID" (Section 1.3).
//===========================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NORA_UDID_WORD_COUNT (4U)

typedef struct
{
    // word[0] = UDID1 (bits 31:0) ... word[3] = UDID4 (bits 127:96).
    uint32_t word[NORA_UDID_WORD_COUNT];
} nora_udid_t;

// Read all four UDID words into *udid. Returns true only when the read succeeds
// AND the value is plausible (see IsPlausible). Returns false (and leaves *udid
// untouched) when udid is NULL; returns false (with *udid populated) when the
// read produced an implausible all-zero / all-one value.
bool nora_udid_read(nora_udid_t *udid);

// True when the UDID is neither all-0x00000000 nor all-0xFFFFFFFF (the two values
// that indicate a failed / unprogrammed read). Pure check; no hardware access.
bool nora_udid_is_plausible(const nora_udid_t *udid);

#ifdef __cplusplus
}
#endif

#endif // NORA_UDID_H
