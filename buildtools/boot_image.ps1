<#
  Shared reader for src/boot/boot_image.psd1 -- the resident boot image manifest.

  WHY A SEPARATE FILE

  Four consumers need the manifest, and each needs it in a slightly different form:

    build_resident_bootloader.ps1   builds ONE device's image
    generate_resident_project.ps1   emits one IDE configuration PER device
    check_resident_project.ps1      regenerates and compares, per device
    sonora_build_state.ps1          only needs the SET of devices that have an image

  The manifest is shared-plus-per-device (Devices / DefaultDevice), so every one of
  those consumers would otherwise have written its own "shared keys, then override
  with the device's" merge. Four merges is four places for a key to be forgotten --
  and a forgotten key here does not fail loudly: it silently builds the default
  device's image under another device's name. So the merge lives here, once, and it
  validates while it is at it.

  Dot-source it:  . (Join-Path $PSScriptRoot 'boot_image.ps1')
#>

# Every device's entry must carry all of these. They are the whole of what differs
# from part to part; anything else in the manifest is shared by construction.
$script:BootImageDeviceKeys = @('ConfigurationName', 'LinkerScript', 'Defsym', 'SizeCapBytes',
                                'DfpPack', 'DfpPackVersion')

# Shared keys, i.e. statements about the image rather than about a part.
$script:BootImageSharedKeys = @('ProjectName', 'Sources', 'Includes', 'Macros',
                                'CompilerFlags', 'LinkerOptions')

function Get-BootImageManifest {
    <#
      Reads and VALIDATES src/boot/boot_image.psd1. Validation is here rather than in
      each consumer because a manifest with a missing key must fail at the moment it
      is read, naming the key -- not later, as a link error or a wrong-looking image.
    #>
    param([Parameter(Mandatory)][string]$RepoRoot)

    $manifestPath = Join-Path $RepoRoot 'src\boot\boot_image.psd1'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "The boot image manifest is missing: $manifestPath"
    }
    $image = Import-PowerShellDataFile -LiteralPath $manifestPath

    foreach ($key in ($script:BootImageSharedKeys + @('Devices', 'DefaultDevice'))) {
        if (-not $image.ContainsKey($key)) {
            throw "src/boot/boot_image.psd1 has no '$key' entry."
        }
    }
    if (@($image.Devices.Keys).Count -eq 0) {
        throw 'src/boot/boot_image.psd1 lists no devices under Devices.'
    }
    if (-not $image.Devices.ContainsKey($image.DefaultDevice)) {
        throw ("src/boot/boot_image.psd1 DefaultDevice is '$($image.DefaultDevice)', which is " +
               "not one of its Devices: $((@($image.Devices.Keys) | Sort-Object) -join ', ')")
    }
    foreach ($device in @($image.Devices.Keys)) {
        foreach ($key in $script:BootImageDeviceKeys) {
            if (-not $image.Devices[$device].ContainsKey($key)) {
                throw "src/boot/boot_image.psd1 device '$device' has no '$key' entry."
            }
        }
    }
    # A per-device key left behind at the top level is the dangerous shape: the merge
    # below would ignore it, so the file would say one thing and every build do another.
    foreach ($key in $script:BootImageDeviceKeys) {
        if ($image.ContainsKey($key)) {
            throw ("src/boot/boot_image.psd1 states '$key' both per device and at the top " +
                   'level. It is per device -- remove the top-level copy, or the top-level ' +
                   'one is a value nothing reads.')
        }
    }

    # build/ and dist/ are named after ConfigurationName, so two devices sharing one
    # would put two different images in one directory and each build would look like
    # it had simply rebuilt the other.
    $names = @(@($image.Devices.Keys) | ForEach-Object { $image.Devices[$_].ConfigurationName })
    if (@($names | Sort-Object -Unique).Count -ne $names.Count) {
        throw ('src/boot/boot_image.psd1 gives two devices the same ConfigurationName ' +
               "($($names -join ', ')). build/ and dist/ are named after it.")
    }

    return $image
}

function Get-BootImageDevices {
    <#
      Every device with a resident image, in a DETERMINISTIC order: the default first,
      then the rest sorted. Determinism is not cosmetic -- generate_resident_project.ps1
      emits one configuration per device and check_resident_project.ps1 compares its
      output, so an order that varied between runs would make the gate fail at random.
      Default first so the configuration a bare build produces is the one at the top of
      the IDE's dropdown.
    #>
    param([Parameter(Mandatory)][hashtable]$Image)

    $rest = @(@($Image.Devices.Keys) | Where-Object { $_ -ne $Image.DefaultDevice } | Sort-Object)
    return @($Image.DefaultDevice) + $rest
}

function Get-BootImageForDevice {
    <#
      The manifest as ONE device sees it: the shared keys plus that device's four, plus
      a 'Device' key holding the part number. Consumers that build or describe a single
      image take this and never look at Devices/DefaultDevice again.
    #>
    param(
        [Parameter(Mandatory)][hashtable]$Image,
        [Parameter(Mandatory)][string]$Device
    )

    if (-not $Image.Devices.ContainsKey($Device)) {
        throw ("src/boot/boot_image.psd1 has no resident image for device '$Device'. " +
               "It knows: $((@($Image.Devices.Keys) | Sort-Object) -join ', ')")
    }

    $view = @{ Device = $Device }
    foreach ($key in $script:BootImageSharedKeys) { $view[$key] = $Image[$key] }
    foreach ($key in $script:BootImageDeviceKeys) { $view[$key] = $Image.Devices[$Device][$key] }
    return $view
}
