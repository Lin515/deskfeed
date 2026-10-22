#Requires -Version 7.0
<#
.SYNOPSIS
    打包 DeskFeed 的 x64 Release 资产。

.DESCRIPTION
    从构建目录取出三个交付二进制（deskfeed.exe、deskfeed-payload.dll、deskfeed-inject.exe），
    确认它们都是 AMD64 镜像，连同 README.md 与 LICENSE 一起压成
    deskfeed-<tag>-x64.zip，并生成 SHA256SUMS.txt。

    发布流程里由 .github/workflows/release.yml 调用；也可以在本地对一份 Release 构建手工运行，
    用来复现 CI 的打包结果。

.PARAMETER Tag
    版本 tag，例如 v1.2.3 或 v1.2.3-rc.1。zip 文件名直接使用它。

.PARAMETER BuildDir
    Release 构建目录，二进制应位于其下的 bin/。相对路径按仓库根解析。

.PARAMETER DistDir
    输出目录。脚本会先清空它，再写入 zip 与校验文件。相对路径按仓库根解析。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Tag,

    [string] $BuildDir = 'build',

    [string] $DistDir = 'dist'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -PathType Leaf)) {
    throw "cannot locate the repository root from '$PSScriptRoot'"
}

$separator = [System.IO.Path]::DirectorySeparatorChar

function Trim-PathEnd([string] $Path) {
    $root = [System.IO.Path]::GetPathRoot($Path)
    if ($Path.Length -le $root.Length) {
        return $Path
    }
    return $Path.TrimEnd([char[]]@('\', '/'))
}

function Resolve-RepoPath([string] $Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return Trim-PathEnd ([System.IO.Path]::GetFullPath($Path))
    }
    return Trim-PathEnd ([System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path)))
}

function Test-SamePath([string] $Left, [string] $Right) {
    return [string]::Equals($Left, $Right, [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-PathWithin([string] $Child, [string] $Parent) {
    return $Child.StartsWith($Parent + $separator, [System.StringComparison]::OrdinalIgnoreCase)
}

$repoRoot = Trim-PathEnd $repoRoot
$build = Resolve-RepoPath $BuildDir
$dist = Resolve-RepoPath $DistDir
$bin = Join-Path $build 'bin'

# 版本 tag 同时是 zip 文件名的一部分，先拒绝不能安全落盘的形式。
$tagPattern = '^v?\d+\.\d+\.\d+(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?(?:\+[0-9A-Za-z][0-9A-Za-z.-]*)?$'
if ($Tag -notmatch $tagPattern) {
    throw "invalid release tag '$Tag'; expected vMAJOR.MINOR.PATCH with an optional suffix"
}
if ($Tag.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) {
    throw "release tag '$Tag' contains characters that are not valid in a file name"
}

# DistDir 会被整个删除重建。拒绝任何可能删到仓库、盘根或构建目录的取值。
# Windows 路径不区分大小写，比较也必须用 OrdinalIgnoreCase，不能用 -eq。
$distRoot = [System.IO.Path]::GetPathRoot($dist)
if ((Test-SamePath $dist $repoRoot) -or (Test-SamePath $dist $distRoot) -or
    (Test-PathWithin $repoRoot $dist) -or
    (Test-SamePath $build $dist) -or (Test-PathWithin $build $dist)) {
    throw "refusing to use '$dist' as the dist directory"
}

if (-not (Test-Path -LiteralPath $bin -PathType Container)) {
    throw "build output directory not found: $bin"
}

function Get-PeMachine {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40) {
        throw "$Path is not a PE file (file is too small)"
    }
    if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$Path is not a PE file (missing MZ signature)"
    }

    $peOffset = [System.BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or ($peOffset + 6) -gt $bytes.Length) {
        throw "$Path is not a PE file (invalid e_lfanew)"
    }
    if ($bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0x00 -or $bytes[$peOffset + 3] -ne 0x00) {
        throw "$Path is not a PE file (missing PE signature)"
    }

    return [System.BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

$binaries = @('deskfeed.exe', 'deskfeed-payload.dll', 'deskfeed-inject.exe')
$documents = @('README.md', 'LICENSE')
$stageName = "deskfeed-$Tag-x64"
$stage = Join-Path $dist $stageName

if (Test-Path -LiteralPath $dist) {
    Remove-Item -LiteralPath $dist -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

foreach ($name in $binaries) {
    $source = Join-Path $bin $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "missing build output: $source"
    }

    # 发布包只可能是 x64：任务栏负载必须与 64 位 explorer.exe 同位数。
    $machine = Get-PeMachine $source
    if ($machine -ne 0x8664) {
        throw ("{0} is not an AMD64 image (PE machine 0x{1:X4})" -f $name, $machine)
    }

    Copy-Item -LiteralPath $source -Destination $stage
}

foreach ($name in $documents) {
    $source = Join-Path $repoRoot $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "missing release document: $source"
    }
    Copy-Item -LiteralPath $source -Destination $stage
}

$zipPath = Join-Path $dist "deskfeed-$Tag-x64.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal

# 压缩包内容就是交付边界：三个二进制加两份说明文件，不多也不少。
$expected = @($binaries) + @($documents)
$archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $entries = @($archive.Entries | ForEach-Object { $_.FullName })
} finally {
    $archive.Dispose()
}
foreach ($name in $expected) {
    if ($entries -notcontains $name) {
        throw "release archive '$zipPath' is missing '$name'"
    }
}

$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksums = Join-Path $dist 'SHA256SUMS.txt'
# 固定 LF 与 ASCII：Linux 上的 sha256sum -c 也能直接校验。
[System.IO.File]::WriteAllText(
    $checksums,
    "$hash  $(Split-Path -Leaf $zipPath)`n",
    [System.Text.Encoding]::ASCII)

if (-not [string]::IsNullOrEmpty($env:GITHUB_OUTPUT)) {
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "zip=$zipPath"
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "checksums=$checksums"
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "sha256=$hash"
}

Write-Host "packaged: $zipPath"
Write-Host "sha256:   $hash"
