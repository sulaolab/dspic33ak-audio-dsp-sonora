# =====================================================================
# boot_image.psd1 -- what the resident boot image is built from. ONE authority,
# read by both consumers:
#
#   buildtools/build_resident_bootloader.ps1   the command-line build, which is
#                                              the authority on the delivered image
#   buildtools/generate_resident_project.ps1   generates resident_bootloader.X for
#                                              the IDE, whose only job is DEBUGGING
#
# WHY A DATA FILE AND NOT JUST THE BUILD SCRIPT
#
# Before this file the lists below lived in the build script, and the MPLAB X
# project did not exist. Adding a project by hand would have created a second
# list of 20 sources and 8 include directories, maintained by memory, in a file
# MPLAB X rewrites on its own whenever the IDE touches it. The two would have
# diverged, and the divergence would not have looked like a failure: both images
# link, both run, and the one you debugged is not quite the one you ship.
#
# So the project is GENERATED from this file and a gate regenerates it into a
# temporary directory and fails on any difference. Edit this file; never edit
# resident_bootloader.X/nbproject/configurations.xml. If MPLAB X rewrites it, the
# gate says so and the fix is to regenerate.
#
# WHY THIS FILE LIVES IN src/boot/ AND NOT buildtools/
#
# It is a statement about the image, not about the tooling -- the same reason the
# sources it names are in src/boot/. A reader who opens src/boot/ sees what the image is
# made of without being told which script to read first. Both consumers are in
# buildtools/ and both find it by relative path.
#
# THE SIZE CAP IS THE REASON ANY OF THIS IS CAREFUL (32 KiB on the MPS512, 16 KiB on
# the MC106 -- see Devices below for why they differ). The image is pinned at
# fixed addresses below the application; it cannot grow into it, and it cannot be
# allowed to move because something in src/app/ moved. See SizeCapBytes below and
# buildtools/verify_resident_image.ps1, which asserts the linked result.
# =====================================================================
@{
    # The MPLAB X project directory, without the .X. Named here rather than in the
    # generator because two other places need to KNOW it without generating anything:
    # buildtools/sonora_build_state.ps1 has to pick the application project out of a
    # repository that now holds two, and check_resident_project.ps1 has to find the
    # project it checks.
    #
    # ONE project with one configuration PER DEVICE -- not one project per device. A
    # second .X directory would have made Resolve-SonoraProjectDir ambiguous (it finds
    # the application project by excluding this one by name) and would have put a
    # second copy of the same 20 itemPaths in the tree. The application project
    # already expresses "same sources, several devices" as several configurations;
    # this one matches it, so the dropdown gesture is the same in both.
    ProjectName = 'resident_bootloader'

    # ---------------------------------------------------------------------------
    # PER DEVICE -- and nothing that is not per device.
    #
    # Everything else in this file is a statement about the IMAGE and is shared: the
    # 20 sources, the 8 include directories, the macros, the compiler flags and the
    # linker options are the same on every part. What differs is which part it is,
    # what the configuration is called, which device linker script the link uses,
    # where in Flash the image goes and how large it may get.
    #
    # Splitting this into one manifest per device would have duplicated the source
    # list, and one of the two copies would have rotted -- exactly the failure this
    # file exists to prevent (see the header). Two devices, one list.
    #
    # ConfigurationName is also the build/ and dist/ subdirectory name, so each
    # device's image lands in its own place, and the command-line build and an IDE
    # build of the same configuration cannot produce two images that differ while
    # both claiming to be "the bootloader".
    #
    # LinkerScript is the APPLICATION's serial-update script for that device, reused:
    # it is the file that knows where the boot region ends and the application
    # begins, and having one script define that boundary for both images is what
    # stops the two from disagreeing. Defsym narrows it to the boot region.
    #
    # Defsym IS PER DEVICE, AND THE TWO NO LONGER AGREE. It was written out for both
    # while the values coincided, on the argument that the low 32 KiB of the panel was
    # each device's own arrangement rather than a family constant -- and on 2026-08-15
    # that turned out to be literal: the MC106's application needed one more erase page
    # than 32 KiB left it, so its boot region became 28 KiB, and on 2026-08-20 the ROM
    # diet took it to 16 KiB. Had the pair been hoisted into the shared section as "the
    # same", each of those would have been a change to both parts.
    # 0x800004 rather than 0x800000 because the reset vector at 0x800000 is not part of
    # the region the script places code into; each LENGTH is its cap minus that 4.
    #
    # SizeCapBytes is the hard cap, asserted on the LINKED image by
    # buildtools/verify_resident_image.ps1 -- not merely documented here. The
    # application's own linker script starts where this ends, so an image that
    # exceeded the cap would not fail to link, it would overlap the application.
    #
    # DfpPack is which Microchip device family pack supports the part. The two parts
    # are in DIFFERENT packs -- MP for the MPS512, MC for the MC106 -- which the build
    # script hard-coded to MP while only one device existed. That would not have failed
    # to build: it would have compiled the AK128 image against a pack that does not
    # contain p33AK128MC106.h, which is a header error at best and the wrong device
    # header at worst. The build script resolves THIS pack and asserts the part's own
    # header is in it.
    #
    # DfpPackVersion pins WHICH version of that pack, and it is not a preference --
    # newest-installed is actively wrong on both parts, for two unrelated reasons:
    #
    #   MC 1.5.263 and later removed PLL1CON.OE / PLL2CON.OE, so
    #     src/boot/hal_clock/nora_clock_dspic33ak_reg.c stops compiling
    #     ("has no member named 'OE'").
    #   MP 1.5.269 no longer accepts NOBTSWP = OFF, so the #pragma config in
    #     src/boot/resident_de_boot_main.c is rejected ("unknown value").
    #
    # Both failures read as defects in this repository and are not, and both appear
    # only in a fresh clone -- a clone with an existing dist/ never rebuilds the boot
    # image and so never sees them. Pinning here rather than in the build script
    # keeps it with the other per-device facts, and next to the reason.
    #
    # These are floors, not ceilings: raise a pin when a newer pack is verified to
    # build the image, and say what was verified.
    Devices = @{
        '33AK512MPS512' = @{
            ConfigurationName = 'dsPIC33AK512_RESIDENT_BOOT'
            LinkerScript      = 'src/linker/p33AK512MPS512_serial_update_app.gld'
            Defsym = @{
                '__SONORA_PROGRAM_ORIGIN' = '0x800004'
                '__SONORA_PROGRAM_LENGTH' = '0x7FFC'
            }
            SizeCapBytes = 0x8000
            DfpPack      = 'dsPIC33AK-MP_DFP'
            # 1.5.269 rejects NOBTSWP = OFF; 1.3.185 builds and has p33AK512MPS512.h.
            DfpPackVersion = '1.3.185'
        }
        '33AK128MC106' = @{
            ConfigurationName = 'dsPIC33AK128_RESIDENT_BOOT'
            LinkerScript      = 'src/linker/p33AK128MC106_serial_update_app.gld'
            # 16 KiB, not 32, and not the 28 KiB this was until 2026-08-20: the ROM diet
            # took this part's image to 15,156 bytes, so four more erase pages went to the
            # application. The reasoning and the measurements are in
            # src/shared/resident_de_manifest.h, whose RESIDENT_BOOT_SIZE_BYTES is this cap
            # and whose RESIDENT_APP_BASE_ADDRESS is where this region ends.
            # 0x3FFC = 0x4000 - 4 for the reset vector.
            #
            # The cap is the REGION, deliberately, not the release criterion. The image is
            # required to link at or under 0x3C00 (15,360 B) so 1 KiB of the region stays
            # spare; that criterion is a review judgement rather than a build failure. What
            # must never be silent is overrunning the region itself, and that is what this
            # number catches.
            Defsym = @{
                '__SONORA_PROGRAM_ORIGIN' = '0x800004'
                '__SONORA_PROGRAM_LENGTH' = '0x3FFC'
            }
            SizeCapBytes = 0x4000
            DfpPack      = 'dsPIC33AK-MC_DFP'
            # 1.5.263+ removed PLL1CON.OE; 1.4.172 is the newest that still builds.
            DfpPackVersion = '1.4.172'
        }
    }

    # Which device a bare `build_resident_bootloader.ps1` builds, and which
    # configuration the generated project lists first. Stated rather than derived from
    # the order of the hashtable above: Import-PowerShellDataFile returns an unordered
    # Hashtable, so "the first one" is not a thing this file can express.
    DefaultDevice = '33AK512MPS512'

    # 20 translation units. Grouped by WHO OWNS THE COPY, because that is the
    # distinction the reorganisation exists to make legible -- not by subsystem.
    Sources = @(
        # src/boot/ -- this image only, and deliberately absent from
        # dspic33ak_audio_dsp.X: the application must never link engine internals.
        'src/boot/resident_de_boot_main.c'
        'src/boot/resident_de_boot_precrt.S'
        'src/boot/resident_de_bootloader.c'
        'src/boot/resident_de_boot_platform.c'
        'src/boot/resident_de_boot_xmodem.c'
        'src/boot/resident_de_boot_crc32.c'
        'src/boot/resident_de_boot_led.c'

        # src/shared/ -- compiled into BOTH images, by necessity. For an ABI, two
        # copies that drift no longer agree on where the mailbox is, and nothing
        # reports it. See src/shared/resident_de_abi.h.
        'src/shared/resident_de_mailbox.c'
        'src/shared/resident_de_pipe.c'

        # src/boot/hal_*/ -- this image's OWN copy of the HAL, vendored from src/app/hal_*/
        # at reorg step 4. An application-side fix does NOT arrive here; it is
        # carried over on purpose, and buildtools/check_hal_drift.ps1 reports where
        # the two copies differ rather than forbidding the difference.
        #
        # The src/boot/hal_*/ directories were vendored WHOLE, so they contain 3 .c
        # files this list deliberately does not name (nora_gpio_event,
        # nora_gpio_table, nora_high_res_timer). They are present so that a header
        # a boot source starts needing tomorrow is already there; compiling them
        # would cost ROM for nothing. THIS LIST, not the directory contents, says
        # what the image is.
        'src/boot/hal_noinit_ram/nora_noinit_ram_dspic33ak.c'
        'src/boot/hal_nvm/nora_nvm_dspic33ak.c'
        'src/boot/hal_clock/nora_clock_dspic33ak.c'
        'src/boot/hal_clock/nora_clock_device_dspic33ak.c'
        'src/boot/hal_clock/nora_clock_dspic33ak_reg.c'
        'src/boot/hal_gpio/nora_gpio_dspic33ak.c'
        'src/boot/hal_gpio/nora_pps_dspic33ak.c'
        'src/boot/hal_uart/nora_uart_dspic33ak.c'
        'src/boot/hal_uart/nora_uart_dspic33ak_device.c'
        'src/boot/hal_uart/nora_uart_dspic33ak_rx_isr_ring.c'
        'src/boot/hal_timer/nora_tick_timer_dspic33ak.c'
    )

    # EIGHT include directories, AND NEVER A NINTH THAT NAMES src/app/.
    #
    # That is the bulkhead (feasibility 8.1 rule 1), and since reorg step 4 it holds
    # by construction rather than by convention: a boot source that reaches for an
    # application header does not compile. Adding '-Isrc/app' here -- or letting MPLAB X
    # add ..\src\app to the generated project's include list -- would undo the whole
    # step silently, because the build would start working again. The right move
    # when a boot source needs an app header is to vendor the file into src/boot/.
    #
    # buildtools/check_resident_project.ps1 asserts the generated project's include
    # list against exactly this one, for that reason.
    Includes = @(
        'src/boot'
        'src/shared'
        'src/boot/hal_noinit_ram'
        'src/boot/hal_nvm'
        'src/boot/hal_clock'
        'src/boot/hal_gpio'
        'src/boot/hal_uart'
        'src/boot/hal_timer'
    )

    # Preprocessor symbols every translation unit gets. SONORA_BOOT_GIT_COMMIT is
    # NOT here: it is computed per build from the working tree, so it is the one
    # macro that cannot come from a checked-in file.
    Macros = @(
        'SONORA_RESIDENT_BOOTLOADER=1'
    )

    # -Os, for MARGIN in both resident images. This flag used to be -O2, with a
    # comment saying -Os had been measured larger here; that did not reproduce on
    # XC-DSC 3.31.01 / DFP 1.4.172 (2026-08-19), where -Os is smaller on both
    # devices:
    #
    #   AK128  17,828 -> 16,228 B  (62% -> 56% of its 28 KiB region)
    #   AK512  29,116 -> 25,312 B  (88.9% -> 77.2% of its 32 KiB region)
    #
    # Those percentages are the ones measured on 2026-08-19 and are kept as the record
    # of why the flag was taken. They are NOT today's occupancy: four more checkpoints
    # followed, and the AK128 region became 16 KiB on 2026-08-20, so that image is now
    # 15,156 B at 92.5%.
    #
    # AK512 is why this is worth doing independently of any 16 KiB AK128 work:
    # at 88.9% it had 3,668 B of headroom, and now has 7,456 B.
    #
    # This list is shared by BOTH configurations -- the per-device table above
    # carries LinkerScript/Defsym/SizeCapBytes/DfpPack but not flags -- so the
    # choice is deliberately a both-devices one, not an AK128 tweak.
    #
    # -Os changes codegen in the program-Flash paths, which on this core is not a
    # free choice: nora_nvm_verify() reads Flash through a plain volatile
    # pointer with no asm guard, and read_flash_byte() exists because this
    # compiler was measured folding a Flash copy loop into a trapping indexed
    # EA. That review was done before this flag changed, function by function
    # over every two-register indexed load in both images. Result: -Os is safer
    # here, because the one instance of the trapping shape (a base register
    # holding a pointer DIFFERENCE rather than an address) was in the -O2 build's
    # nora_nvm_verify, and -Os bases that load on the real Flash address. The asm
    # guard stays single-register under -Os. Full analysis, including why
    # "indexed" is not by itself the hazard:
    #   [internal] report_resident_bootloader_16k_feasibility_2026-08-19.md
    #
    # BOARD-VALIDATED on both devices: AK512 board-gate run 1 on this -Os image, and
    # AK128 runs 2, 3 and 4, every gate in scope PASS. Run 2's readback verification
    # is what settles nora_nvm_verify() on hardware rather than by review.
    #
    # -ffunction-sections/-fdata-sections are the precondition for --gc-sections;
    # without them the linker has nothing to discard, so the three flags travel
    # together. -msfr-warn=off: the HAL writes SFRs the compiler warns about by
    # default, and the warning is not actionable.
    CompilerFlags = @(
        '-Os'
        '-Wall'
        '-msfr-warn=off'
        '-ffunction-sections'
        '-fdata-sections'
    )

    # The linker script is per device -- see Devices above.

    LinkerOptions = @(
        '--stack=1024'
        '--check-sections'
        '--data-init'
        '--pack-data'
        '--handles'
        '--gc-sections'
        '--stackguard=64'
        '--ivt'
        '--isr'
        '--no-force-link'
        '--smart-io'
        '--report-mem'
        '--cref'
        '--warn-section-align'
    )

    # Where this image lives (Defsym) and how big it may get (SizeCapBytes) are per
    # device -- see Devices above.
}
