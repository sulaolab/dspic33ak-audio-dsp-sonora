# Classic audio DSP modules

This directory contains the sample-domain processing, analysis and generation
modules owned by the Classic Audio Demo application. ASRC must not depend on
headers or symbols from this directory.

It does not own:

- application lifecycle or build variations;
- codec, TDM, DMA or SAI transport;
- Sonora pin, clock or device configuration;
- ASRC FIFO, servo, measurement or interpolation policy.

Cross-application sample conversion and level-meter support lives in
`src/apps/shared/`. The current physical location of these Classic modules is
transitional; ownership is enforced by the separation ratchet and MPLAB source
manifest.
