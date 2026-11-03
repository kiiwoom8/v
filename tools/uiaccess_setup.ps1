# uiaccess_setup.ps1 - installs the UIAccess build of vim mouse (README, "Hints
# on the Start menu and notifications"). Windows runs a program whose manifest
# asks for uiAccess only when it is signed by a certificate the machine trusts
# and started from Program Files, so this script
#   1. makes a code-signing certificate "CN=vim mouse UIAccess" in your user's
#      certificate store the first time (its private key cannot be exported),
#   2. signs vim-uiaccess.exe (mingw32-make uiaccess) with it,
#   3. trusts the certificate machine-wide and copies the signed exe to
#      "C:\Program Files\vim mouse\vim.exe" (one administrator prompt),
#   4. copies vim.ini next to the source exe to %APPDATA%\vim mouse when there
#      is none there yet (the installed program keeps its settings there),
#      points "Start with Windows" at the new exe when it is on, stops the
#      running vim mouse and starts the installed one (not elevated).
# Run it again after every build; the tray's "Update to latest version" says
# so instead of downloading. -Uninstall removes the installed copy and the
# certificate's trust and starts vim.exe next to the source exe if there is one.
#   powershell -ExecutionPolicy Bypass -File tools\uiaccess_setup.ps1 [-Exe path] [-Uninstall]
param([string]$Exe = "", [switch]$Uninstall, [switch]$Elevated)
$ErrorActionPreference = "Stop"
$Subject = "CN=vim mouse UIAccess"
$Dir = Join-Path $env:ProgramFiles "vim mouse"
$Target = Join-Path $Dir "vim.exe"
$RunKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$AppName = "vim mouse"
$SourceDir = Split-Path -Parent $PSScriptRoot
if ($Exe -eq "") { $Exe = Join-Path $SourceDir "vim-uiaccess.exe" }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# Every running vim mouse (by product name, so an editor called vim.exe is left alone).
function Get-VimMouse {
  Get-Process vim -ErrorAction SilentlyContinue | Where-Object {
    try { $_.MainModule.FileVersionInfo.ProductName -eq $AppName } catch { $false } }
}
function Stop-VimMouse {
  $procs = @(Get-VimMouse)
  if ($procs.Count -gt 0) {
    # A graceful close is not reachable from here (its main window is hidden);
    # the next start puts the system cursors back.
    $procs | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Write-Output ("stopped vim mouse (" + ($procs | ForEach-Object { $_.Path }) -join ", " + ")")
  }
}
# Starts the installed exe without administrator rights, even from an elevated shell.
function Start-Installed {
  if ($isAdmin) { Start-Process explorer.exe -ArgumentList ('"' + $Target + '"') }
  else { Start-Process -FilePath $Target -WorkingDirectory $Dir }
  $deadline = (Get-Date).AddSeconds(8)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    if (@(Get-VimMouse | Where-Object { $_.Path -ieq $Target }).Count -gt 0) { return $true }
  }
  return $false
}
function Elevate([string[]]$ArgList) {
  $all = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"' + $PSCommandPath + '"'), "-Elevated") + $ArgList
  $p = Start-Process powershell -Verb RunAs -ArgumentList $all -Wait -PassThru
  if ($p.ExitCode -ne 0) { throw "the administrator step failed (exit code $($p.ExitCode))" }
}

