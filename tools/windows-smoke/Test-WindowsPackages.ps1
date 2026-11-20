[CmdletBinding()]
param([Parameter(Mandatory)] [string] $Scratch)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64' -or
    [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture -ne 'X64' -or
    -not [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([Runtime.InteropServices.OSPlatform]::Windows)) {
    throw 'This smoke requires a native Windows x64 PowerShell process and operating system.'
}
if ($env:EOD_DRAFT_TOKEN -or $env:GH_TOKEN -or $env:GITHUB_TOKEN) {
    throw 'Remove GitHub credentials before executing the package.'
}

$Scratch = (Resolve-Path -LiteralPath $Scratch).Path
$receipt = Get-Content -LiteralPath (Join-Path $Scratch 'receipt.json') -Raw | ConvertFrom-Json
$system = Get-CimInstance Win32_OperatingSystem
Write-Host ("Runner: {0}, version {1}, build {2}, native x64, image {3} {4}" -f
    $system.Caption, $system.Version, $system.BuildNumber, $env:ImageOS, $env:ImageVersion)
Write-Host ($receipt | ConvertTo-Json -Depth 4)

function Expand-VerifiedBundle($Asset, [string] $Destination) {
    $zip = Join-Path $Scratch $Asset.archive
    if ((Get-Item -LiteralPath $zip).Length -ne $Asset.bytes -or
        (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $Asset.sha256) {
        throw 'Archive bytes no longer match the authenticated receipt.'
    }
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $size = [long] 0
    try {
        if ($archive.Entries.Count -lt 1 -or $archive.Entries.Count -gt 20000) {
            throw 'Archive entry count is outside the expected bounds.'
        }
        foreach ($entry in $archive.Entries) {
            $relative = $entry.FullName.TrimEnd('/')
            $parts = $relative.Split('/')
            $mode = ($entry.ExternalAttributes -shr 16) -band 0xF000
            if ($entry.FullName.IndexOfAny([char[]] '\:*?"<>|%') -ne -1 -or
                $mode -eq 0xA000 -or -not $paths.Add($relative)) {
                throw 'Archive contains a link, ambiguous path or duplicate entry.'
            }
            foreach ($part in $parts) {
                if ([string]::IsNullOrWhiteSpace($part) -or $part.EndsWith('.') -or
                    $part -ne $part.Trim() -or $part -match '^(CON|PRN|AUX|NUL|COM[0-9¹²³]|LPT[0-9¹²³])(?:\.|$)' -or
                    $part -match '^(DATA|SLUS_209\.10|SYSTEM\.CNF|memcard|saves|\.venv|__pycache__|eod\.ini|imgui\.ini)$') {
                    throw 'Archive contains an invalid Windows filename or private data.'
                }
            }
            if ($relative -match '\.(iso|cue|bin|elf|trk|car|res|bnk|pss|m2v|ps2|psu|mcr|mcd|sav|srm|eodproject|pyc)$' -or
                ($relative -match '(?:^|/)Mods/' -and $relative -notmatch '(?:^|/)Mods/README\.txt$')) {
                throw 'Archive contains a retail asset, private save, project or cache.'
            }
            $size += $entry.Length
            if ($size -gt 2GB) { throw 'Archive contents exceed 2 GiB.' }
        }
        [IO.Compression.ZipFileExtensions]::ExtractToDirectory($archive, $Destination)
        Write-Host "Inspected and extracted $($paths.Count) archive entries."
    } finally {
        $archive.Dispose()
    }
}

function Get-BundleRoot([string] $Destination, [string] $RequiredFile) {
    $folders = @((Get-Item -LiteralPath $Destination)) + @(Get-ChildItem -LiteralPath $Destination -Directory)
    $found = @($folders | Where-Object {
        Test-Path -LiteralPath (Join-Path $_.FullName $RequiredFile) -PathType Leaf
    })
    if ($found.Count -ne 1) { throw "Expected one package root containing $RequiredFile." }
    if ($found[0].Name -notmatch '^[A-Za-z0-9 ._-]+$') { throw 'The bundle root has an unexpected name.' }
    $found[0].FullName
}

function Assert-NativeImages([string] $Root) {
    $images = @(Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll', '.pyd' })
    if ($images.Count -eq 0) { throw "No Windows binaries found under $Root." }
    foreach ($image in $images) {
        $stream = [IO.File]::OpenRead($image.FullName)
        $reader = [IO.BinaryReader]::new($stream)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Missing DOS header in $($image.Name)." }
            $stream.Position = 0x3C
            $stream.Position = $reader.ReadUInt32()
            if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
                throw "Expected an AMD64 PE image in $($image.Name)."
            }
        } finally { $reader.Dispose() }
    }
    Write-Host "Verified AMD64 PE headers for $($images.Count) package binaries."
}

$game = if ($receipt.game) {
    $gameDirectory = Join-Path $Scratch 'g'
    Expand-VerifiedBundle $receipt.game $gameDirectory
    Assert-NativeImages $gameDirectory
    Get-BundleRoot $gameDirectory 'eod.exe'
}
$editor = if ($receipt.editor) {
    $editorDirectory = Join-Path $Scratch 'e'
    Expand-VerifiedBundle $receipt.editor $editorDirectory
    Assert-NativeImages $editorDirectory
    Get-BundleRoot $editorDirectory 'runtime\python\python.exe'
} elseif ($receipt.package_mode -eq 'combined') {
    Get-BundleRoot $gameDirectory 'runtime\python\python.exe'
}
if (-not $game -and -not $editor) { throw 'No Windows package found.' }

if ($game -and -not (Test-Path -LiteralPath (Join-Path $game 'eodsetup.exe') -PathType Leaf)) {
    throw 'Missing eodsetup.exe.'
}
if ($editor) {
    foreach ($file in @(
        'Launch EOD World Editor.bat', 'Launch EOD World Editor Console.bat', 'Launch EOD Vehicle Tools.bat',
        'EOD Tools Prompt.bat', 'tools\world_editor\app.py', 'tools\eod_mod_gui.py',
        'tools\mod_package.py', 'tools\mod_install.py', 'eod_editor_plume.dll',
        'runtime\python\pythonw.exe', 'runtime\python\python313.dll', 'runtime\python\python313._pth',
        'runtime\python\Lib\site-packages\PySide6\plugins\platforms\qoffscreen.dll',
        'runtime\python\Lib\site-packages\PySide6\plugins\platforms\qwindows.dll',
        'runtime\python\tcl\tcl8.6\init.tcl', 'runtime\python\tcl\tk8.6\tk.tcl'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $editor $file) -PathType Leaf)) { throw "Missing $file." }
    }
}

