#Requires -Version 5.1
<#
.SYNOPSIS
  Windows 10/11 Installer, Backup, and Restorer for Actions ATJ2157 ZX Spectrum Next TZX Mode.
  Automatically installs a locally signed zero-prompt inbox WinUSB driver for ADFU (10d6:10d6)
  and runs tzx_installer.py using 100% built-in Windows DLLs (no Zadig or extra packages).

.EXAMPLE
  .\install.ps1 -Check                 # Read-only compatibility check (writes nothing)
  .\install.ps1                        # Backup stock firmware, verify, and install TZX Mode
  .\install.ps1 -Backup                # Read-only full 4 MiB flash backup
  .\install.ps1 -Restore .\backups\... # Bit-exact restore to factory stock
  .\install.ps1 -Update  .\backups\... # Update TZX Mode using an existing stock backup
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Backup,
    [string]$Restore,
    [string]$Update,
    [switch]$Yes,
    [switch]$SkipDriver
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# 1. Auto-elevate via UAC once (needed for WinUSB INF install + raw PhysicalDrive SCSI pass-through)
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "[Windows] Elevating via UAC for WinUSB driver & USB SCSI access..."
    $fwd = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$($MyInvocation.MyCommand.Path)`"")
    if ($Check)      { $fwd += "-Check" }
    if ($Backup)     { $fwd += "-Backup" }
    if ($Restore)    { $fwd += @("-Restore", "`"$Restore`"") }
    if ($Update)     { $fwd += @("-Update", "`"$Update`"") }
    if ($Yes)        { $fwd += "-Yes" }
    if ($SkipDriver) { $fwd += "-SkipDriver" }
    $p = Start-Process -FilePath "powershell.exe" -ArgumentList $fwd -Verb RunAs -PassThru -Wait
    exit $p.ExitCode
}

# 2. Zero-prompt inbox WinUSB INF + dynamically generated & signed .cat catalog (mscat32.dll)
function Install-AdfuWinUsbDriver {
    $drvDir  = Join-Path $ScriptDir "drivers"
    $infPath = Join-Path $drvDir "atj2157_adfu.inf"
    $catPath = Join-Path $drvDir "atj2157_adfu.cat"
    $cdfPath = Join-Path $drvDir "atj2157_adfu.cdf"

    if (-not (Test-Path $infPath)) {
        throw "Missing driver INF: $infPath"
    }

    # Check if driver is already staged in Windows Driver Store
    $existing = & pnputil.exe /enum-drivers 2>$null | Select-String -Pattern "atj2157_adfu\.inf"
    if ($existing) {
        Write-Host "[Driver] ATJ2157 ADFU WinUSB driver already installed."
        return
    }

    Write-Host "[Driver] Creating local trusted code-signing certificate & signed WinUSB catalog..."
    $subject = "CN=TZX Mode Local Driver Signing"
    $cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(10)
    }

    # Trust locally in Root & TrustedPublisher so pnputil installs with zero prompts
    foreach ($storeName in @("Root", "TrustedPublisher")) {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, "LocalMachine")
        $store.Open("ReadWrite")
        if (-not ($store.Certificates | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
            $store.Add($cert)
        }
        $store.Close()
    }

    # Write CDF file and build .cat via native mscat32.dll (no WDK/Inf2Cat required)
    $cdfContent = @"
[CatalogHeader]
Name=$catPath
CatalogVersion=2
HashAlgorithms=SHA256
[CatalogFiles]
<hash>atj2157_adfu.inf=$infPath
"@
    [System.IO.File]::WriteAllText($cdfPath, $cdfContent, [System.Text.Encoding]::ASCII)

    if (-not ("CatBuilder" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class CatBuilder {
    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CryptCATCDFOpen(string pwszFilePath, IntPtr pfnParseError);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern bool CryptCATCDFClose(IntPtr pCDF);
    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CryptCATCDFEnumMembersByCDFTagEx(
        IntPtr pCDF, IntPtr pwszPrevCDFTag, IntPtr pfnParseError,
        ref IntPtr ppMember, bool fContinueOnError, IntPtr pvReserved);
    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CryptCATCDFEnumAttributesWithCDFTag(
        IntPtr pCDF, IntPtr pwszMemberTag, IntPtr pMember,
        IntPtr pPrevAttr, IntPtr pfnParseError);

    public static void Build(string cdfPath) {
        IntPtr cdf = CryptCATCDFOpen(cdfPath, IntPtr.Zero);
        if (cdf == IntPtr.Zero) throw new Exception("CryptCATCDFOpen failed: " + Marshal.GetLastWin32Error());
        try {
            IntPtr tag = IntPtr.Zero;
            IntPtr member = IntPtr.Zero;
            while ((tag = CryptCATCDFEnumMembersByCDFTagEx(cdf, tag, IntPtr.Zero, ref member, false, IntPtr.Zero)) != IntPtr.Zero) {
                IntPtr attr = IntPtr.Zero;
                while ((attr = CryptCATCDFEnumAttributesWithCDFTag(cdf, tag, member, attr, IntPtr.Zero)) != IntPtr.Zero) { }
            }
        } finally {
            CryptCATCDFClose(cdf);
        }
    }
}
"@
    }
    [CatBuilder]::Build($cdfPath)
    Remove-Item -Force -ErrorAction SilentlyContinue $cdfPath

    $sig = Set-AuthenticodeSignature -FilePath $catPath -Certificate $cert -HashAlgorithm SHA256
    if ($sig.Status -ne "Valid") {
        throw "Failed to sign catalog ${catPath}: $($sig.Status)"
    }

    Write-Host "[Driver] Installing WinUSB driver into Windows Driver Store..."
    $out = & pnputil.exe /add-driver $infPath /install 2>&1
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) {
        Write-Host ($out | Out-String)
        throw "pnputil failed with exit code $LASTEXITCODE"
    }
    Write-Host "[Driver] WinUSB driver ready for 10d6:10d6 (ADFU)."
}

if (-not $SkipDriver) {
    Install-AdfuWinUsbDriver
}

# 3. Locate Python 3
$pyCmd = $null
if (Get-Command py -ErrorAction SilentlyContinue) {
    $pyCmd = @("py", "-3")
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $pyCmd = @("python")
} else {
    Write-Error "Python 3 is required. Install Python 3 from https://www.python.org/downloads/ (or 'winget install Python.Python.3.12') and re-run."
    exit 1
}

# 4. Build arguments and run tzx_installer.py
$pyArgs = @()
if ($pyCmd.Count -gt 1) { $pyArgs += $pyCmd[1] }
$pyArgs += (Join-Path $ScriptDir "tzx_installer.py")

if ($Check) {
    $pyArgs += "check"
} elseif ($Backup) {
    $pyArgs += "backup"
} elseif ($Restore) {
    $pyArgs += @("restore", $Restore)
} else {
    $pyArgs += "install"
    if ($Update) { $pyArgs += @("--backup-dir", $Update) }
}
if ($Yes) { $pyArgs += "--yes" }

& $pyCmd[0] @pyArgs
exit $LASTEXITCODE
