#!/usr/bin/env python3
"""
Hot-path invariance harness.

Purpose: prove that a refactor which only widens *capability* (more transport
legs, more ASRC engines) costs the ALREADY-SUPPORTED build nothing.  "Nothing"
is not a feeling -- it is:

  1. every watched function's INSTRUCTION SEQUENCE is unchanged,
  2. program and data size are unchanged,
  3. (measured separately, on hardware) the *az kernel bench is unchanged.

Why mnemonic sequences and not raw objdump text: absolute addresses of code and
statics legitimately move when a translation unit grows, and an immediate that
holds a static's address is not a cost.  The real invariant is *which
instructions execute*.  So each function is reduced to its ordered list of
mnemonics; that catches the things a capability refactor could actually cost us
-- an added multiply (runtime leg index into a non-power-of-two struct), a lost
constant fold (loop bound or channel count no longer a literal), an added
branch (runtime kernel dispatch), or a spill.  Operands are reported only for
functions that differ, to make the diff readable.

Usage:
  # after building the reference configuration
  python tools/asrc/hotpath_invariance.py --save baseline.json
  # ... refactor, rebuild ...
  python tools/asrc/hotpath_invariance.py --compare baseline.json

Exit code 0 = invariant held, 1 = a watched function or a size changed.
"""

import argparse
import json
import os
import re
import subprocess
import sys

DEFAULT_ELF = os.path.join(
    "dspic33ak_audio_dsp.X", "dist", "dsPIC33AK512_ASRC", "production",
    "dspic33ak_audio_dsp.X.production.elf")
DEFAULT_MAP = DEFAULT_ELF[:-len(".elf")] + ".map"
DEFAULT_OBJDUMP = r"C:\Program Files\Microchip\xc-dsc\v3.31.01\bin\xc-dsc-objdump.exe"
# Same resolution order as buildtools/provision.ps1: an explicit DSPIC33AK_DFP
# wins, otherwise the per-user pack directory. Never a hard-coded home path --
# this file is published.
DEFAULT_DFP = os.environ.get("DSPIC33AK_DFP") or os.path.join(
    os.path.expanduser("~"), ".mchp_packs", "Microchip",
    "dsPIC33AK-MP_DFP", "1.3.185", "xc16")
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def default_src_dirname(repo_root):
    """Mirror build.ps1's `Split-Path -Leaf $repoRoot` -> -DAPP_SRC_DIRNAME."""
    return os.path.basename(os.path.normpath(repo_root))


def default_git_commit(repo_root):
    """Mirror build.ps1's -DSONORA_GIT_COMMIT derivation (short=7 hash, '_dirty' suffix)."""
    def run(args):
        out = subprocess.run(["git", "-C", repo_root] + args,
                              capture_output=True, text=True)
        return out.returncode, out.stdout.strip()

    rc, commit = run(["rev-parse", "--short=7", "HEAD"])
    if rc != 0 or not commit:
        return "unknown"
    rc, status = run(["status", "--porcelain", "--untracked-files=normal"])
    if rc == 0 and status.strip():
        commit += "_dirty"
    return re.sub(r"[^A-Za-z0-9_]", "_", commit)