$systemRoot = $env:SystemRoot
$cmd = Join-Path $systemRoot 'System32\cmd.exe'
$profileRoot = Join-Path $Scratch 'profile'
$empty = Join-Path $Scratch 'empty'
foreach ($directory in @($empty, "$profileRoot\Local", "$profileRoot\Roaming", "$profileRoot\Temp", "$profileRoot\Editor")) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
# The game uses SHGetFolderPath for its installed-disc fallback. Confirm the runner account is clean.
if ($game -and (Test-Path -LiteralPath (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'TDEODRecomp\disc'))) {
    throw 'The Windows runner already has a game installation.'
}

$childEnvironment = @{
    SystemRoot = $systemRoot
    WINDIR = $systemRoot
    ComSpec = $cmd
    PATH = "$systemRoot\System32;$systemRoot"
    PATHEXT = '.COM;.EXE;.BAT;.CMD'
    USERPROFILE = $profileRoot
    HOME = $profileRoot
    LOCALAPPDATA = "$profileRoot\Local"
    APPDATA = "$profileRoot\Roaming"
    TEMP = "$profileRoot\Temp"
    TMP = "$profileRoot\Temp"
    EOD_EDITOR_APPDATA = "$profileRoot\Editor"
    EOD_SKIP_HOME_HUB = '1'
    EOD_HEADLESS = '1'
    EOD_MUTE = '1'
    QT_QPA_PLATFORM = 'offscreen'
    QT_OPENGL = 'software'
}
$childEnvironment.HOMEDRIVE = [IO.Path]::GetPathRoot($profileRoot).TrimEnd('\')
$childEnvironment.HOMEPATH = $profileRoot.Substring(2)

Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public class EodWindowsSmoke {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);
}
'@
# Disable loader/critical-error dialogs for package processes. Their exit codes still fail the smoke.
[EodWindowsSmoke]::SetErrorMode(0x8003) | Out-Null

