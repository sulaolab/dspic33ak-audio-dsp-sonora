#ifndef NORA_I2C_CONF_H
#define NORA_I2C_CONF_H

//===========================================================
// board/i2c/nora_i2c_conf.h  (project-supplied HAL config)
//
// Compile-time configuration for the NORA I2C HAL (src/app/hal_i2c). The HAL
// ships no conf.h of its own: it picks this file up if the project puts one on
// the include path, and otherwise falls back to its documented defaults, so a
// project that vendors hal_i2c without this file keeps the previous behaviour.
// Same arrangement as board/audio/nora_spi_i2s_tdm_conf.h and the
// noinit_ram_config.h that hal_noinit_ram looks for.
//===========================================================


//===========================================================
// I2C slave interrupt-vector ownership.
//
//   1 (HAL default) : TURNKEY -- the HAL DEFINES _I2CxInterrupt, _I2CxRXInterrupt
//                     and _I2CxTXInterrupt for every instance this silicon has,
//                     and each routes to the portable slave engine. Link the HAL
//                     and a slave application needs no interrupt code of its own.
//   0               : the HAL defines NO I2C vectors. The slave engine is still
//                     compiled and callable; an integration that wants the slave
//                     role owns the IVT and calls nora_i2c_slave_event_irq(inst)
//                     (plus the _rx_irq / _tx_irq hedge) from its own vectors.
//
// This product is set to 0 because it is I2C MASTER ONLY, and the vectors were
// not merely unused -- they could not fire at all, while keeping 1,196 B of
// program memory and 88 B of RAM linked (2026-08-20 link audit,
// [internal] analysis_ak128_app_link_audit_2026-08-20.md):
//
//   * nora_i2c_slave_init() is reached only from cmsis_driver/Driver_I2C_dsPIC33AK.c,
//     whose object is discarded whole because ENA_CMSIS_I2C is off, so no
//     instance is ever configured as a client;
//   * all three nora_i2c_device_*_irq_enable() are discarded too, and no code
//     writes an I2C IEC bit directly, so I2CxIF is never armed;
//   * the master path (the WM8904 codec) is polled end to end.
//
// Turn this back to 1 -- here, or with -D -- before enabling ENA_CMSIS_I2C in a
// slave role or calling nora_i2c_slave_init() from the application. Nothing
// diagnoses that combination at build time: the slave engine still links, so it
// would configure the peripheral and then never be serviced. Interrupt PRIORITY
// and flag helpers in the device layer are unaffected by this switch; only the
// vector definitions are.
//===========================================================
#ifndef NORA_I2C_DEFINE_SLAVE_VECTORS
#define NORA_I2C_DEFINE_SLAVE_VECTORS   0
#endif


//===========================================================
// How many I2C instances this product's per-instance state covers.
//
// The public enum (NORA_I2C_INST_1..4, NORA_I2C_INST_COUNT) and the whole API are
// unaffected; only the SIZE of the driver's per-instance arrays narrows, and
// nora_i2c_get_device() reports the instances above this count as absent so a
// narrowed instance fails with NOT_PRESENT instead of indexing past an array.
//
// This board reaches I2C1..I2C3 and never I2C4 (board/devices/app_i2c.c:
// AK512 initialises INST_2 = MikroBUS-A and INST_3 = MikroBUS-B; the AK128 J3
// variant initialises INST_2 and INST_1 = DIM-P4/P6; board/devices/wm8904.c maps
// codecs to the same three). 3 therefore covers every configuration in this repo,
// and drops 25 B of data memory (the six master arrays plus g_role were 100 B at
// four instances) -- 2026-08-22 AK512 ASRC RAM work,
// [internal] report_ak512_asrc_ram_gate_2026-08-22.md.
//
// Raise this to 4 (here or with -D) before using NORA_I2C_INST_4 -- including
// through the CMSIS-Driver wrapper, where Driver_I2C3 maps to it
// (cmsis_driver/Driver_I2C_dsPIC33AK.c). Nothing diagnoses that at build time:
// Driver_I2C3 would simply fail to initialise.
//===========================================================
#ifndef NORA_I2C_INST_SUPPORTED_COUNT
#define NORA_I2C_INST_SUPPORTED_COUNT   3
#endif

#if ((NORA_I2C_DEFINE_SLAVE_VECTORS != 0) && (NORA_I2C_DEFINE_SLAVE_VECTORS != 1))
#error "NORA_I2C_DEFINE_SLAVE_VECTORS must be 0 or 1."
#endif

#endif /* NORA_I2C_CONF_H */