# Historic baseline JSON files are immutable records of their original builds.
# Canonicalize renamed symbols only while comparing snapshots so an API namespace
# refactor does not appear as a hot-path disappearance/appearance.
#
# This table is the ONLY place a rename is recorded -- do not edit the checked-in
# baselines to follow a rename. Editing them makes the record disagree with the
# build it came from, and anyone restoring an older baseline from git history then
# gets a wall of phantom "DISAPPEARED" findings for symbols that were only renamed.
#
# Only `functions` / `census` keys are canonicalized. Operand text inside the
# stored disassembly still carries the original names (e.g. a call target printed
# as <_dspic33ak_dma_half_from_status>); compare() keys off the mnemonic list, not
# the operand text, so that costs nothing and keeps the record faithful.
#
# Below is the NORA API rename, 224 symbols
# across ccp / clock / dma / gpio / high_res_timer / i2c / pinmux / pps / reset /
# spi_i2s_tdm / tick_timer / uart. Most are _dspic33ak_x -> _nora_x, but three shapes
# are not, which is why this table is generated from a real build's symbols and
# verified rather than pattern-substituted at compare time:
#   - device-scoped: _nora_<family>_dspic33ak_<rest> (all the uart rx_isr/async/device
#     entry points, nora_clock_dspic33ak_clkgen_configure, the three sumprof calls),
#   - restructured, not renamed: the reset latched-cause accessors moved under
#     nora_reset_snapshot_*.  _dspic33ak_reset_cause_str is the trap -- a
#     nora_reset_cause_str() does exist, but it takes arguments and is a different
#     function; the old zero-arg one is now nora_reset_snapshot_cause_str,
#   - not renamed at all: _dspic33ak_clock_reg_{clkgen,pll}_configure. The reg layer
#     kept the silicon prefix, so those two must have NO entry here.
# An earlier attempt to keep the baselines in step by editing them had frozen at an
# intermediate naming (_pps_route_output) that no build has ever emitted, and it
# covered only 4 of the 12 families -- another reason not to hand-edit the records.
# 115 of these targets are absent from today's dsPIC33AK512_ASRC build because the
# function was inlined or gc-sectioned away. Those entries are still correct as a
# record of the rename, and mapping them is what lets the resulting "DISAPPEARED"
# finding mean "the function is gone", not "the symbol was spelled differently".
LEGACY_SYMBOL_ALIASES = {
    "_dspic33ak_ccp_icap_configure": "_nora_ccp_icap_configure",
    "_dspic33ak_ccp_icap_isr": "_nora_ccp_icap_isr",
    "_dspic33ak_ccp_icap_overflow": "_nora_ccp_icap_overflow",
    "_dspic33ak_ccp_icap_read": "_nora_ccp_icap_read",
    "_dspic33ak_ccp_icap_set_callback": "_nora_ccp_icap_set_callback",
    "_dspic33ak_ccp_icap_start": "_nora_ccp_icap_start",
    "_dspic33ak_ccp_icap_stop": "_nora_ccp_icap_stop",
    "_dspic33ak_ccp_icap_timebase_hz": "_nora_ccp_icap_timebase_hz",
    "_dspic33ak_clock_clkgen_configure": "_nora_clock_dspic33ak_clkgen_configure",
    "_dspic33ak_clock_device_encode_clkgen_source": "_nora_clock_device_encode_clkgen_source",
    "_dspic33ak_clock_device_encode_pll_source": "_nora_clock_device_encode_pll_source",
    "_dspic33ak_clock_pll_configure": "_nora_clock_pll_configure",
    "_dspic33ak_dma_channel_config": "_nora_dma_channel_config",
    "_dspic33ak_dma_channel_enable": "_nora_dma_channel_enable",
    "_dspic33ak_dma_clear_irq_flag": "_nora_dma_clear_irq_flag",
    "_dspic33ak_dma_clear_status": "_nora_dma_clear_status",
    "_dspic33ak_dma_global_init": "_nora_dma_global_init",
    "_dspic33ak_dma_global_is_ready": "_nora_dma_global_is_ready",
    "_dspic33ak_dma_half_from_status": "_nora_dma_half_from_status",
    "_dspic33ak_dma_irq_enable": "_nora_dma_irq_enable",
    "_dspic33ak_dma_irq_is_enabled": "_nora_dma_irq_is_enabled",
    "_dspic33ak_dma_read_status": "_nora_dma_read_status",
    "_dspic33ak_gpio_clear": "_nora_gpio_clear",
    "_dspic33ak_gpio_config": "_nora_gpio_config",
    "_dspic33ak_gpio_config_digital_input": "_nora_gpio_config_digital_input",
    "_dspic33ak_gpio_config_digital_output": "_nora_gpio_config_digital_output",
    "_dspic33ak_gpio_pin_from_rp": "_nora_gpio_pin_from_rp",
    "_dspic33ak_gpio_read": "_nora_gpio_read",
    "_dspic33ak_gpio_read_output": "_nora_gpio_read_output",
    "_dspic33ak_gpio_rp_clear": "_nora_gpio_rp_clear",
    "_dspic33ak_gpio_rp_config": "_nora_gpio_rp_config",
    "_dspic33ak_gpio_rp_config_digital_input": "_nora_gpio_rp_config_digital_input",
    "_dspic33ak_gpio_rp_config_digital_output": "_nora_gpio_rp_config_digital_output",
    "_dspic33ak_gpio_rp_from_pin": "_nora_gpio_rp_from_pin",
    "_dspic33ak_gpio_rp_read": "_nora_gpio_rp_read",
    "_dspic33ak_gpio_rp_read_output": "_nora_gpio_rp_read_output",
    "_dspic33ak_gpio_rp_set": "_nora_gpio_rp_set",
    "_dspic33ak_gpio_rp_set_analog": "_nora_gpio_rp_set_analog",
    "_dspic33ak_gpio_rp_set_direction": "_nora_gpio_rp_set_direction",
    "_dspic33ak_gpio_rp_set_open_drain": "_nora_gpio_rp_set_open_drain",
    "_dspic33ak_gpio_rp_set_pull": "_nora_gpio_rp_set_pull",
    "_dspic33ak_gpio_rp_toggle": "_nora_gpio_rp_toggle",
    "_dspic33ak_gpio_rp_write": "_nora_gpio_rp_write",
    "_dspic33ak_gpio_set": "_nora_gpio_set",
    "_dspic33ak_gpio_set_analog": "_nora_gpio_set_analog",
    "_dspic33ak_gpio_set_direction": "_nora_gpio_set_direction",
    "_dspic33ak_gpio_set_open_drain": "_nora_gpio_set_open_drain",
    "_dspic33ak_gpio_set_pull": "_nora_gpio_set_pull",
    "_dspic33ak_gpio_toggle": "_nora_gpio_toggle",
    "_dspic33ak_gpio_write": "_nora_gpio_write",
    "_dspic33ak_high_res_timer_count_to_us": "_nora_high_res_timer_count_to_us",
    "_dspic33ak_high_res_timer_count_to_us_x10": "_nora_high_res_timer_count_to_us_x10",
    "_dspic33ak_high_res_timer_deinit": "_nora_high_res_timer_deinit",
    "_dspic33ak_high_res_timer_elapsed_count": "_nora_high_res_timer_elapsed_count",
    "_dspic33ak_high_res_timer_elapsed_us": "_nora_high_res_timer_elapsed_us",
    "_dspic33ak_high_res_timer_elapsed_us_x10": "_nora_high_res_timer_elapsed_us_x10",
    "_dspic33ak_high_res_timer_get_count": "_nora_high_res_timer_get_count",
    "_dspic33ak_high_res_timer_init": "_nora_high_res_timer_init",
    "_dspic33ak_high_res_timer_is_initialized": "_nora_high_res_timer_is_initialized",
    "_dspic33ak_high_res_timer_is_present": "_nora_high_res_timer_is_present",
    "_dspic33ak_i2c_calc_brg": "_nora_i2c_calc_brg",
    "_dspic33ak_i2c_deinit": "_nora_i2c_deinit",
    "_dspic33ak_i2c_get_device": "_nora_i2c_get_device",
    "_dspic33ak_i2c_get_regs": "_nora_i2c_get_regs",
    "_dspic33ak_i2c_get_role": "_nora_i2c_get_role",
    "_dspic33ak_i2c_init": "_nora_i2c_init",
    "_dspic33ak_i2c_inst_is_valid": "_nora_i2c_inst_is_valid",
    "_dspic33ak_i2c_instance_is_present": "_nora_i2c_instance_is_present",
    "_dspic33ak_i2c_irq_clear": "_nora_i2c_irq_clear",
    "_dspic33ak_i2c_irq_disable": "_nora_i2c_irq_disable",
    "_dspic33ak_i2c_irq_enable": "_nora_i2c_irq_enable",
    "_dspic33ak_i2c_is_initialized": "_nora_i2c_is_initialized",
    "_dspic33ak_i2c_is_present": "_nora_i2c_is_present",
    "_dspic33ak_i2c_ll_ack_busy": "_nora_i2c_ll_ack_busy",
    "_dspic33ak_i2c_ll_ack_issue": "_nora_i2c_ll_ack_issue",
    "_dspic33ak_i2c_ll_has_collision": "_nora_i2c_ll_has_collision",
    "_dspic33ak_i2c_ll_has_error": "_nora_i2c_ll_has_error",
    "_dspic33ak_i2c_ll_has_nack": "_nora_i2c_ll_has_nack",
    "_dspic33ak_i2c_ll_read_byte_get": "_nora_i2c_ll_read_byte_get",
    "_dspic33ak_i2c_ll_read_byte_issue": "_nora_i2c_ll_read_byte_issue",
    "_dspic33ak_i2c_ll_read_byte_ready": "_nora_i2c_ll_read_byte_ready",
    "_dspic33ak_i2c_ll_restart_busy": "_nora_i2c_ll_restart_busy",
    "_dspic33ak_i2c_ll_restart_done": "_nora_i2c_ll_restart_done",
    "_dspic33ak_i2c_ll_restart_issue": "_nora_i2c_ll_restart_issue",
    "_dspic33ak_i2c_ll_start_busy": "_nora_i2c_ll_start_busy",
    "_dspic33ak_i2c_ll_start_done": "_nora_i2c_ll_start_done",
    "_dspic33ak_i2c_ll_start_issue": "_nora_i2c_ll_start_issue",
    "_dspic33ak_i2c_ll_stop_busy": "_nora_i2c_ll_stop_busy",
    "_dspic33ak_i2c_ll_stop_done": "_nora_i2c_ll_stop_done",
    "_dspic33ak_i2c_ll_stop_issue": "_nora_i2c_ll_stop_issue",
    "_dspic33ak_i2c_ll_write_byte_acked": "_nora_i2c_ll_write_byte_acked",
    "_dspic33ak_i2c_ll_write_byte_busy": "_nora_i2c_ll_write_byte_busy",
    "_dspic33ak_i2c_ll_write_byte_issue": "_nora_i2c_ll_write_byte_issue",
    "_dspic33ak_i2c_master_read_after_restart": "_nora_i2c_master_read_after_restart",
    "_dspic33ak_i2c_master_stop": "_nora_i2c_master_stop",
    "_dspic33ak_i2c_master_write_no_stop": "_nora_i2c_master_write_no_stop",
    "_dspic33ak_i2c_read": "_nora_i2c_read",
    "_dspic33ak_i2c_set_bus_speed": "_nora_i2c_set_bus_speed",
    "_dspic33ak_i2c_set_interrupt_priority": "_nora_i2c_set_interrupt_priority",
    "_dspic33ak_i2c_set_role": "_nora_i2c_set_role",
    "_dspic33ak_i2c_write": "_nora_i2c_write",
    "_dspic33ak_i2c_write_read": "_nora_i2c_write_read",
    "_dspic33ak_pinmux_route_input": "_nora_pinmux_route_input",
    "_dspic33ak_pinmux_route_output": "_nora_pinmux_route_output",
    "_dspic33ak_pps_lock": "_nora_pps_lock",
    "_dspic33ak_pps_route_input": "_nora_pps_route_input",
    "_dspic33ak_pps_route_output": "_nora_pps_route_output",
    "_dspic33ak_pps_unlock": "_nora_pps_unlock",
    "_dspic33ak_reset_cause": "_nora_reset_snapshot_cause",
    "_dspic33ak_reset_cause_str": "_nora_reset_snapshot_cause_str",
    "_dspic33ak_reset_is_power_on_class": "_nora_reset_snapshot_is_power_on_class",
    "_dspic33ak_reset_is_warm": "_nora_reset_snapshot_is_warm",
    "_dspic33ak_reset_latch_cause": "_nora_reset_snapshot_capture",
    "_dspic33ak_reset_raw": "_nora_reset_snapshot_raw",
    "_dspic33ak_spi_i2s_tdm_close": "_nora_spi_i2s_tdm_close",
    "_dspic33ak_spi_i2s_tdm_configure_system": "_nora_spi_i2s_tdm_configure_system",
    "_dspic33ak_spi_i2s_tdm_consume_clock_event": "_nora_spi_i2s_tdm_consume_clock_event",
    "_dspic33ak_spi_i2s_tdm_diag_check_deadline": "_nora_spi_i2s_tdm_diag_check_deadline",
    "_dspic33ak_spi_i2s_tdm_diag_get_load": "_nora_spi_i2s_tdm_diag_get_load",
    "_dspic33ak_spi_i2s_tdm_diag_isr_begin": "_nora_spi_i2s_tdm_diag_isr_begin",
    "_dspic33ak_spi_i2s_tdm_diag_isr_end": "_nora_spi_i2s_tdm_diag_isr_end",
    "_dspic33ak_spi_i2s_tdm_diag_note_block": "_nora_spi_i2s_tdm_diag_note_block",
    "_dspic33ak_spi_i2s_tdm_diag_note_dma_status": "_nora_spi_i2s_tdm_diag_note_dma_status",
    "_dspic33ak_spi_i2s_tdm_diag_note_errflags": "_nora_spi_i2s_tdm_diag_note_errflags",
    "_dspic33ak_spi_i2s_tdm_diag_read_counts": "_nora_spi_i2s_tdm_diag_read_counts",
    "_dspic33ak_spi_i2s_tdm_diag_reset": "_nora_spi_i2s_tdm_diag_reset",
    "_dspic33ak_spi_i2s_tdm_fs_clc_engage": "_nora_spi_i2s_tdm_fs_clc_engage",
    "_dspic33ak_spi_i2s_tdm_fs_clc_release": "_nora_spi_i2s_tdm_fs_clc_release",
    "_dspic33ak_spi_i2s_tdm_get_last_error": "_nora_spi_i2s_tdm_get_last_error",
    "_dspic33ak_spi_i2s_tdm_get_load": "_nora_spi_i2s_tdm_get_load",
    "_dspic33ak_spi_i2s_tdm_get_status": "_nora_spi_i2s_tdm_get_status",
    "_dspic33ak_spi_i2s_tdm_hw_apply_config": "_nora_spi_i2s_tdm_hw_apply_config",
    "_dspic33ak_spi_i2s_tdm_hw_dma_config": "_nora_spi_i2s_tdm_hw_dma_config",
    "_dspic33ak_spi_i2s_tdm_hw_dma_trigger_enable": "_nora_spi_i2s_tdm_hw_dma_trigger_enable",
    "_dspic33ak_spi_i2s_tdm_hw_get_ss_pps_code": "_nora_spi_i2s_tdm_hw_get_ss_pps_code",
    "_dspic33ak_spi_i2s_tdm_hw_irq_clear_flags": "_nora_spi_i2s_tdm_hw_irq_clear_flags",
    "_dspic33ak_spi_i2s_tdm_hw_module_enable": "_nora_spi_i2s_tdm_hw_module_enable",
    "_dspic33ak_spi_i2s_tdm_hw_sample_ack_errflags": "_nora_spi_i2s_tdm_hw_sample_ack_errflags",
    "_dspic33ak_spi_i2s_tdm_hw_soft_stop": "_nora_spi_i2s_tdm_hw_soft_stop",
    "_dspic33ak_spi_i2s_tdm_inst": "_nora_spi_i2s_tdm_inst",
    "_dspic33ak_spi_i2s_tdm_inst_configure": "_nora_spi_i2s_tdm_inst_configure",
    "_dspic33ak_spi_i2s_tdm_inst_get_load": "_nora_spi_i2s_tdm_inst_get_load",
    "_dspic33ak_spi_i2s_tdm_inst_get_setup": "_nora_spi_i2s_tdm_inst_get_setup",
    "_dspic33ak_spi_i2s_tdm_inst_get_status": "_nora_spi_i2s_tdm_inst_get_status",
    "_dspic33ak_spi_i2s_tdm_inst_rx_isr": "_nora_spi_i2s_tdm_inst_rx_isr",
    "_dspic33ak_spi_i2s_tdm_inst_start": "_nora_spi_i2s_tdm_inst_start",
    "_dspic33ak_spi_i2s_tdm_inst_stop": "_nora_spi_i2s_tdm_inst_stop",
    "_dspic33ak_spi_i2s_tdm_inst_tx_active_half": "_nora_spi_i2s_tdm_inst_tx_active_half",
    "_dspic33ak_spi_i2s_tdm_inst_tx_active_pos": "_nora_spi_i2s_tdm_inst_tx_active_pos",
    "_dspic33ak_spi_i2s_tdm_inst_tx_fill_mirror": "_nora_spi_i2s_tdm_inst_tx_fill_mirror",
    "_dspic33ak_spi_i2s_tdm_inst_tx_fill_ptr": "_nora_spi_i2s_tdm_inst_tx_fill_ptr",
    "_dspic33ak_spi_i2s_tdm_instance_count": "_nora_spi_i2s_tdm_instance_count",
    "_dspic33ak_spi_i2s_tdm_is_active": "_nora_spi_i2s_tdm_is_active",
    "_dspic33ak_spi_i2s_tdm_is_running": "_nora_spi_i2s_tdm_is_running",
    "_dspic33ak_spi_i2s_tdm_open": "_nora_spi_i2s_tdm_open",
    "_dspic33ak_spi_i2s_tdm_set_block_callback": "_nora_spi_i2s_tdm_set_block_callback",
    "_dspic33ak_spi_i2s_tdm_set_port": "_nora_spi_i2s_tdm_set_port",
    "_dspic33ak_spi_i2s_tdm_spi1": "_nora_spi_i2s_tdm_spi1",
    "_dspic33ak_spi_i2s_tdm_spi2": "_nora_spi_i2s_tdm_spi2",
    "_dspic33ak_spi_i2s_tdm_spi3": "_nora_spi_i2s_tdm_spi3",
    "_dspic33ak_spi_i2s_tdm_spi4": "_nora_spi_i2s_tdm_spi4",
    "_dspic33ak_spi_i2s_tdm_start_all_domains": "_nora_spi_i2s_tdm_start_all_domains",
    "_dspic33ak_spi_i2s_tdm_start_domain": "_nora_spi_i2s_tdm_start_domain",
    "_dspic33ak_spi_i2s_tdm_stop_all_domains": "_nora_spi_i2s_tdm_stop_all_domains",
    "_dspic33ak_spi_i2s_tdm_stop_domain": "_nora_spi_i2s_tdm_stop_domain",
    "_dspic33ak_spi_i2s_tdm_sumprof_configure": "_nora_spi_i2s_tdm_dspic33ak_sumprof_configure",
    "_dspic33ak_spi_i2s_tdm_sumprof_reset": "_nora_spi_i2s_tdm_dspic33ak_sumprof_reset",
    "_dspic33ak_spi_i2s_tdm_sumprof_snapshot": "_nora_spi_i2s_tdm_dspic33ak_sumprof_snapshot",
    "_dspic33ak_spi_i2s_tdm_tdmsum_configure": "_nora_spi_i2s_tdm_tdmsum_configure",
    "_dspic33ak_spi_i2s_tdm_tdmsum_get": "_nora_spi_i2s_tdm_tdmsum_get",
    "_dspic33ak_spi_i2s_tdm_tdmsum_reset": "_nora_spi_i2s_tdm_tdmsum_reset",
    "_dspic33ak_tick_timer_deinit": "_nora_tick_timer_deinit",
    "_dspic33ak_tick_timer_get_ms": "_nora_tick_timer_get_ms",
    "_dspic33ak_tick_timer_init": "_nora_tick_timer_init",
    "_dspic33ak_tick_timer_irq_handler": "_nora_tick_timer_irq_handler",
    "_dspic33ak_tick_timer_is_initialized": "_nora_tick_timer_is_initialized",
    "_dspic33ak_tick_timer_is_present": "_nora_tick_timer_is_present",
    "_dspic33ak_uart_async_notify_events": "_nora_uart_dspic33ak_async_notify_events",
    "_dspic33ak_uart_async_rx_feed": "_nora_uart_dspic33ak_async_rx_feed",
    "_dspic33ak_uart_deinit": "_nora_uart_deinit",
    "_dspic33ak_uart_deinit.part": "_nora_uart_deinit.part",
    "_dspic33ak_uart_device_set_rx_irq_priority": "_nora_uart_dspic33ak_device_set_rx_irq_priority",
    "_dspic33ak_uart_device_set_tx_irq_priority": "_nora_uart_dspic33ak_device_set_tx_irq_priority",
    "_dspic33ak_uart_get_baudrate": "_nora_uart_get_baudrate",
    "_dspic33ak_uart_get_device": "_nora_uart_dspic33ak_get_device",
    "_dspic33ak_uart_init": "_nora_uart_init",
    "_dspic33ak_uart_instance_is_present": "_nora_uart_dspic33ak_instance_is_present",
    "_dspic33ak_uart_is_initialized": "_nora_uart_is_initialized",
    "_dspic33ak_uart_is_present": "_nora_uart_is_present",
    "_dspic33ak_uart_read": "_nora_uart_read",
    "_dspic33ak_uart_read_byte": "_nora_uart_read_byte",
    "_dspic33ak_uart_rx_abort": "_nora_uart_rx_abort",
    "_dspic33ak_uart_rx_count_get": "_nora_uart_rx_count_get",
    "_dspic33ak_uart_rx_enable": "_nora_uart_rx_enable",
    "_dspic33ak_uart_rx_flush": "_nora_uart_rx_flush",
    "_dspic33ak_uart_rx_irq_handler": "_nora_uart_rx_irq_handler",
    "_dspic33ak_uart_rx_is_busy": "_nora_uart_rx_is_busy",
    "_dspic33ak_uart_rx_is_enabled": "_nora_uart_rx_is_enabled",
    "_dspic33ak_uart_rx_isr_config": "_nora_uart_dspic33ak_rx_isr_config",
    "_dspic33ak_uart_rx_isr_disable": "_nora_uart_dspic33ak_rx_isr_disable",
    "_dspic33ak_uart_rx_isr_enable": "_nora_uart_dspic33ak_rx_isr_enable",
    "_dspic33ak_uart_rx_isr_flush": "_nora_uart_dspic33ak_rx_isr_flush",
    "_dspic33ak_uart_rx_isr_read_byte": "_nora_uart_dspic33ak_rx_isr_read_byte",
    "_dspic33ak_uart_rx_isr_ready": "_nora_uart_dspic33ak_rx_isr_ready",
    "_dspic33ak_uart_rx_isr_status_clear": "_nora_uart_dspic33ak_rx_isr_status_clear",
    "_dspic33ak_uart_rx_isr_status_get": "_nora_uart_dspic33ak_rx_isr_status_get",
    "_dspic33ak_uart_rx_ready": "_nora_uart_rx_ready",
    "_dspic33ak_uart_rx_start": "_nora_uart_rx_start",
    "_dspic33ak_uart_rx_start_clean": "_nora_uart_rx_start_clean",
    "_dspic33ak_uart_rx_status_clear": "_nora_uart_rx_status_clear",
    "_dspic33ak_uart_rx_status_get": "_nora_uart_rx_status_get",
    "_dspic33ak_uart_set_baudrate": "_nora_uart_set_baudrate",
    "_dspic33ak_uart_set_callback": "_nora_uart_set_callback",
    "_dspic33ak_uart_tx_abort": "_nora_uart_tx_abort",
    "_dspic33ak_uart_tx_count_get": "_nora_uart_tx_count_get",
    "_dspic33ak_uart_tx_done": "_nora_uart_tx_done",
    "_dspic33ak_uart_tx_enable": "_nora_uart_tx_enable",
    "_dspic33ak_uart_tx_irq_handler": "_nora_uart_tx_irq_handler",
    "_dspic33ak_uart_tx_is_busy": "_nora_uart_tx_is_busy",
    "_dspic33ak_uart_tx_is_enabled": "_nora_uart_tx_is_enabled",
    "_dspic33ak_uart_tx_ready": "_nora_uart_tx_ready",
    "_dspic33ak_uart_tx_start": "_nora_uart_tx_start",
    "_dspic33ak_uart_write": "_nora_uart_write",
    "_dspic33ak_uart_write_byte": "_nora_uart_write_byte",
}


