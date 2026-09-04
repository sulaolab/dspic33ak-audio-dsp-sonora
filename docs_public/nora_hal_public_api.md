# NORA-HAL public API naming

NORA-HAL means **Native On-chip Resource Assistant**.  It is the public HAL
brand for the on-chip resource layer used by dsPIC33AK (**dsPIC33A** family)
and dsPIC33CK (**dsPIC33C** family) projects.  The brand spans the two families;
it is not a synonym for either one, and NORA is not "the dsPIC33A HAL".  It is
also not a claim that the API is portable to arbitrary processor families: RP
numbers, dsPIC ports, packed-pin representations, and selected clock/peripheral
features remain dsPIC concepts.

## Public namespace

Every public HAL header and every application-visible identifier in `src/app/hal_*`
uses the NORA namespace:

- Header files: `nora_<peripheral>.h`, for example `nora_gpio.h`,
  `nora_pps.h`, and `nora_spi_i2s_tdm.h`.
- Functions, types, variables, and callback types: lower-case `nora_`, for
  example `nora_gpio_config()`, `nora_gpio_pin_t`, and
  `nora_spi_i2s_tdm_start_all_domains()`.
- Macros, enum constants, include guards, and compile-time configuration
  identifiers: upper-case `NORA_`, for example `NORA_GPIO_OUTPUT`,
  `NORA_SPI_I2S_TDM_CLOCK_MASTER`, and `NORA_NOINIT_RAM_SIZE`.

An exported function must use lower-case `nora_` even if the preceding API
used an upper-case function-like spelling.  `NORA_*` is reserved for
compile-time identifiers, never for a callable function.

## Target implementation boundary

Target-specific implementation files may retain an explicit silicon suffix,
such as `nora_gpio_dspic33ak.c` or an existing `dspic33ak_gpio.c`.  That suffix
is an implementation detail; it must not leak into a public header name, a
public type, a public function, or a public macro.  Device register definitions
may also remain implementation-private.

The backend tag is **`_dspic33ak`**, naming the part series the backend is
actually written and validated against.  It is deliberately not `_dspic33a`:
nothing validates a family-wide dsPIC33A backend, and the code itself is
per-part where it has to be (`NORA_SPI_I2S_TDM_DSPIC33AK_DEV_AK512` /
`..._DEV_AK128`).  There is likewise no `_dspic33c` backend to pair with it —
the CK project keeps its own `dspic33ck_*` implementation naming. Since
the tag never appears in a public name, adding a further backend later costs
nothing on the public surface.

AK/CK differences are expressed through the common NORA contract and explicit
capability/result handling.  A capability that only one target can implement
must report that fact; it must not be silently ignored.  Rich target-native
transport paths are either made private behind a NORA API or retained as
explicitly non-portable integration code outside the NORA public boundary.

## ISR fast paths: `<portable name>_hot`

Some calls need an inline, register-direct form in an ISR while keeping an
out-of-line portable form for foreground code.  There is one shape for that:

- The inline is `static inline`, lives in `<module>_<backend>_fast.h`, and is named
  **`<the portable function it shadows>_hot`** — `nora_dma_read_src()` /
  `nora_dma_read_src_hot()`, `nora_ccp_icap_read()` / `nora_ccp_icap_read_hot()`.
- The out-of-line portable version in the backend `.c` is a call to the inline, so
  the two cannot drift.
- The `_hot` suffix goes on the **portable stem**, never a `<backend>` tag in the
  middle.  An ISR body written against `_hot` names ports AK↔CK unchanged; only the
  `*_fast.h` that supplies the inline differs.  A name like
  `nora_dma_dspic33ak_read_src()` forces the ISR body itself to name the chip, which
  turns every ISR into a port site — that was the state before this rule.
- Backend-private helpers with **no** portable twin keep
  `<module>_<backend>_<name>` (e.g. `nora_ccp_dspic33ak_hot_regs()`,
  `nora_spi_i2s_tdm_dspic33ak_sumprof_*`).  They are not a variant of anything, so
  the chip belongs in their name.
- An application file may include a `*_fast.h` **only** to write its own ISR.  That
  is the one sanctioned application-layer dependency on a backend header; reaching
  one to get at a register is not.

The rule is stated in full, with the reasoning, at the top of
`src/app/hal_dma/nora_dma_dspic33ak_fast.h`; the other fast headers point at it.

Inlining is a claim about object code, so verify it that way: the `_hot` symbol
must not appear as its own `.text.*` section in the map, and the ISR that uses it
must contain no `call`/`rcall`.

## Migration rule

The public API uses the `nora_` / `NORA_` namespaces. Compatibility aliases for
the older processor-prefixed namespaces are not added by default, because they
would keep deprecated names public. A downstream product that needs a transition
alias requires an explicit, time-bounded compatibility decision.

## Canonical transport contract for NORA SPI/I2S/TDM

**Scope: this section is about the SPI/I2S/TDM transport only.** It does not
restrict adapters to other standards; the CMSIS-Driver / `Driver_SAI` binding
over this transport remains supported.

`nora_spi_i2s_tdm.h` defines the NORA public transport contract. A second,
reduced portability facade above it is not used.

- The contract includes the native SYSTEM/domain model, per-leg resource model,
  and transport diagnostics.
- Silicon-specific data representation and capability differences are expressed
  as a capability the caller can query or an explicit unsupported result. They
  must not be hidden by silently narrowing or emulating the API.
- Do not introduce a second, reduced stream facade above this transport. If a
  target cannot support an operation, expose that through the contract's
  capability and unsupported-result handling.
