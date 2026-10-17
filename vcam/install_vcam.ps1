# Installs (or removes) the iPhoneCam virtual camera media source.
# Windows Frame Server only loads virtual camera sources registered under HKLM, so this needs admin rights.
# Keep this file ASCII-only: Windows PowerShell 5.1 misreads UTF-8 files without a BOM.
param(
    [int]$Width = 1920,
    [int]$Height = 1080,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Width', $Width, '-Height', $Height)
    if ($Uninstall) { $argList += '-Uninstall' }
    $proc = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$installDir = Join-Path $env:ProgramFiles 'iPhoneCam'
$dllPath = Join-Path $installDir 'iphonecam.dll'
$logDir = 'C:\ProgramData\iPhoneCam\logs'
$regKey = 'HKLM:\SOFTWARE\iPhoneCam'
$transcript = Join-Path $env:TEMP 'iphonecam-install.log'
Start-Transcript -Path $transcript -Force | Out-Null

try {
    # Frame Server keeps the source DLL loaded; stop it so the file can be replaced
    foreach ($svc in 'FrameServer', 'FrameServerMonitor') {
        $s = Get-Service -Name $svc -ErrorAction SilentlyContinue
        if ($s -and $s.Status -eq 'Running') { Stop-Service -Name $svc -Force }
    }

    if ($Uninstall) {
        if (Test-Path $dllPath) {
            & regsvr32.exe /u /s $dllPath
            Remove-Item -Recurse -Force $installDir
        }
        Remove-Item -Recurse -Force $regKey -ErrorAction SilentlyContinue
        Write-Host 'iPhoneCam virtual camera removed.'
        exit 0
    }

    if ($Width % 2 -or $Height % 2) { throw 'Width and Height must be even.' }
    $built = Join-Path $PSScriptRoot 'build\Release\iphonecam.dll'
    if (-not (Test-Path $built)) { throw "Build output not found: $built (run: cmake --build vcam\build --config Release)" }

    New-Item -ItemType Directory -Force -Path $installDir | Out-Null
    if (Test-Path $dllPath) {
        # a process may still hold the old DLL: rename it out of the way (allowed while loaded)
        Get-ChildItem $installDir -Filter 'iphonecam.dll.old-*' | Remove-Item -Force -ErrorAction SilentlyContinue
        Rename-Item $dllPath ("iphonecam.dll.old-" + (Get-Date -Format 'yyyyMMddHHmmss'))
    }
    Copy-Item $built $dllPath

    $p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$dllPath`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($p.ExitCode)" }

    New-Item -Force -Path $regKey | Out-Null
    Set-ItemProperty -Path $regKey -Name Width -Value $Width -Type DWord
    Set-ItemProperty -Path $regKey -Name Height -Value $Height -Type DWord

    # logs are written from Frame Server (LocalService/SYSTEM) and from the user's server.py
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    & icacls.exe $logDir /grant '*S-1-5-19:(OI)(CI)M' '*S-1-5-18:(OI)(CI)F' '*S-1-5-4:(OI)(CI)M' | Out-Null

    Write-Host "iPhoneCam virtual camera installed: $dllPath (${Width}x${Height})"
    exit 0
}
catch {
    Write-Host "ERROR: $_"
    exit 1
}
finally {
    Stop-Transcript | Out-Null
}