def canonical_symbol(name):
    return LEGACY_SYMBOL_ALIASES.get(name, name)


def canonicalize_snapshot_symbols(snapshot):
    """Return a shallow normalized view without rewriting historic baselines."""
    normalized = dict(snapshot)
    normalized["functions"] = dict(
        (canonical_symbol(name), value)
        for name, value in snapshot.get("functions", {}).items())
    normalized["census"] = dict(
        (canonical_symbol(name), value)
        for name, value in snapshot.get("census", {}).items())
    return normalized

# The functions whose cost this refactor must not change.  Anything reachable
# from a TDM block ISR at audio rate belongs here; foreground/console code does
# not (it may grow freely).  Names are the assembler symbols (leading '_').
WATCHED = [
    # --- DMA block ISRs: the entry points of the whole audio hot path ---
    "__DMA0Interrupt",
    "__DMA2Interrupt",
    "_nora_spi_i2s_tdm_inst_rx_isr",
    # --- ASRC clock-measurement capture ISRs ---
    # These were unwatched until 2026-08-08.  They run at the BCLK/FS capture rate
    # and they are the only consumers of the CCP input-capture `_hot` inlines, so a
    # change that turned those inlines back into out-of-line calls or into indirect
    # read-modify-write on IFSx would show up here and nowhere else in this list.
    "__CCP1Interrupt",
    "__CCP2Interrupt",
    # --- resampler kernels (hand asm) ---
    "_mchp_stream8_pair_slot_f32",
    "_stream8pair_slot_body",
    "_stream8pair_slot_tail",
    "_stream8pair_slot_pack",
    "_stream8pair_slot_epilogue",
    "_stream8pair_ce_prep",
    "_stream8pair_slot_body_ce",
    "_stream8pair_slot_tail_ce",
    "_stream8pair_slot_epilogue_ce",
    "_stream8pair_ced_zero",
    "_stream8pair_slot_body_ced",
    "_stream8pair_slot_epilogue_ced",
    "_mchp_stream16_paird_f32",
    "_mchp_stream16_pair_slot32_f32",
    "_mchp_stream16_block_slot32_f32",
    "_asrc_push16_stereo_loop",
    # M=30's normal 16-frame producer uses this fixed-geometry writer.  Watch
    # both its dispatch entry and its two store loops: a call-site mnemonic
    # comparison alone cannot distinguish this target from the generic writer.
    "_mchp_asrc_push16_stereo30_aligned_f32",
    "_asrc_push16_stereo30_aligned_fast",
    "_asrc_push16_stereo30_aligned_mirror_loop",
    "_asrc_push16_stereo30_aligned_main_loop",
    # --- ASRC engine hot path (C; asrc_push is static and inlines into these) ---
    "_asrc_pull",
    "_audio_app_asrc_push_ab",
    "_audio_app_asrc_pull_ab",
    "_audio_app_asrc_push_ab_frames",
    "_audio_app_asrc_push_ba",
    "_audio_app_asrc_pull_ba",
    "_audio_app_asrc_push_ba_frames",
    # --- audio path callbacks (run in the block ISRs) ---
    "_asrc_audio_path_leg_a_callback",
    "_asrc_audio_path_leg_b_callback",
    # --- front-end decimator ---
    "_asrc_decimator_48_to_8_process_s24_left",
    "_asrc_decimator_48_to_8_process_f32",
    "_asrc_decimator_48_to_16_process_s24_left",
    "_asrc_decimator_48_to_16_process_f32",
    "_asrc_decimator_48_to_24_process_s24_left",
    "_asrc_decimator_48_to_24_process_f32",
    # `_process_48_to_24` is deliberately NOT listed: like `_process_48_to_16` it has only two
    # callers and GCC inlines it into both wrappers (measured 2026-07-29 -- the wrappers come out
    # at 289/262 insn, i.e. the whole body, while `_process_48_to_12`'s wrappers are 8/4 insn and
    # the shared body survives as a symbol).  Watching a name that is absent would make every
    # snapshot report a permanent "missing" and train us to ignore that line.
    # The /4 chain was missing from this list until 2026-07-29 even though it has been the
    # shipping 11.025 kHz front end since the respec -- so its cost was unwatched while the
    # /6 and /3 chains beside it were guarded.  `_process_48_to_12` is the shared body the
    # two wrappers call; watching only the wrappers would miss every change inside it.
    "_asrc_decimator_48_to_12_process_s24_left",
    "_asrc_decimator_48_to_12_process_f32",
    "_process_48_to_12",
    # --- level meter (submitted from both callbacks) ---
    "_level_meter_process",
    "_level_meter_process_i32",
    "_level_meter_process_i32_sparse",
]

