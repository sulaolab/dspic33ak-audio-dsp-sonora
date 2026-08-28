![Microchip logo](https://raw.githubusercontent.com/wiki/Microchip-MPLAB-Harmony/Microchip-MPLAB-Harmony.github.io/images/microchip_logo.png)

# dspic33-cmsis-dsp Library File Generation (Q31)

This folder contains scripts to build source files into a `libcmsis-dsp-dspic33-elf-q31.a` pre-compiled library file for the **dspic33-cmsis-dsp** and also provides an already pre-compiled library file (`libcmsis-dsp-dspic33-elf-q31.a`).  
CMake is used to build the source into a library for use within **MPLAB X** projects.

---

## Building the dspic33-cmsis-dsp Library

To build the library, the following must be installed:

- make  
- CMake (version 3.30 or later)
- Python (version 3.10 or later)  
- Microchip XC-DSC (version 3.31.00 or later)

### Installing for Builds in Windows

The packages can be found at these links:

- [make (through Cygwin installer)](https://www.cygwin.com/install.html)  
- [CMake](https://github.com/Kitware/CMake/releases)  
- [Python 3.10 or later](https://www.python.org)  
- [Microchip XC-DSC](https://www.microchip.com/en-us/tools-resources/develop/mplab-xc-compilers/xc-dsc#downloads)

### Installing for Builds in Linux

For Linux, `make`, `CMake`, and `Python` are typically installed using the system package manager.

- [Microchip XC-DSC](https://www.microchip.com/en-us/tools-resources/develop/mplab-xc-compilers/xc-dsc#downloads)

---

## Environment Verification

Before building, confirm the following commands work:
    python --version
    cmake --version
    make --version

For Linux, ensure the XC-DSC binaries are executable.

---

## Executing the Build

Once the prerequisites are installed, open a Command Prompt or Unix terminal and navigate to the `Library/q31/Scripts` directory.  
Then launch the `buildLibrary_q31.py` script with the desired arguments:

    usage: buildLibrary_q31.py [-h] [-j JSONFILE] [-r] [-l LIBRARY]
                           [-d DEVICE] [-c COMPILER]

    options:
      -h, --help            show this help message and exit
      -j JSONFILE, --jsonfile JSONFILE
                            JSON file with build configuration
      -r, --rebuild         run a clean build (remove artifacts)
      -l LIBRARY, --library LIBRARY
                            library file name
      -d DEVICE, --device DEVICE
                            MCPU device flag
      -c COMPILER, --compiler COMPILER
                            path to compiler

A JSON file that provides the `library`, `device`, and `compiler` parameters can be found in the `./json` directory.  
Update this file as needed.

---

## Example Parameters

### JSON Input
    python buildLibrary_q31.py -j json\libcmsis-dsp-dspic33-elf-q31.json

### Parameterized Input (Defaults, Build)
    python buildLibrary_q31.py

### Parameterized Input (Defaults, Rebuild)
    python buildLibrary_q31.py -r

### Parameterized Input (All)
    python buildLibrary_q31.py -l cmsis-dsp-dspic33-elf-q31 -d GENERIC-32DSP-AK -c "C:\Program Files\Microchip\xc-dsc\v3.31" -r

> **Note:**  
> The `cmake/build_q31` directory is automatically created during the build process.

---

## Output Artifacts

After a successful build, the following artifacts are generated:

- **Static library**
    Library/q31/libcmsis-dsp-dspic33-elf-q31.a

- **Build intermediate files**
    cmake/build_q31/*

> Libraries are compiler-, device-, and architecture-specific.  
> Always rebuild the library if the XC-DSC version or target device changes.

---

## JSON Configuration File Format

### Required Fields
    {
    "library": "cmsis-dsp-dspic33-elf-q31",
    "device": "GENERIC-32DSP-AK",
    "toolchain": "xc-dsc_toolchain.cmake",
    "compiler": "C:/Program Files/Microchip/xc-dsc/v3.31"
    }

### Notes

- All fields are required when using `--jsonfile`
- Paths must be absolute
- On Windows, paths containing spaces must be enclosed in quotes

---

## Troubleshooting

### Compiler Not Found
    error: Compiler path '...' not found.

- Confirm XC-DSC is installed
- Verify the path matches the installed version
- Use quotes on Windows paths

---

### make Not Found (Windows)

Ensure Cygwin is installed and `make.exe` is available in `PATH`.

---

## Python Version Support

- Python **3.10 or later** is required
- Earlier versions are not tested

---
