[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet('Native', 'RtxVideo4K', 'RtxVideo4K60', 'Hdr60')]
    [string] $Preset,
    [switch] $AutoHdrPeak,
    [string] $ChiakiExe,
    [string] $VeyraRuntimeDir,
    [string] $ConsoleNickname,
    [string] $ConsoleAddress
)

$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'This preset targets Windows.' }

$package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'chiaki-ps5-preset.json') -Raw | ConvertFrom-Json
if ($package.schema -ne 1) { throw 'Unsupported preset schema.' }
if (!$Preset) { $Preset = $package.defaultProfile }
$profile = $package.profiles.$Preset
if (!$profile -or $profile.codec -cnotin @('h265', 'h265_hdr') -or
    $profile.sourceFps -notin @(30, 60) -or $profile.rtxVideoQuality -notin @(0, 4) -or
    $profile.frameGeneration -cnotin @('off', 'low')) {
    throw 'Invalid video profile.'
}

# Write only the video settings listed here, using Qt's registry value types.
$integers = @(
    'bitrate_local_ps5', 'display_target_prim', 'display_target_trc', 'display_target_peak',
    'display_target_contrast', 'hdr_peak', 'hdr_black', 'hdr_midtones', 'hdr_saturation', 'hdr_curve'
)
$strings = @('resolution_local_ps5', 'render_backend', 'hw_decoder', 'placebo_preset', 'window_type')
$booleans = @(
    'direct_frame_mapping', 'use_zero_copy', 'vsync', 'vulkan_deferred_swap',
    'idr_on_fec_failure', 'hdr_display_gamut'
)
$approved = $integers + $strings + $booleans
$fields = @($package.settings.PSObject.Properties.Name)
if (@(Compare-Object -ReferenceObject $approved -DifferenceObject $fields -CaseSensitive).Count -ne 0) {
    throw 'Unexpected or missing video settings in the preset.'
}
$values = [ordered]@{}
foreach ($name in $integers) { $values[$name] = [int]$package.settings.$name }
foreach ($name in $strings) { $values[$name] = [string]$package.settings.$name }
foreach ($name in $booleans) {
    $value = $package.settings.$name
    if ($value -isnot [bool]) { throw "Invalid boolean setting: $name" }
    $values[$name] = $value.ToString().ToLowerInvariant()
}
$values['codec_local_ps5'] = [string]$profile.codec
$values['fps_local_ps5'] = [string]$profile.sourceFps
if ($AutoHdrPeak) { $values['hdr_peak'] = 0 }

if ([bool]$ConsoleNickname -ne [bool]$ConsoleAddress -or ($ConsoleAddress -and !$ChiakiExe)) {
    throw 'Direct connection requires -ChiakiExe, -ConsoleNickname and -ConsoleAddress together.'
}
$launchEnvironment = @{
    CHIAKI_DIRECT_PRESENT_IMMEDIATE = '1'
    CHIAKI_VRR = '1'
    CHIAKI_DLSS_SR = '0'
    CHIAKI_STREAM_CLEANUP = '0'
    CHIAKI_RTX_VIDEO_SR = [string]$profile.rtxVideoQuality
    CHIAKI_RTX_VIDEO_SCALE = '2'
    CHIAKI_VFG = [string]$profile.frameGeneration
}
$launchArguments = @()
if ($ChiakiExe) {
    $ChiakiExe = (Get-Item -LiteralPath $ChiakiExe).FullName
    if (![System.IO.File]::Exists($ChiakiExe)) { throw 'Select a Chiaki-dlss executable.' }
    $appDirectory = Split-Path -LiteralPath $ChiakiExe
    if ($profile.rtxVideoQuality -gt 0 -or $profile.frameGeneration -ne 'off') {
        if (!(Test-Path -LiteralPath (Join-Path $appDirectory 'chiaki-veyra-vfg.dll') -PathType Leaf)) {
            throw 'Place chiaki-veyra-vfg.dll beside the Chiaki-dlss executable.'
        }
        if (!$VeyraRuntimeDir) { throw 'Supply the Veyra runtime directory with -VeyraRuntimeDir.' }
        $runtime = (Get-Item -LiteralPath $VeyraRuntimeDir).FullName
        if ($profile.rtxVideoQuality -gt 0) {
            $launchEnvironment.CHIAKI_DLSS_RUNTIME = (Get-Item -LiteralPath (Join-Path $runtime 'experimental')).FullName
            if (!(Test-Path -LiteralPath (Join-Path $runtime 'config\ngx-local.json') -PathType Leaf)) {
                throw 'The Veyra runtime is missing config\ngx-local.json.'
            }
        }
        if ($profile.frameGeneration -ne 'off') {
            $launchEnvironment.CHIAKI_VFG_RUNTIME = (Get-Item -LiteralPath (Join-Path $runtime 'nvidia-vfg')).FullName
            if (!(Test-Path -LiteralPath (Join-Path $appDirectory 'shaders\VfgShotChange.dxil') -PathType Leaf)) {
                throw 'Place VfgShotChange.dxil in the shaders directory beside Chiaki-dlss.'
            }
        }
    }
    if ($ConsoleAddress) {
        $launchArguments = @('--borderless-fullscreen', '--exit-app-on-stream-exit', '--source-fps',
                             [string]$profile.sourceFps, 'stream', $ConsoleNickname, $ConsoleAddress)
    }
}

if (!$PSCmdlet.ShouldProcess("Chiaki video settings for the current Windows user", "Apply $Preset")) {
    [pscustomobject]@{ Preset = $Preset; VideoSettings = $values; LaunchEnvironment = $launchEnvironment }
    return
}

$registry = [Microsoft.Win32.Registry]::CurrentUser
$appKey = 'Software\Chiaki\Chiaki'
$selector = $registry.OpenSubKey("$appKey\settings")
$activeProfile = ''
if ($selector) {
    try { $activeProfile = [string]$selector.GetValue('current_profile', '') }
    finally { $selector.Dispose() }
}
if ($activeProfile.Contains('\') -or $activeProfile.Contains('/')) {
    throw 'The active Chiaki profile contains a path separator.'
}
if ($activeProfile) { $appKey += "-$activeProfile" }

$key = $registry.CreateSubKey("$appKey\settings")
try {
    foreach ($name in $values.Keys) {
        $kind = if ($name -in $integers) { 'DWord' } else { 'String' }
        $key.SetValue($name, $values[$name], [Microsoft.Win32.RegistryValueKind]$kind)
    }
} finally {
    $key.Dispose()
}
Write-Host "Applied $Preset to the active Chiaki profile."

if ($ChiakiExe) {
    $savedEnvironment = @{}
    Push-Location -LiteralPath $appDirectory
    try {
        foreach ($name in $launchEnvironment.Keys) {
            $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
            [Environment]::SetEnvironmentVariable($name, $launchEnvironment[$name], 'Process')
        }
        & $ChiakiExe @launchArguments
    } finally {
        foreach ($name in $savedEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
        }
        Pop-Location
    }
}
