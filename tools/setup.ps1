# One-click setup for the Red Alert 2 and Yuri's Revenge static recompilation.
# Run it by double-clicking Setup.cmd in the repo folder; the README's "Step by
# step" is the same thing by hand, command for command.
#
# It copies your install into game\, then for each game (Yuri's Revenge,
# gamemd.exe, and Red Alert 2, game.exe) analyses the exe, builds the function
# catalog, lifts it to C, builds the 32-bit host and leaves a shortcut. Each
# step is skipped when its output already exists (-Force redoes them).
# -Games yr or -Games ra2 builds only that one. Everything it does goes to
# setup.log.
param([switch]$Force, [string]$Game = "", [ValidateSet('both', 'yr', 'ra2')][string]$Games = 'both')

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = Join-Path (Split-Path -Parent $Root) 'tools'
$T = Join-Path $Toolkit 'tools'
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
$Targets = @(
  @{ Key = 'yr'; Name = "Yuri's Revenge"; Exe = 'gamemd.exe'; Target = 'gamemd'; Work = 'work'; Gen = 'src\recomp\gen'; Build = 'build' },
  @{ Key = 'ra2'; Name = 'Red Alert 2'; Exe = 'game.exe'; Target = 'game'; Work = 'work\game'; Gen = 'src\recomp\gen_game'; Build = 'build-game' }
) | Where-Object { $Games -eq 'both' -or $_.Key -eq $Games }
$Targets = @($Targets)   # one match would be a bare hashtable
$Steps = 3 + 4 * @($Targets).Count
# A log line that cannot be written (the log open in another program) is
# not a reason to stop the setup.
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Step($n, $t) { Write-Host ""; Say "[$n/$Steps] $t" 'Cyan' }
function Fail($t) {
  Say "" ; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Read-Host "Press Enter to close" | Out-Null; exit 1
}
function Ask($q) { $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }   # Enter = yes
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)   # @(): a one-element slice would splat as characters
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}

Clear-Host
Say "Red Alert 2 and Yuri's Revenge - static recompilation setup" 'White'
Say "Building: $(($Targets | ForEach-Object { $_.Name }) -join ' and ')"
Say "You need your installed copy of Red Alert 2 and Yuri's Revenge (the Steam build) and about 6 GB free."
Say "The function catalog, the lift and the build take 30 to 60 minutes per game, once."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools the pipeline needs"
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  if (-not (Ask "  Install Python 3.12 now (winget, for your user only, about 30 MB)?")) { Fail "Python 3 is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install Python yourself."
  }
  Exec @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--scope', 'user', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
Say "  Python: $($pyargs -join ' ')"