FUNC_RE = re.compile(r"^([0-9a-fA-F]+)\s+<([^>]+)>:\s*$")
INSN_RE = re.compile(r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2}\s+)+\s*(\S+)(?:\s+(.*))?$")
# GCC appends a translation-unit-unique number to file-local symbols
# (`_rates_hz.26765`, `_CSWTCH.8`).  The number shifts whenever anything earlier
# in the compilation allocates one more, which is a renumbering, not a cost --
# strip it so the size attribution below names real changes only.
LOCAL_SUFFIX_RE = re.compile(r"\.\d+$")


def strip_local_suffix(name):
    return LOCAL_SUFFIX_RE.sub("", name)


def disassemble(objdump, elf, dfp):
    cmd = [objdump, "-d", elf]
    if dfp:
        cmd.append('-mdfp=' + dfp)
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0 or not out.stdout.strip():
        sys.stderr.write("objdump failed (rc=%d)\n%s\n" % (out.returncode, out.stderr))
        sys.exit(2)
    return out.stdout


def parse_functions(text):
    """symbol -> {'mnemonics': [...], 'operands': [...]}

    objdump emits compiler-generated local labels (`.LVL123`, `.LFB37`, `.LCFI49`)
    as section headers of their own; they are debug bookkeeping inside a
    function, not new functions, so instructions after them still belong to the
    enclosing symbol.  Treating them as boundaries would slice every C function
    into a dozen 1-instruction fragments.
    """
    funcs = {}
    cur = None
    for line in text.splitlines():
        m = FUNC_RE.match(line)
        if m:
            if m.group(2).startswith("."):
                continue          # local label: stay inside the current symbol
            cur = m.group(2)
            funcs[cur] = {"mnemonics": [], "operands": []}
            continue
        if cur is None:
            continue
        m = INSN_RE.match(line)
        if m:
            funcs[cur]["mnemonics"].append(m.group(1))
            funcs[cur]["operands"].append((m.group(2) or "").strip())
    return funcs