function New-PackageProcess([string] $File, [string] $WorkingDirectory) {
    $info = [Diagnostics.ProcessStartInfo]::new($File)
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.Environment.Clear()
    foreach ($item in $childEnvironment.GetEnumerator()) { $info.Environment[$item.Key] = $item.Value }
    $info
}

function New-BatchProcess([string] $Name, [string] $Arguments = '') {
    $info = New-PackageProcess $cmd $empty
    $path = Join-Path $editor $Name
    $info.Arguments = "/D /C call `"$path`" $Arguments"
    $info
}

function Invoke-PackageCheck(
    [Diagnostics.ProcessStartInfo] $Info, [string] $Label, [int] $ExitCode, [string] $Expected,
    [string] $InputText = ''
) {
    $Info.RedirectStandardOutput = $true
    $Info.RedirectStandardError = $true
    $Info.RedirectStandardInput = $true
    $Info.CreateNoWindow = $true
    $process = [Diagnostics.Process]::Start($Info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if ($InputText) { $process.StandardInput.Write($InputText) }
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(35000)) { throw "$Label timed out." }
        if (-not $stdout.Wait(3000) -or -not $stderr.Wait(3000)) {
            throw "$Label left its stdout or stderr handles open."
        }
        $output = $stdout.Result + "`n" + $stderr.Result
        if ($process.ExitCode -ne $ExitCode -or $output -notmatch $Expected) {
            throw "$Label failed (exit $($process.ExitCode)). Output: $output"
        }
        Write-Host "PASS $Label (exit $ExitCode)"
        if ($output.Trim()) { Write-Host $output.Trim() }
    } finally {
        if (-not $process.HasExited) { $process.Kill($true); [void] $process.WaitForExit(5000) }
        $process.Dispose()
    }
}

function Wait-ForMainWindow([Diagnostics.Process] $Process, [string] $Title) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        $Process.Refresh()
        if ($Process.HasExited) { throw "Expected window '$Title', but process exited with $($Process.ExitCode)." }
        if ($Process.MainWindowHandle -ne 0 -and $Process.MainWindowTitle -eq $Title) { return }
        Start-Sleep -Milliseconds 150
    } while ($clock.Elapsed.TotalSeconds -lt 20)
    throw "Timed out waiting for window '$Title'."
}

function Close-PackageWindow([Diagnostics.Process] $Process, [string] $Label) {
    if (-not $Process.CloseMainWindow() -or -not $Process.WaitForExit(8000) -or $Process.ExitCode -ne 0) {
        throw "$Label did not close cleanly."
    }
    Write-Host "PASS $Label window opened and closed (exit 0)"
}

function Invoke-DetachedLauncher([string] $Name, [scriptblock] $Check) {
    $starter = [Diagnostics.Process]::Start((New-BatchProcess $Name))
    try {
        if (-not $starter.WaitForExit(5000) -or $starter.ExitCode -ne 0) {
            throw "Batch launcher failed: $Name."
        }
        $clock = [Diagnostics.Stopwatch]::StartNew()
        do {
            $matches = @(Get-Process -Name 'pythonw' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $pythonw })
            if ($matches.Count -gt 1) { throw "Multiple bundled pythonw processes for $Name." }
            if ($matches.Count -eq 1) {
                & $Check $matches[0]
                return
            }
            Start-Sleep -Milliseconds 150
        } while ($clock.Elapsed.TotalSeconds -lt 12)
        throw "No bundled pythonw process started for $Name."
    } finally {
        if (-not $starter.HasExited) { $starter.Kill($true); [void] $starter.WaitForExit(5000) }
        $starter.Dispose()
        Get-Process -Name 'pythonw' -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $pythonw } |
            ForEach-Object {
                if (-not $_.HasExited) { $_.Kill($true); [void] $_.WaitForExit(5000) }
                $_.Dispose()
            }
    }
}

