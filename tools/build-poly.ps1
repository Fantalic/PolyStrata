# build-poly.ps1
#
# Builds strata-poly incrementally with the release recipe flags and deploys the
# result to engine-poly\strata-poly.exe.
#
# Why the release flags matter: a build with the bare defaults (unpinned ggml
# submodule, non-portable CPU code) measured decode at 20.0 tok/s on this
# machine. The release recipe (Release + Ninja + portable build + pinned ggml
# + CUDA arch 86-real) measures 45.5 tok/s — more than 2x faster. Those flags
# are mandatory for acceptable performance on this PC (RTX 30-class, 2026-11-14).
#
# Switches:
#   -Configure   Reconfigure the CMake cache with the exact release flags, then
#                build and deploy (useful after a branch switch or cache stale).
#   -Tests       Also build the four offline conversation/spill tests:
#                glm_conversations_test, spill_test, conversation_memory_test,
#                poly_guesses_test.
#   -NoDeploy    Build only — do not copy the exe to engine-poly\.
#   -Jobs <int>  Parallelism, default half the cores (at least 2).

param(
    [string]$Vcvars    = 'C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
    [string]$Cmake     = 'C:\Python310\Scripts\cmake.exe',
    [string]$Ninja     = 'C:\Python310\Scripts\ninja.exe',
    [switch]$Configure,
    [switch]$Tests,
    [switch]$NoDeploy,
    [int]$Jobs          = [Math]::Max(2, [Environment]::ProcessorCount / 2)
)

$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
$BuildDir = Join-Path $Repo 'build-verify'
$Exe = Join-Path $BuildDir 'strata-poly.exe'
$Deploy = Join-Path $Repo 'engine-poly\strata-poly.exe'

function Invoke-With-Exit {
    param([scriptblock]$Block)
    & $Block
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

# Verify toolchain files exist before doing anything.
foreach ($f in @($Vcvars, $Cmake)) {
    if (-not (Test-Path $f)) {
        Write-Error "Toolchain not found: $f"
        exit 1
    }
}
if ($Ninja -and -not (Test-Path $Ninja)) {
    Write-Error "Ninja not found: $Ninja"
    exit 1
}

# -Configure: reconfigure the CMake cache with the release recipe flags.
if ($Configure) {
    $TestFlag = if ($Tests) { '-DSTRATA_BUILD_CONVERSATION_TESTS=ON' } else { '' }
    $cmd = "$Cmake -G Ninja -DCMAKE_MAKE_PROGRAM=`"$Ninja`" -S `"$Repo`" -B `"$BuildDir`" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=86-real -DCMAKE_CUDA_COMPILER=`"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin\nvcc.exe`" -DSTRATA_PORTABLE=ON -DSTRATA_GGML_DIR=`"$Repo\third_party\llama.cpp`" -DSTRATA_BUILD_TESTS=OFF $TestFlag"
    Write-Host 'Configuring:' $cmd
    Invoke-With-Exit { cmd.exe /s /c $cmd }
}

# Build strata-poly (incremental — CMake / Ninja skip unchanged files).
$targets = @('--target', 'strata-poly')
if ($Tests) {
    $targets += '--target', 'glm_conversations_test', '--target', 'spill_test',
                '--target', 'conversation_memory_test', '--target', 'poly_guesses_test'
}
$cmd = "cmd /c `"`"$Vcvars`" >nul && `"$Cmake`" --build `"$BuildDir`" -j $Jobs $($targets -join ' ')`""
Write-Host 'Building:' $cmd
Invoke-With-Exit { cmd.exe /s /c $cmd }

# Verify the exe exists after build.
if (-not (Test-Path $Exe)) {
    Write-Error "Build output not found: $Exe"
    exit 1
}

# Deploy unless -NoDeploy. The copy fails while the engine is running (the exe
# is locked), which is the signal to stop the server first, not to force it.
if (-not $NoDeploy) {
    if (Test-Path $Deploy) {
        $bak = Join-Path (Get-Item $Deploy).Directory 'strata-poly.prev.exe'
        Copy-Item $Deploy $bak -Force
        Write-Host "Backed up: $bak"
    }
    Copy-Item $Exe $Deploy -Force
    $new = Get-Item $Deploy
    Write-Host "Deployed: $Deploy"
    Write-Host ('  Size:    {0:N0} bytes' -f $new.Length)
    Write-Host ('  Written: {0}' -f $new.LastWriteTime)
}
