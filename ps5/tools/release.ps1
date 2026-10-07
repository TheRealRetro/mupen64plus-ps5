# release.ps1 - publishes a Mupen64Plus PS5 release on GitHub (run from Windows: release.bat).
#
#   1. builds the app from scratch (build-native.bat Clean): build-native\PPSA99064\ and Mupen64PlusPS5.zip. Always
#      clean: 0.6.1 was published from an incremental build that didn't start on the console;
#   2. checks that everything is committed and pushed, that release-notes\<contentVersion>.md exists and that
#      the tag isn't taken;
#   3. creates the GitHub release: tag = contentVersion (NN.NNN.NNN, from VERSION in ps5\Makefile), asset
#      Mupen64PlusPS5.zip, notes from release-notes\<contentVersion>.md (the app shows them when it offers the
#      update);
#   4. reads the release back and compares GitHub's SHA-256 of the asset with the local zip (the app checks
#      the download against that digest).
#
# Needs the GitHub CLI, signed in (gh auth login). Never reuse or lower a version: the app offers a release
# only when its tag is higher than its own contentVersion.
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$repo = 'TheRealRetro/mupen64plus-ps5'
$title = 'PPSA99064'
$asset = 'Mupen64PlusPS5.zip' # RELEASE_ZIP in ps5\Makefile; PPSA99064.zip up to 0.6.2
$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh) { $gh = 'C:\Program Files\GitHub CLI\gh.exe' }
if (-not (Test-Path $gh)) { throw 'GitHub CLI not found: winget install GitHub.cli, then gh auth login' }

Push-Location $root
try {
	& cmd /c "`"$root\build-native.bat`" Clean"
	if ($LASTEXITCODE -ne 0) { throw 'build failed' }
	$zip = Join-Path $root "build-native\$asset"
	$param = Get-Content (Join-Path $root "build-native\$title\sce_sys\param.json") -Raw | ConvertFrom-Json
	$cv = $param.contentVersion
	if ($cv -notmatch '^\d{2}\.\d{3}\.\d{3}$') { throw "contentVersion '$cv' is not NN.NNN.NNN" }
	$parts = $cv.Split('.') | ForEach-Object { [int]$_ }
	$version = "$($parts[0]).$($parts[1]).$($parts[2])"

	$notes = Join-Path $root "release-notes\$cv.md"
	if (-not (Test-Path $notes)) { throw "write the release notes first: release-notes\$cv.md" }

	$dirty = git status --porcelain
	if ($dirty) { throw "commit your changes first:`n$dirty" }
	git fetch -q origin
	$ahead = git rev-list --count origin/main..HEAD
	if ([int]$ahead -gt 0) { throw "push your commits first (git push): $ahead not on GitHub" }

	# "release not found" on stderr is the expected answer: PowerShell 5.1 would make it fatal under 'Stop'
	$ErrorActionPreference = 'Continue'
	& $gh release view $cv --repo $repo 2>&1 | Out-Null
	$exists = $LASTEXITCODE -eq 0
	$ErrorActionPreference = 'Stop'
	if ($exists) { throw "release $cv already exists: raise VERSION in ps5\Makefile" }

	Write-Host "Publishing Mupen64Plus PS5 $version (tag $cv) to $repo"
	& $gh release create $cv $zip --repo $repo --target main --title "Mupen64Plus PS5 $version" --notes-file $notes
	if ($LASTEXITCODE -ne 0) { throw 'gh release create failed' }

	$local = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
	# parsed here: PowerShell 5.1 strips the quotes a --jq filter would need
	$release = (& $gh api "repos/$repo/releases/tags/$cv") -join "`n" | ConvertFrom-Json
	$remote = ($release.assets | Where-Object { $_.name -eq $asset }).digest
	if ($remote -ne "sha256:$local") { throw "GitHub's digest '$remote' doesn't match the local zip (sha256:$local)" }
	Write-Host "Done: https://github.com/$repo/releases/tag/$cv (sha256 $local)"
	Write-Host 'Consoles running an older version will offer it the next time Mupen64Plus PS5 starts.'
}
finally { Pop-Location }
