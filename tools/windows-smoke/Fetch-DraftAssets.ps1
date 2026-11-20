# Run only in the workflow fetch step. This script never executes archive contents.
$ErrorActionPreference = 'Stop'

if ($env:GITHUB_REPOSITORY -cne 'catsandsoup/eod-recomp' -or
    $env:GITHUB_API_URL -cne 'https://api.github.com' -or
    [string]::IsNullOrEmpty($env:EOD_DRAFT_TOKEN)) {
    throw 'Draft download requires the catsandsoup/eod-recomp GitHub Actions token.'
}
if ($env:EOD_RELEASE_ID -notmatch '^[1-9][0-9]{0,17}$' -or
    $env:EOD_RELEASE_TAG -cnotmatch '^v[0-9][A-Za-z0-9._-]{1,99}$' -or
    $env:EOD_SOURCE_SHA -cnotmatch '^[0-9a-f]{40}$') {
    throw 'Supply the exact numeric release ID, version tag and full target commit SHA.'
}
if ([string]::IsNullOrEmpty($env:EOD_GAME_ASSET) -ne
    [string]::IsNullOrEmpty($env:EOD_GAME_SHA256)) {
    throw 'A game or combined bundle requires both an exact asset name and its SHA-256.'
}
if ([string]::IsNullOrEmpty($env:EOD_EDITOR_ASSET) -ne
    [string]::IsNullOrEmpty($env:EOD_EDITOR_SHA256)) {
    throw 'A separate editor requires both an exact asset name and its SHA-256.'
}

$hasGame = -not [string]::IsNullOrEmpty($env:EOD_GAME_ASSET)
$hasEditor = -not [string]::IsNullOrEmpty($env:EOD_EDITOR_ASSET)
$validMode = switch -CaseSensitive ($env:EOD_PACKAGE_MODE) {
    'combined' { $hasGame -and -not $hasEditor }
    'separate' { $hasGame -and $hasEditor }
    'game-only' { $hasGame -and -not $hasEditor }
    'editor-only' { $hasEditor -and -not $hasGame }
    default { $false }
}
if (-not $validMode) { throw 'The package mode does not match the supplied Windows archive inputs.' }

function Assert-AssetInput([string] $Name, [string] $Sha256) {
    if ($Name -cnotmatch '^EOD-[A-Za-z0-9._-]{1,155}-Windows-x64\.zip$' -or
        $Name.Contains('..') -or $Sha256 -notmatch '^[a-f0-9]{64}$') {
        throw 'Supply the exact Windows x64 ZIP basename and its SHA-256.'
    }
}

if ($env:EOD_GAME_ASSET) { Assert-AssetInput $env:EOD_GAME_ASSET $env:EOD_GAME_SHA256 }
if ($env:EOD_EDITOR_ASSET) {
    Assert-AssetInput $env:EOD_EDITOR_ASSET $env:EOD_EDITOR_SHA256
    if ($env:EOD_GAME_ASSET -eq $env:EOD_EDITOR_ASSET) {
        throw 'Use a single asset input for a combined ZIP.'
    }
}

$repoApi = 'https://api.github.com/repos/catsandsoup/eod-recomp'
$headers = @{
    Authorization = "Bearer $env:EOD_DRAFT_TOKEN"
    Accept = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}
try {
    $release = Invoke-RestMethod -Uri "$repoApi/releases/$env:EOD_RELEASE_ID" -Headers $headers -TimeoutSec 30
} catch {
    $status = if ($_.Exception.Response.StatusCode) { " (HTTP $([int] $_.Exception.Response.StatusCode))" }
    throw "Could not access the specified draft release$status. Check its ID and the workflow contents permission."
}
if ($release.id.ToString() -cne $env:EOD_RELEASE_ID -or
    $release.tag_name -cne $env:EOD_RELEASE_TAG -or $release.draft -ne $true -or $release.prerelease -ne $true) {
    throw 'The release ID and exact tag must identify an unpublished prerelease draft.'
}
try {
    # Resolve the pre-created ref to its commit, including annotated tags.
    $commit = Invoke-RestMethod -Uri "$repoApi/commits/refs/tags/$env:EOD_RELEASE_TAG" -Headers $headers -TimeoutSec 30
} catch {
    throw 'Could not resolve the pre-created release tag to its commit.'
}
if ($commit.sha -cne $env:EOD_SOURCE_SHA) { throw 'The release tag does not match the requested source commit.' }

$scratch = Join-Path $env:RUNNER_TEMP "eod-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT"
New-Item -Path $scratch -ItemType Directory | Out-Null

function Get-VerifiedAsset([string] $Name, [string] $Sha256, [string] $FileName) {
    $selection = @($release.assets | Where-Object { $_.name -ceq $Name })
    if ($selection.Count -ne 1 -or $selection[0].state -ne 'uploaded' -or
        $selection[0].size -le 0 -or $selection[0].size -gt 1GB) {
        throw "Expected a single uploaded Windows archive under 1 GiB for $Name."
    }
    $asset = $selection[0]
    if ($asset.digest -and $asset.digest -ne "sha256:$Sha256") {
        throw "GitHub's asset digest disagrees with the expected SHA-256 for $Name."
    }
    $zip = Join-Path $scratch $FileName
    $headers.Accept = 'application/octet-stream'
    try {
        # PowerShell drops the Authorization header on redirects by default.
        Invoke-WebRequest -Uri "$repoApi/releases/assets/$($asset.id)" -Headers $headers -OutFile $zip `
            -MaximumRedirection 2 -MaximumRetryCount 2 -RetryIntervalSec 3 -TimeoutSec 90 -ProgressAction SilentlyContinue
    } catch {
        $status = if ($_.Exception.Response.StatusCode) { " (HTTP $([int] $_.Exception.Response.StatusCode))" }
        throw "The authenticated draft asset download failed for $Name$status."
    }
    if ((Get-Item -LiteralPath $zip).Length -ne $asset.size -or
        (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $Sha256) {
        throw "Downloaded bytes disagreed with the expected size or SHA-256 for $Name."
    }
    Write-Host ("Verified {0} ({1} bytes) SHA-256 {2}" -f $Name, $asset.size, $Sha256.ToLowerInvariant())
    [ordered]@{
        name = $Name
        id = $asset.id
        bytes = $asset.size
        sha256 = $Sha256.ToLowerInvariant()
        archive = $FileName
    }
}

$game = if ($env:EOD_GAME_ASSET) {
    Get-VerifiedAsset $env:EOD_GAME_ASSET $env:EOD_GAME_SHA256 'game.zip'
} else { $null }
$editor = if ($env:EOD_EDITOR_ASSET) {
    Get-VerifiedAsset $env:EOD_EDITOR_ASSET $env:EOD_EDITOR_SHA256 'editor.zip'
} else { $null }

[ordered]@{
    repository = $env:GITHUB_REPOSITORY
    tag = $env:EOD_RELEASE_TAG
    release_id = $release.id
    source_commit = $env:EOD_SOURCE_SHA
    package_mode = $env:EOD_PACKAGE_MODE
    workflow_commit = $env:GITHUB_SHA
    run_id = $env:GITHUB_RUN_ID
    run_attempt = $env:GITHUB_RUN_ATTEMPT
    game = $game
    editor = $editor
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $scratch 'receipt.json') -Encoding utf8
