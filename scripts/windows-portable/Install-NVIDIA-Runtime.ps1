[CmdletBinding()]
param([string] $ArchivePath)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$release = 'https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.1.9/Veyra-2.1.9-NVIDIA-win64-portable.zip'
$expectedSha256 = '4225ecb11a8e96c2826b4b0b798feb811d7ae237f3edf0b627f3b6895c79f362'
$prefix = 'Veyra-2.1.9-NVIDIA-win64-portable/'
$download = $null
try {
    if (!$ArchivePath) {
        $download = Join-Path ([IO.Path]::GetTempPath()) ('veyra-runtime-' + [guid]::NewGuid() + '.zip')
        Write-Host 'Downloading the original Veyra 2.1.9 NVIDIA release (697 MiB).'
        Invoke-WebRequest -Uri $release -UseBasicParsing -OutFile $download
        $ArchivePath = $download
    }
    if ((Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash -ne $expectedSha256) {
        throw 'The Veyra archive SHA-256 does not match the published 2.1.9 release.'
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead((Get-Item -LiteralPath $ArchivePath).FullName)
    try {
        $files = [ordered]@{
            'runtime/config/ngx-local.json' = 'runtime/config/ngx-local.json'
            'runtime/experimental/nvngx_vsr.dll' = 'runtime/experimental/nvngx_vsr.dll'
            'runtime/nvidia-vfg/cudart64_12.dll' = 'runtime/nvidia-vfg/cudart64_12.dll'
            'runtime/nvidia-vfg/NVCVImage.dll' = 'runtime/nvidia-vfg/NVCVImage.dll'
            'runtime/nvidia-vfg/nvngxruntime.dll' = 'runtime/nvidia-vfg/nvngxruntime.dll'
            'runtime/nvidia-vfg/NVVideoEffects.dll' = 'runtime/nvidia-vfg/NVVideoEffects.dll'
            'runtime/nvidia-vfg/nvVFXVideoFrameGeneration.dll' = 'runtime/nvidia-vfg/nvVFXVideoFrameGeneration.dll'
            'licenses/NVIDIA_RTX_VIDEO_SDK_LICENSE.pdf' = 'runtime/licenses/NVIDIA_RTX_VIDEO_SDK_LICENSE.pdf'
            'licenses/nvidia-vfg/NVIDIA-Software-License-Agreement-2025.05.05.pdf' = 'runtime/licenses/NVIDIA-Software-License-Agreement.pdf'
            'licenses/nvidia-vfg/product-specific-terms-for-nvidia-ai-products-2025.05.05.pdf' = 'runtime/licenses/NVIDIA-AI-Product-Terms.pdf'
            'licenses/nvidia-vfg/NVIDIA-Open-Model-License-Agreements-24-10-2025.pdf' = 'runtime/licenses/NVIDIA-Open-Model-License.pdf'
        }
        foreach ($source in $files.Keys) {
            if (!$zip.GetEntry($prefix + $source)) { throw "The runtime archive is missing $source." }
        }
        foreach ($source in $files.Keys) {
            $destination = Join-Path $PSScriptRoot $files[$source]
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($zip.GetEntry($prefix + $source), $destination, $true)
        }
    } finally {
        $zip.Dispose()
    }
    Write-Host 'NVIDIA runtime installed. Start chiaki-dlss.exe to use the RTX Video and VFG presets.'
} finally {
    if ($download -and (Test-Path -LiteralPath $download)) { Remove-Item -LiteralPath $download }
}
