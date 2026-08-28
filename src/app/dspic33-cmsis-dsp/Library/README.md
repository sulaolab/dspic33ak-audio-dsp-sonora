![Microchip logo](https://raw.githubusercontent.com/wiki/Microchip-MPLAB-Harmony/Microchip-MPLAB-Harmony.github.io/images/microchip_logo.png)

# dspic33-cmsis-dsp Library

This section explains how to integrate the **precompiled `dspic33-cmsis-dsp` static library**
(`.a` file) into an **MPLAB X IDE** project using the **Microchip XC-DSC compiler**.

---

## Library Variants

Two pre-compiled libraries are provided, one for f32 functions and one for q31 functions.

---

### f32 — Floating-Point

Contains the scripts to build **floating-point (float32)** based library file for dsPIC33 devices.

**Includes:**
- Precompiled static library for f32 DSP functions
- CMake build configuration
- Python build scripts to regenerate the library
- Device and compiler configuration support for XC-DSC

**Typical use cases:**
- Applications requiring higher numerical precision
- Control algorithms and signal processing using floating-point math

See:
![f32 - Library File Generation](f32/README.md)
for build instructions and tool setup.

---

### q31 — Fixed-Point

Contains the scripts to build **fixed-point q31** based library file optimized for dsPIC33 devices.

**Includes:**
- Precompiled static library for q31 DSP functions
- CMake build configuration
- Python build scripts to regenerate the library
- Device and compiler configuration support for XC-DSC

**Typical use cases:**
- Performance- or memory-constrained applications
- Deterministic DSP workloads using fixed-point arithmetic

See:
![q31 - Library File Generation](q31/README.md)
for build instructions and tool setup.

---

Choose **f32** or **q31** based on your application’s precision, performance, and memory requirements.