SIZE_RE = re.compile(
    r'Total\s+"(program|data)"\s+memory\s+used\s*\(bytes\)\s*:\s*0x[0-9a-fA-F]+\s+\((\d+)\)',
    re.I)


def parse_sizes(map_path):
    """Total program and data bytes as the linker reports them."""
    sizes = {}
    if not os.path.exists(map_path):
        return sizes
    with open(map_path, "r", errors="replace") as fh:
        for line in fh:
            m = SIZE_RE.search(line)
            if m:
                sizes[m.group(1).lower() + "_used"] = int(m.group(2))
    return sizes


def banner_strings(args):
    """The two build-stamp tokens baked into the boot banner (main.c), and the
    flash bytes they cost: each is a NUL-terminated string literal, so a clone
    checked out under a longer/shorter directory name -- or a different commit
    hash length -- moves program_used by exactly (new_len - old_len) bytes with
    no code change at all.  Recording the strings here lets compare() net that
    out instead of a human re-deriving it from directory-name character counts
    (measured 2026-07-30: a clone named 5 characters longer than the pinned
    baseline's cost +8 bytes and read as an unexplained regression).

    Note on the checked-in baselines: a `src_dirname` or `git_commit` of
    `<redacted>` means the provenance string was removed before publication.
    `bytes` is the stored measurement and is used as recorded, so the netting
    above stays correct -- only the printed name is unavailable."""
    src_dirname = args.src_dirname or default_src_dirname(REPO_ROOT)
    git_commit = args.git_commit or default_git_commit(REPO_ROOT)
    return {"src_dirname": src_dirname, "git_commit": git_commit,
            "bytes": len(src_dirname) + 1 + len(git_commit) + 1}


