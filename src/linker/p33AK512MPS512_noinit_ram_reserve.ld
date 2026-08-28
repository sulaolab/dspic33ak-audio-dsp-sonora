/*
** p33AK512MPS512_noinit_ram_reserve.ld -- reserve the hal_noinit_ram block in a
** NON-RESIDENT (no serial-update) AK512 image.
**
** WHY THIS FILE IS TINY, AND WHY IT IS NOT A COPY OF THE DEVICE SCRIPT
** -------------------------------------------------------------------
** A serial-update image already gets its reservation from this repo's full linker script,
** linker/p33AK512MPS512_serial_update_app.gld, because that image needs a custom program
** layout anyway (the resident bootloader owns the low flash). A non-resident image has no
** reason to replace the device default script -- it needs the default layout plus ONE data
** range held back. Copying the DFP script to add ten lines would mean carrying 6,626
** generated lines in this repo and re-syncing them on every DFP update.
**
** So this is a SUPPLEMENTARY script, passed in addition to the device default:
**
**   linker extra options:  -Wl,-T../src/linker/p33AK512MPS512_noinit_ram_reserve.ld
**
** GNU ld (2.32 here) merges the SECTIONS blocks of every script it is given, and the
** device default arrives via --script= from the MPLAB/DFP machinery, so the reservation
** below is simply appended to it. Measured 2026-08-12 on this project: the block appears
** at 0x13F30 size 0xD0, and the automatic stack ends at 0x13F2F -- i.e. ld accounts for
** this section when it places the stack, which is exactly the property the reservation is
** for. (INSERT AFTER and ld's own INCLUDE were both tried first: INSERT reports
** ".text not found" because supplementary scripts are resolved before the default one is
** parsed, and INCLUDE chokes on the '#if' in the DFP script, which the compiler driver
** preprocesses. No macros or -I reach that preprocessing pass either, which is why there
** is one of these files PER DEVICE rather than one with an #if.)
**
** THE ADDRESS IS NOT CHOSEN HERE. shared/noinit_ram_config.h chooses it, per device, and
** buildtools/build.ps1 asserts the linked result against the map after every link. If you
** change one, change both -- the assertion is what will tell you that you didn't.
**
** TWO LINK WARNINGS ARE EXPECTED, AND THE "> data" THAT CAUSES THEM IS LOAD-BEARING
** ---------------------------------------------------------------------------------
** Every non-delivery link prints:
**
**   warning: memory region `data' not declared
**   warning: redeclaration of memory region `data'
**
** -- because this supplementary script is parsed BEFORE the device default that declares
** the region. Tempting to silence by dropping the "> data". Measured 2026-08-12: dropping
** it puts the section at the right address and lets the automatic stack grow straight
** through it (stack length 0x7E38 -> 0x7F08, i.e. ending at 0x14000 instead of 0x13F30).
** Assert-StandaloneMapLayout caught it, which is the only reason it is a note here rather
** than RAM corruption on a board. The region assignment is what makes ld account for the
** block when it sizes the stack; the warnings are the price and they are worth paying.
*/

SECTIONS
{
  /*
  ** KEEP because --gc-sections would otherwise discard a block that nothing relocates
  ** against; (NOLOAD) keeps it out of .dinit, which is what makes the contents survive a
  ** warm reset instead of being re-initialized by crt0.
  */
  .noinit_ram 0x13F30 (NOLOAD) :
  {
    KEEP (*(.noinit_ram));
  } > data
}
