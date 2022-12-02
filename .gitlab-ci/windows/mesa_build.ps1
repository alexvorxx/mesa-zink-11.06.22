# Clear CI_COMMIT_MESSAGE and CI_COMMIT_DESCRIPTION for please meson
# when the commit message is complicated
$env:CI_COMMIT_MESSAGE=""
$env:CI_COMMIT_DESCRIPTION=""

# force the CA cert cache to be rebuilt, in case Meson tries to access anything
Write-Host "Refreshing Windows TLS CA cache"
(New-Object System.Net.WebClient).DownloadString("https://github.com") >$null

$env:PYTHONUTF8=1

Get-Date
Write-Host "Compiling Mesa"
$builddir = New-Item -Force -ItemType Directory -Name "_build"
$installdir = New-Item -Force -ItemType Directory -Name "_install"
$builddir=$builddir.FullName
$installdir=$installdir.FullName
$sourcedir=$PWD

Remove-Item -Recurse -Force $builddir
Remove-Item -Recurse -Force $installdir
New-Item -ItemType Directory -Path $builddir
New-Item -ItemType Directory -Path $installdir

Write-Output builddir:$builddir
Write-Output installdir:$installdir
Write-Output sourcedir:$sourcedir

$installPath=& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -version 16.0  -property installationpath
Write-Output "vswhere.exe installPath: $installPath"
$installPath="C:\BuildTools"
Write-Output "Final installPath: $installPath"
Import-Module (Join-Path $installPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $installPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -no_logo -host_arch=amd64'

Push-Location $builddir

meson --default-library=shared -Dzlib:default_library=static --buildtype=release -Db_ndebug=false `
-Db_vscrt=mt --cmake-prefix-path="C:\llvm-10" `
--pkg-config-path="C:\llvm-10\lib\pkgconfig;C:\llvm-10\share\pkgconfig;C:\spirv-tools\lib\pkgconfig" `
--prefix="$installdir" `
-Dllvm=enabled -Dshared-llvm=disabled `
"-Dvulkan-drivers=swrast,amd,microsoft-experimental" "-Dgallium-drivers=swrast,d3d12,zink" `
-Dshared-glapi=enabled -Dgles2=enabled -Dmicrosoft-clc=enabled -Dstatic-libclc=all -Dspirv-to-dxil=true `
-Dbuild-tests=true -Dwerror=true -Dwarning_level=2 -Dzlib:warning_level=1 -Dlibelf:warning_level=1 `
$sourcedir

if ($?) {
  ninja install -j32
}

if ($?) {
  meson test --num-processes 32
}

$buildstatus = $?
Pop-Location

Get-Date

if (!$buildstatus) {
  Write-Host "Mesa build or test failed"
  Exit 1
}

Copy-Item ".\.gitlab-ci\windows\piglit_run.ps1" -Destination $installdir

Copy-Item ".\.gitlab-ci\windows\spirv2dxil_check.ps1" -Destination $installdir
Copy-Item ".\.gitlab-ci\windows\spirv2dxil_run.ps1" -Destination $installdir

Copy-Item ".\.gitlab-ci\windows\deqp_runner_run.ps1" -Destination $installdir

Get-ChildItem -Recurse -Filter "ci" | Get-ChildItem -Filter "*.txt" | Copy-Item -Destination $installdir