# ---------------------------------------------------------------------------
# The administrator part: trust the certificate, put the exe in place (or take both out).
if ($Elevated) {
  try {
    if ($Uninstall) {
      Stop-VimMouse
      if (Test-Path $Target) { Remove-Item -Force $Target; Write-Output "removed $Target" }
      if ((Test-Path $Dir) -and -not (Get-ChildItem $Dir)) { Remove-Item -Force $Dir }
      $store = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", "LocalMachine")
      $store.Open("ReadWrite")
      foreach ($c in @($store.Certificates | Where-Object { $_.Subject -eq $Subject })) { $store.Remove($c); Write-Output ("removed the trust in " + $c.Thumbprint) }
      $store.Close()
      exit 0
    }
    $sig = Get-AuthenticodeSignature -FilePath $Exe
    if ($sig.Status -ne "Valid" -and $sig.Status -ne "UnknownError") { throw "the exe is not signed ($($sig.Status))" }
    $cert = $sig.SignerCertificate
    if (-not $cert -or $cert.Subject -ne $Subject) { throw "the exe is not signed by $Subject" }
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", "LocalMachine")
    $store.Open("ReadWrite")
    if (-not ($store.Certificates | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
      $store.Add($cert); Write-Output ("trusted " + $cert.Thumbprint + " machine-wide")
    }
    $store.Close()
    Stop-VimMouse
    New-Item -ItemType Directory -Force $Dir | Out-Null
    Copy-Item -Force $Exe $Target
    Write-Output "installed $Target"
    exit 0
  } catch {
    Write-Output ("failed: " + $_.Exception.Message)
    Start-Sleep -Seconds 4
    exit 1
  }
}

# ---------------------------------------------------------------------------
# The user part.
if ($Uninstall) {
  Stop-VimMouse
  if ($isAdmin) { & $PSCommandPath -Elevated -Uninstall } else { Elevate @("-Uninstall") }
  $run = Get-ItemProperty -Path $RunKey -Name $AppName -ErrorAction SilentlyContinue
  $plain = Join-Path $SourceDir "vim.exe"
  if ($run -and $run.$AppName -like "*$Target*") {
    if (Test-Path $plain) { Set-ItemProperty -Path $RunKey -Name $AppName -Value ('"' + $plain + '"'); Write-Output "Start with Windows now points at $plain" }
    else { Remove-ItemProperty -Path $RunKey -Name $AppName; Write-Output "Start with Windows turned off (no vim.exe next to the source)" }
  }
  if (Test-Path $plain) { Start-Process -FilePath $plain -WorkingDirectory $SourceDir; Write-Output "started $plain" }
  Write-Output "done; vim.ini in $env:APPDATA\vim mouse was left as it is"
  exit 0
}

if (-not (Test-Path $Exe)) { throw "$Exe not found: run mingw32-make uiaccess first, or pass -Exe" }
$bytes = [IO.File]::ReadAllBytes($Exe)
if (-not [Text.Encoding]::ASCII.GetString($bytes).Contains('uiAccess="true"')) {
  throw "$Exe does not ask for UIAccess in its manifest: build it with mingw32-make uiaccess"
}

# The certificate, made once. The private key stays in this user's store and
# cannot be exported: trusting the certificate lets this machine run anything
# signed with that key, so the key must not travel.
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert -or $cert.NotAfter -lt (Get-Date).AddDays(30)) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -KeyExportPolicy NonExportable `
            -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(10) -KeyLength 2048 -HashAlgorithm SHA256
  Write-Output ("made the code-signing certificate " + $cert.Thumbprint)
}

# Sign a copy in the temp folder; the administrator step moves that copy in.
$signed = Join-Path $env:TEMP "vim-uiaccess-signed.exe"
Copy-Item -Force $Exe $signed
$sig = Set-AuthenticodeSignature -FilePath $signed -Certificate $cert -HashAlgorithm SHA256
if ($sig.Status -ne "Valid" -and $sig.Status -ne "UnknownError") { throw "signing failed: $($sig.Status) $($sig.StatusMessage)" }
Write-Output "signed $Exe"

if ($isAdmin) { & $PSCommandPath -Elevated -Exe $signed; if ($LASTEXITCODE -ne 0) { throw "the install step failed" } }
else { Elevate @("-Exe", ('"' + $signed + '"')) }
Remove-Item -Force $signed -ErrorAction SilentlyContinue

# Settings: the installed program cannot write next to itself, so it uses
# %APPDATA%\vim mouse\vim.ini; seed that with the settings in use so far.
$appIni = Join-Path $env:APPDATA "vim mouse\vim.ini"
$srcIni = Join-Path (Split-Path -Parent $Exe) "vim.ini"
if (-not (Test-Path $appIni) -and (Test-Path $srcIni)) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $appIni) | Out-Null
  Copy-Item $srcIni $appIni
  Write-Output "copied $srcIni to $appIni"
}
$run = Get-ItemProperty -Path $RunKey -Name $AppName -ErrorAction SilentlyContinue
if ($run -and $run.$AppName -notlike "*$Target*") {
  Set-ItemProperty -Path $RunKey -Name $AppName -Value ('"' + $Target + '"')
  Write-Output "Start with Windows now points at $Target"
}

if (Start-Installed) {
  Write-Output "vim mouse is running from $Target with UIAccess (Windows would not start it otherwise)."
  Write-Output "With log = 1, vim.log in $env:APPDATA\vim mouse starts with 'uiaccess: on'."
} else {
  throw "vim mouse did not start from $Target; start it by hand to see Windows' message"
}
