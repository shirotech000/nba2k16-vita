# windows wrapper, just runs build.sh in WSL
param([switch]$Clean)
$repo = wsl -e wslpath -a ($PSScriptRoot -replace '\\','/')
$arg = if ($Clean) { 'clean' } else { '' }
wsl -e bash -lc "cd '$repo' && bash ./build.sh $arg"
exit $LASTEXITCODE