def snapshot(args):
    funcs = parse_functions(disassemble(args.objdump, args.elf, args.dfp))
    watched = {}
    missing = []
    for name in WATCHED:
        if name in funcs:
            watched[name] = funcs[name]
        else:
            missing.append(name)
    # Instruction count for *every* function, so that a program-size delta can
    # be attributed to a named function instead of hand-waved.  Cold code is
    # allowed to change; it just has to be explainable.
    census = dict((strip_local_suffix(n), len(f["mnemonics"]))
                  for n, f in funcs.items())
    return {"functions": watched, "missing": missing,
            "census": census, "sizes": parse_sizes(args.map),
            "banner": banner_strings(args)}


def report_snapshot(snap):
    print("watched functions found: %d / %d" % (len(snap["functions"]), len(WATCHED)))
    for name in sorted(snap["functions"]):
        print("  %-48s %5d insn" % (name, len(snap["functions"][name]["mnemonics"])))
    if snap["missing"]:
        print("  not present in this build (ok if the config excludes them):")
        for name in snap["missing"]:
            print("    %s" % name)
    if snap["sizes"]:
        print("sizes: %s" % snap["sizes"])
    if snap.get("banner"):
        b = snap["banner"]
        print("banner strings: dirname=%r commit=%r (%d bytes)" %
              (b["src_dirname"], b["git_commit"], b["bytes"]))


