/*
** p33AK128MC106_noinit_ram_reserve.ld -- reserve the hal_noinit_ram block in an AK128
** image. Sibling of p33AK512MPS512_noinit_ram_reserve.ld; read that file first, it carries
** the reasoning for the whole mechanism.
**
** WHY A SEPARATE FILE INSTEAD OF AN #if
** ------------------------------------
** The address differs between the two devices, and nothing that could select it is
** visible while a linker script is preprocessed: neither the device macro
** (__dsPIC33AK128MC106__) nor the configuration macro the link command already carries
** (-DXPRJ_<conf>), and -I does not reach that pass either. Both were measured
** 2026-08-12. One file per device is therefore the mechanism, not a preference.
**
** WHY 0x7F30 IS THE SAME DECISION AS THE AK512's 0x13F30
** -----------------------------------------------------
** Both are "top of data RAM minus 0xD0", and on both devices that range is Y memory,
** which is what NORA_NOINIT_RAM_SPACE says:
**
**   AK512MPS512   data 0x4000..0x13FFF   Y 0xC000..0x13FFF   block 0x13F30..0x13FFF
**   AK128MC106    data 0x4000..0x7FFF    Y 0x6000..0x7FFF    block 0x7F30..0x7FFF
**
** The top of RAM is chosen for the same reason on both: the automatic stack grows up to
** whatever is left, so a range held back at the very top costs the stack its last 208
** bytes and nothing else has to move.
**
** THE ADDRESS IS NOT CHOSEN HERE -- shared/noinit_ram_config.h chooses it per device, and
** buildtools/build.ps1 asserts the linked result against the map.
**
** The two "memory region `data'" link warnings this produces are expected, and the "> data"
** that causes them must stay -- it is what makes ld hold the stack below the block. See the
** AK512 sibling script, where that was measured.
*/

SECTIONS
{
  .noinit_ram 0x7F30 (NOLOAD) :
  {
    KEEP (*(.noinit_ram));
  } > data
}
