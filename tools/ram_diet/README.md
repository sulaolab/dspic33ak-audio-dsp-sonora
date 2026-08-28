# tools/ram_diet — where the data memory went

`tools/rom_diet/` answers "how many program bytes, and which object owns them".
These two answer the same question for RAM, which the link map states only as a
single total.  Neither script writes anything; both take a build artefact.

## `map_ram_report.py <map> [limit]`

Attributes every data-memory input section in the link map to the object file
that contributed it, then prints the per-object total and the largest individual
objects.  Sections are filtered to `addr >= 0x4000` (the data region's origin —
program memory starts at 0x800000) and `.debug*`/`.comment`/`.dinit` are dropped,
because those live at address 0 in the map and would otherwise dominate.

    python tools/ram_diet/map_ram_report.py \
      dspic33ak_audio_dsp.X/dist/dsPIC33AK128_SERIAL_UPDATE/production/dspic33ak_audio_dsp.X.production.map

The attributed total is a few hundred bytes under the map's own
`Total "data" memory used` — the difference is the libc `.data`/`.bss`
contributions the parser reports under their archive member, not an object.

## `stack_depth.py <disassembly> [root ...]`

The map does not price the stack: the linker hands whatever is left over to it
(`Dynamic Memory Usage / stack`), so a RAM diet needs a separate answer to "how
much of that does the firmware actually need".  This walks the direct call graph
of an `objdump -d` and reports the deepest path from each root, using
4 B per prologue push plus each `add.l w15, #imm, w15` allocation as the frame,
and 4 B per call for the return address.  With no root it prints the ten deepest
functions in the image.

    xc-dsc-objdump -d "-mdfp=$HOME/.mchp_packs/Microchip/dsPIC33AK-MC_DFP/1.4.172/xc16" \
      dspic33ak_audio_dsp.X/dist/.../dspic33ak_audio_dsp.X.production.elf > ak128.dis
    python tools/ram_diet/stack_depth.py ak128.dis _main __DMA0Interrupt

Note the `xc16` subdirectory in `-mdfp`: pointing it at the pack root fails with
`can't disassemble for architecture UNKNOWN!` because `c30_device.info` lives one
level down.

**The result is a LOWER bound.**  Only direct `rcall`/`call` edges are followed,
and this tree dispatches the audio block and the console through function
pointers, so a root like `__DMA0Interrupt` reports its own frame and misses the
DSP chain hanging off the callback.  Price those separately (e.g. give
`_classic_audio_path_process` as a root) and add them up.  A real worst case
still wants a runtime stack-painting check on hardware.