def compare(base, cur):
    """Return (fatal_diffs, notes).

    Fatal = the claim "this build's cost did not change": a watched hot-path
    function whose instruction sequence moved, or any change in data (RAM) use.
    Non-fatal = program (flash) size, which cold configuration and bind-time
    code may legitimately change.  It is still printed with per-function
    attribution, because "explained" and "unnoticed" are different things.
    """
    diffs = []
    notes = []
    for name in sorted(set(base["functions"]) | set(cur["functions"])):
        b = base["functions"].get(name)
        c = cur["functions"].get(name)
        if b is None:
            diffs.append("+ %s: appeared (was absent in the baseline)" % name)
            continue
        if c is None:
            diffs.append("- %s: DISAPPEARED (inlined away, renamed, or dropped)" % name)
            continue
        if b["mnemonics"] == c["mnemonics"]:
            continue
        diffs.append("! %s: %d -> %d instructions" %
                     (name, len(b["mnemonics"]), len(c["mnemonics"])))
        for i, (bm, cm) in enumerate(zip(b["mnemonics"], c["mnemonics"])):
            if bm != cm:
                diffs.append("    first divergence at index %d: %s %s  ->  %s %s" % (
                    i, bm, b["operands"][i], cm, c["operands"][i]))
                break
        else:
            longer = "current" if len(c["mnemonics"]) > len(b["mnemonics"]) else "baseline"
            diffs.append("    common prefix identical; %s has extra instructions" % longer)
    banner_delta = None
    bb, cb = base.get("banner"), cur.get("banner")
    if bb and cb:
        banner_delta = cb["bytes"] - bb["bytes"]
    for key in sorted(set(base["sizes"]) | set(cur["sizes"])):
        bv, cv = base["sizes"].get(key), cur["sizes"].get(key)
        if bv == cv:
            continue
        actual = (cv or 0) - (bv or 0)
        line = "size %s: %s -> %s (%+d)" % (key, bv, cv, actual)
        sink = notes if key == "program_used" else diffs
        sink.append(("" if sink is notes else "! ") + line)
        if key == "program_used" and banner_delta is not None:
            sink.append("    banner strings: %r/%r (%d bytes) -> %r/%r (%d bytes),"
                        " explains %+d of the %+d byte delta" %
                        (bb["src_dirname"], bb["git_commit"], bb["bytes"],
                         cb["src_dirname"], cb["git_commit"], cb["bytes"],
                         banner_delta, actual))
            residual = actual - banner_delta
            if residual == 0:
                sink.append("    residual after banner strings: 0 -- fully explained,"
                            " no code-size change")
                continue
            sink.append("    residual after banner strings: %+d -- attributing that:" % residual)
        sink.extend("    " + t for t in attribute(base, cur))
    return diffs, notes