if ((Exec ($pyargs + @('-c', 'import pefile, capstone'))) -ne 0) {
  Say "  The Python packages pefile and capstone are missing (about 15 MB)."
  if (-not (Ask "  Install them now (pip, for your user only)?")) { Fail "pefile and capstone are required." }
  Run "Installing pefile and capstone" ($pyargs + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone'))
}

if (-not (Test-Path (Join-Path $T 'lift\lift32.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed to fetch pcrecomp. Install Git for Windows, then run Setup.cmd again." }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
if (-not (Test-Path (Join-Path $Toolkit 'runtime\native32\native32.c'))) {
  Fail "your pcrecomp at $Toolkit predates runtime\native32. Update it: git -C `"$Toolkit`" pull"
}
Say "  pcrecomp: $Toolkit"

# The compiler. build.cmd finds Visual Studio itself; this only checks that it
# is there, with CMake and Ninja. Visual Studio is several GB with an installer
# of its own, so this says what to install rather than installing it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
if (-not $vs) { Fail "Visual Studio 2022 (or its Build Tools) with 'Desktop development with C++' is needed. Install it, then run Setup.cmd again." }
Say "  Visual Studio: $vs"
# $tool, not $t: PowerShell names are case-insensitive, and $T is the toolkit path.
foreach ($tool in @(@('cmake', 'Kitware.CMake', '30 MB'), @('ninja', 'Ninja-build.Ninja', '1 MB'))) {
  if (Get-Command $tool[0] -ErrorAction SilentlyContinue) { continue }
  Say "  $($tool[0]) is not installed."
  if (-not (Ask "  Install $($tool[0]) now (winget, about $($tool[2]))?")) { Fail "$($tool[0]) is required." }
  Exec @('winget', 'install', '-e', '--id', $tool[1], '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  if (-not (Get-Command $tool[0] -ErrorAction SilentlyContinue)) { Fail "$($tool[0]) installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}

# ---------------------------------------------------------------- the game
Step 2 "Copying your copy of Red Alert 2 into game\"
function Is-Install([string]$d) { return ($d -and -not ($Targets | Where-Object { -not (Test-Path (Join-Path $d $_.Exe)) })) }
function Find-Steam {
  $roots = @()
  foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
    try { $roots += (Get-ItemProperty $k -ErrorAction Stop).SteamPath } catch {}
    try { $roots += (Get-ItemProperty $k -ErrorAction Stop).InstallPath } catch {}
  }
  foreach ($r in ($roots | Where-Object { $_ } | Select-Object -Unique)) {
    $libs = @($r)
    $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
      $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
    }
    foreach ($l in $libs) {
      $d = Join-Path $l 'steamapps\common\Command & Conquer Red Alert II'
      if (Is-Install $d) { return $d }
    }
  }
  return ""
}

if ((Is-Install 'game') -and -not $Force) {
  Say "  Already in game\ (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not (Is-Install $Game)) {
    $Game = Find-Steam
    if ($Game) { Say "  Found it in your Steam library: $Game" }
  }
  while (-not (Is-Install $Game)) {
    $Game = (Read-Host "  Paste the folder Red Alert 2 is installed in (the one with gamemd.exe and game.exe)").Trim('"', ' ')
    if (-not (Is-Install $Game)) { Say "  No $(($Targets | ForEach-Object { $_.Exe }) -join ' and ') in that folder." 'Yellow' }
  }
  Say "  Copying $Game (about 1.9 GB)..."
  $code = Exec @('robocopy', $Game, (Join-Path $Root 'game'), '/E', '/NFL', '/NDL', '/NJH', '/NP')
  if ($code -ge 8) { Fail "copying the game failed (robocopy exit code $code)." }   # robocopy: <8 is success
}

# ---------------------------------------------------------------- each game
# Analyse, catalog, lift and build, one game at a time, so the first is
# playable while the second builds.
$n = 3
foreach ($g in $Targets) {
  $exe = "game\$($g.Exe)"
  Step $n "$($g.Name): analysing $($g.Exe) (seconds)"; $n++
  New-Item -ItemType Directory -Force $g.Work | Out-Null
  if ((Test-Path "$($g.Work)\rtti_seeds.json") -and -not $Force) { Say "  Already done (skipping)." }
  else {
    Run "Headers and imports" ($pyargs + @("$T\pe\pe_analyze.py", $exe, '--json', "$($g.Work)\pe_analysis.json"))
    Run "C++ classes from RTTI" ($pyargs + @("$T\cpp\rtti.py", $exe, '-o', "$($g.Work)\rtti.json", '--seeds', "$($g.Work)\rtti_seeds.json"))
  }

  Step $n "$($g.Name): finding every function (about 15 minutes, once)"; $n++
  if ((Test-Path "$($g.Work)\functions.json") -and -not $Force) { Say "  Already done (skipping)." }
  else {
    Run "Disassembling" ($pyargs + @("$T\disasm\disasm32.py", $exe, '-o', "$($g.Work)\functions.json", '--seed-functions', "$($g.Work)\rtti_seeds.json"))
  }

  Step $n "$($g.Name): lifting to C (5 to 15 minutes)"; $n++
  if ((Test-Path "$($g.Gen)\recomp_dispatch.c") -and -not $Force) { Say "  Already done (skipping)." }
  else {
    $env:PCRECOMP = $Toolkit
    Run "Lifting" ($pyargs + @('run_lift.py', '--all', '--target', $g.Target))
  }

  Step $n "$($g.Name): building $($g.Build)\ra2.exe (10 to 30 minutes)"; $n++
  if ((Test-Path "$($g.Build)\ra2.exe") -and -not $Force) { Say "  Already done (skipping)." }
  else {
    $env:BUILD_DIR = $g.Build; $env:CMAKE_ARGS = "-DRA2_TARGET=$($g.Target)"
    $buildCmd = Join-Path $Root 'build.cmd'
    Run "Compiling" @('cmd', '/c', ('call "' + $buildCmd + '"'))
  }
}

# ---------------------------------------------------------------- shortcuts
Step $n "Making the shortcuts"
foreach ($g in $Targets) {
  $lnk = Join-Path $Root "$($g.Name) (recomp).cmd"
  Set-Content -Path $lnk -Encoding ASCII -Value @(
    '@echo off',
    'rem Plays the recompiled game in its own window: settings F10, scaling F12, fullscreen F11.',
    'cd /d "%~dp0"',
    "start `"`" $($g.Build)\ra2.exe --run")
  Say "  $lnk"
}

Write-Host ""
$names = ($Targets | ForEach-Object { "'$($_.Name) (recomp).cmd'" }) -join ' or '
Say "Done. Double-click $names to play." 'Green'
Say "F10 opens the settings (scaling, fullscreen, HD vehicles, the game's resolution)."
Read-Host "Press Enter to close" | Out-Null
