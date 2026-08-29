# Disassembly evidence for the IRQ-register-atomicity port.
#
# For one ELF this records, per watched ISR / hot function:
#   - the instruction count
#   - the number of call/rcall instructions (a hot path must not gain calls)
#   - every instruction whose operand is a *direct* address inside the
#     IFS0..IFS11 / IEC0..IEC11 window, split single-bit (bset.b/bclr.b) vs
#     other, so a byte read-modify-write on a shared interrupt register is read
#     off the image instead of being inferred from the C source.
#
# Scope limit: this sees direct-address forms only (`mov.b w1, 0xc8`). A pointer
# form (`mov.b w1, [w2]` with w2 == &IEC2) carries no address in the operand and
# is invisible here. That gap is covered from the source side by
# tools/irq_atomicity_source_lint.py, which bans taking &IFS/&IEC at all.
#
# Usage: pwsh -File tools/irq_atomicity_disasm.ps1 -Elf _ab_before/dsPIC33AK512_ASRC.elf -Out _ab_before/disasm_asrc.txt
param(
    [Parameter(Mandatory = $true)][string]$Elf,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Objdump = 'C:\Program Files\Microchip\xc-dsc\v3.31.01\bin\xc-dsc-objdump.exe',
    # Read out of tools/dfp_packs.py's PINS (via its CLI, since PowerShell
    # cannot import a Python module) so this stays in sync with the pin the
    # build itself uses, instead of duplicating "MP_DFP/1.3.185" here.
    [string]$Dfp
)

if ([string]::IsNullOrWhiteSpace($Dfp)) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $Dfp = & python (Join-Path $repoRoot 'tools/dfp_packs.py') $Elf
    if ($LASTEXITCODE -ne 0) { throw "dfp_packs.py could not resolve a DFP for $Elf : $Dfp" }
}

$ErrorActionPreference = 'Stop'

$watch = @(
    '__DMA0Interrupt', '__DMA1Interrupt', '__DMA2Interrupt', '__DMA3Interrupt',
    '__CCP1Interrupt', '__CCP2Interrupt',
    '_nora_spi_i2s_tdm_inst_rx_isr', '_tdm_rx_block',
    '_nora_dma_hw_irq_enable', '_nora_dma_irq_clear_flag',
    '_nora_ccp_icap_isr', '_nora_ccp_icap_irq_clear',
    '_nora_gpio_event_irq_set_enabled', '_nora_gpio_event_dispatch',
    '_nora_uart_dspic33ak_rx_irq_set_enabled', '_nora_uart_dspic33ak_tx_irq_set_enabled'
)

$lines = & $Objdump -d ("-mdfp=" + $Dfp) $Elf
$sb = [System.Text.StringBuilder]::new()
$cur = $null
$body = $null

function Write-FuncSummary {
    param($name, $body, $sb)
    if ($null -eq $name) { return }
    $insn = @($body | Where-Object { $_ -match '^\s+[0-9a-f]+:\s' })
    $calls = @($insn | Where-Object { $_ -match '\b(call|rcall)\b' })
    # IFS0..IFS11 live at 0x90..0xbf and IEC0..IEC11 at 0xc0..0xef on this family
    # (taken from the ELF symbol table, not guessed).  A direct data-memory
    # operand in that window is a touch of a shared interrupt register; an
    # immediate (#0x..) is not, hence the "not preceded by #" guard.
    $irqreg = @($insn | Where-Object { $_ -match '(?<![#\w.])0x0*([9a-e][0-9a-f])\b' })
    $atomic = @($irqreg | Where-Object { $_ -match '\b(bset|bclr|bfins|btg)' })
    [void]$sb.AppendLine(("{0}: {1} insn, {2} call(s), {3} IFS/IEC touch ({4} single-bit)" `
        -f $name, $insn.Count, $calls.Count, $irqreg.Count, $atomic.Count))
    foreach ($c in $calls) { [void]$sb.AppendLine("    CALL " + $c.Trim()) }
    foreach ($r in $irqreg) { [void]$sb.AppendLine("    IRQREG " + $r.Trim()) }
}

foreach ($l in $lines) {
    if ($l -match '^[0-9a-f]+\s+<([^>]+)>:\s*$') {
        $name = $Matches[1]
        # GCC emits debug/scope labels (.LBB / .LVL / .L0) as their own symbol
        # headers in the middle of a function.  They are not functions: treat
        # them as a continuation, or every body is truncated at the first one.
        if ($name -like '.L*') { continue }
        Write-FuncSummary $cur $body $sb
        if ($watch -contains ($name -replace '\.\d+$', '')) { $cur = $name; $body = @() }
        else { $cur = $null; $body = $null }
        continue
    }
    if ($null -ne $cur) { $body += $l }
}
Write-FuncSummary $cur $body $sb

Set-Content -Path $Out -Value $sb.ToString()
Write-Host ("wrote " + $Out)