def attribute(base, cur):
    """Name the functions that account for a size delta (cold code included)."""
    bc, cc = base.get("census") or {}, cur.get("census") or {}
    if not bc or not cc:
        return ["(no census in one of the snapshots -- re-save the baseline"
                " to get size attribution)"]
    rows = []
    for name in set(bc) | set(cc):
        b, c = bc.get(name), cc.get(name)
        if b != c:
            rows.append((abs((c or 0) - (b or 0)), name, b, c))
    if not rows:
        return ["no function changed instruction count -- the delta is in data,"
                " alignment, or a literal pool"]
    rows.sort(reverse=True)
    out = ["attributed to %d function(s):" % len(rows)]
    for _, name, b, c in rows[:15]:
        out.append("  %-52s %s -> %s insn" % (name, b, c))
    if len(rows) > 15:
        out.append("  ... and %d more" % (len(rows) - 15))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--map", default=DEFAULT_MAP)
    ap.add_argument("--objdump", default=DEFAULT_OBJDUMP)
    ap.add_argument("--dfp", default=DEFAULT_DFP)
    ap.add_argument("--src-dirname", default=None,
                    help="override auto-detected -DAPP_SRC_DIRNAME (leaf of repo root)")
    ap.add_argument("--git-commit", default=None,
                    help="override auto-detected -DSONORA_GIT_COMMIT (short=7 HEAD, +_dirty)")
    ap.add_argument("--save", metavar="FILE", help="write a baseline snapshot")
    ap.add_argument("--compare", metavar="FILE", help="compare against a baseline snapshot")
    args = ap.parse_args()

    snap = snapshot(args)

    if args.compare:
        with open(args.compare, "r") as fh:
            base = canonicalize_snapshot_symbols(json.load(fh))
        diffs, notes = compare(base, canonicalize_snapshot_symbols(snap))
        if diffs:
            print("INVARIANT BROKEN:")
            for d in diffs:
                print("  " + d)
        else:
            print("INVARIANT HELD: %d watched hot-path functions instruction-identical,"
                  " data use unchanged." % len(snap["functions"]))
        if notes:
            print("cold-code note (not a hot-path cost, but explain it):")
            for n in notes:
                print("  " + n)
        print("sizes: %s" % snap["sizes"])
        return 1 if diffs else 0

    report_snapshot(snap)
    if args.save:
        with open(args.save, "w") as fh:
            json.dump(snap, fh, indent=1)
        print("baseline written: %s" % args.save)
    return 0


if __name__ == "__main__":
    sys.exit(main())