if ($game) {
    $gameInfo = New-PackageProcess (Join-Path $game 'eod.exe') $empty
    $gameInfo.ArgumentList.Add('--help')
    Invoke-PackageCheck $gameInfo 'eod.exe --help' 0 '(?m)^usage:.+--data DIR.+--renderer'

    $gameInfo = New-PackageProcess (Join-Path $game 'eod.exe') $empty
    foreach ($argument in @('--headless', '--renderer', 'software', '--config', "$empty\empty.ini")) {
        $gameInfo.ArgumentList.Add($argument)
    }
    Invoke-PackageCheck $gameInfo 'eod.exe clean missing-disc startup' 1 'Choose a game ISO or pass --data <image>\.'

    $setup = [Diagnostics.Process]::Start((New-PackageProcess (Join-Path $game 'eodsetup.exe') $empty))
    try {
        Wait-ForMainWindow $setup 'Eve of Destruction - Setup'
        Close-PackageWindow $setup 'eodsetup.exe'
    } finally {
        if (-not $setup.HasExited) { $setup.Kill($true); [void] $setup.WaitForExit(5000) }
        $setup.Dispose()
    }
}

if ($editor) {
    Invoke-PackageCheck (New-BatchProcess 'Launch EOD World Editor Console.bat' '--help') `
        'bundled World Editor console launcher' 0 'EOD World Editor \(Qt 6.*OpenGL'

    $pythonw = Join-Path $editor 'runtime\python\pythonw.exe'
    Invoke-DetachedLauncher 'Launch EOD World Editor.bat' {
        param($Process)
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $plugin = Join-Path $editor 'runtime\python\Lib\site-packages\PySide6\plugins\platforms\qoffscreen.dll'
        do {
            $Process.Refresh()
            if ($Process.HasExited) { throw "World Editor launcher exited with $($Process.ExitCode)." }
            if (@($Process.Modules | Where-Object { $_.FileName -eq $plugin }).Count -eq 1) {
                Write-Host 'PASS detached World Editor launcher loaded the shipped Qt offscreen platform DLL'
                return
            }
            Start-Sleep -Milliseconds 150
        } while ($clock.Elapsed.TotalSeconds -lt 20)
        throw 'World Editor launcher did not load its bundled Qt offscreen platform DLL.'
    }

    $python = Join-Path $editor 'runtime\python\python.exe'
    $editorInfo = New-PackageProcess $python $empty
    foreach ($argument in @('-I', '-B', (Join-Path $PSScriptRoot 'probe-editor.py'), $editor, $empty)) {
        $editorInfo.ArgumentList.Add($argument)
    }
    Invoke-PackageCheck $editorInfo 'bundled World Editor main window and native imports' 0 'PASS bundled World Editor window'

    Invoke-DetachedLauncher 'Launch EOD Vehicle Tools.bat' {
        param($Process)
        Wait-ForMainWindow $Process 'Test Drive: Eve of Destruction - Modding Studio'
        Close-PackageWindow $Process 'bundled Vehicle Tools launcher'
    }

    $commands = "python -I -B tools\mod_package.py --help && python -I -B tools\mod_install.py --help && echo EOD_TOOLS_PROMPT_OK`r`nexit`r`n"
    Invoke-PackageCheck (New-BatchProcess 'EOD Tools Prompt.bat') 'bundled Tools Prompt and CLI help' 0 '(?m)^EOD_TOOLS_PROMPT_OK\r?$' $commands
}

$summary = @"
### Windows x64 package smoke passed

- Release: $($receipt.tag) (ID $($receipt.release_id))
- Source: $($receipt.source_commit)
- Workflow: $($receipt.workflow_commit), run $($receipt.run_id), attempt $($receipt.run_attempt)
- Runner: $($system.Caption), build $($system.BuildNumber), native x64, image $env:ImageOS $env:ImageVersion
"@
if ($receipt.game) {
    $summary += "`n- Game or combined asset: $($receipt.game.name) (ID $($receipt.game.id), $($receipt.game.bytes) bytes), SHA-256 $($receipt.game.sha256)"
}
if ($receipt.editor) {
    $summary += "`n- Editor asset: $($receipt.editor.name) (ID $($receipt.editor.id), $($receipt.editor.bytes) bytes), SHA-256 $($receipt.editor.sha256)"
}
if ($game) {
    $summary += "`n- Game checks: CLI help, clean missing-disc exit and setup window."
}
if ($editor) {
    $summary += "`n- Editor checks: console and GUI launchers, native runtime imports, offscreen main window, synthetic project round-trip, Vehicle Tools GUI and mod utilities."
}
$summary | Write-Host
if ($env:GITHUB_STEP_SUMMARY) { Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value $summary -Encoding utf8 }
