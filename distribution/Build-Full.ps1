[CmdletBinding()]
param(
 [string]$MSBuild,
 [string]$PlatformToolset='v145',
 [string]$Output,
 [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $Output){$Output=Join-Path $root 'dist/Standalone'}
$BundleRoot=[IO.Path]::GetFullPath($Output)
$Output=Join-Path $BundleRoot 'data'
if(-not $MSBuild){
 $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
 $vs=& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
 $MSBuild=Join-Path $vs 'MSBuild/Current/Bin/MSBuild.exe'
}
function Build($project,$platform,$outDir){
 & $MSBuild (Join-Path $root $project) /p:Configuration=Release "/p:Platform=$platform" "/p:PlatformToolset=$PlatformToolset" /p:PreferredToolArchitecture=x64 /p:NaturalVoiceRegisterOutput=false /p:GenerateDebugInformation=false "/p:SolutionDir=$root\" "/p:OutDir=$outDir\" /v:minimal /nologo
 if($LASTEXITCODE){throw "编译失败：$project $platform"}
}
if(-not (Test-Path -LiteralPath (Join-Path $root 'packages/Microsoft.CognitiveServices.Speech.1.41.1/build/native/Microsoft.CognitiveServices.Speech.targets'))){throw '请先使用 NuGet restore 主适配器及 Installer/packages.config；参见 docs/整合与构建.md。'}
if(-not $SkipBuild){
 foreach($arch in @(@{Platform='x64';Folder='x64'},@{Platform='Win32';Folder='x86'})){
  $outDir=Join-Path $root ('out/full/'+$arch.Folder)
  Build 'NaturalVoiceSAPIAdapter/NaturalVoiceSAPIAdapter.vcxproj' $arch.Platform $outDir
  Build 'HD/sapi/HdSapiEngine.vcxproj' $arch.Platform $outDir
 }
 Build 'HD/tools/NativeHdProbe.vcxproj' 'x64' (Join-Path $root 'out/full/hd')
 Build 'Installer/Installer.vcxproj' 'Win32' (Join-Path $root 'out/full/advanced')
 Build 'distribution/Installer.vcxproj' 'Win32' (Join-Path $root 'out/full/launcher')
}
# Never collect arbitrary DLLs from System32 or bundle downloaded voice models.
$speech=@('core','extension.audio.sys','extension.embedded.tts','extension.onnxruntime','extension.telemetry')
$crt=@('concrt140.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll','vcruntime140.dll','vcruntime140_1.dll','vcruntime140_threads.dll')
foreach($folder in @('x64','x86','hd','licenses','sdk','docs')){New-Item -ItemType Directory -Path (Join-Path $Output $folder) -Force|Out-Null}
# A reused output containing obsolete payload must not silently enter a release.
$allowed=@('Installer.exe','VoiceSettings.exe','Install-Full.ps1','使用说明.txt','package.json','EdgeGecV2-CorrespondingSource.zip')
foreach($file in Get-ChildItem -LiteralPath $Output -File){if($file.Name -notin $allowed){throw "输出目录有未知文件，请使用新目录：$($file.Name)"}}
foreach($arch in @('x64','x86')){
 $source=Join-Path $root ('out/full/'+$arch)
 $target=Join-Path $Output $arch
 $files=@('NaturalVoiceSAPIAdapter.dll','HdSapiEngine.dll','EdgeGecV2.dll')+@($speech|ForEach-Object{'Microsoft.CognitiveServices.Speech.'+$_+'.dll'})
 $files+=@($crt|Where-Object{Test-Path -LiteralPath (Join-Path $source $_)})
 foreach($file in $files){Copy-Item -LiteralPath (Join-Path $source $file) -Destination $target -Force}
 foreach($existing in Get-ChildItem -LiteralPath $target -File){if($existing.Name -notin $files){throw "旧输出包含不应发行的文件：$($existing.FullName)"}}
}
Copy-Item -LiteralPath (Join-Path $root 'out/full/hd/NativeHdProbe.exe') -Destination (Join-Path $Output 'hd/HdSpeechHost.exe') -Force
Copy-Item -LiteralPath (Join-Path $root 'out/full/advanced/Installer.exe') -Destination (Join-Path $Output 'VoiceSettings.exe') -Force
Copy-Item -LiteralPath (Join-Path $root 'out/full/launcher/Installer.exe') -Destination $Output -Force
foreach($file in @('Install-Full.ps1','使用说明.txt')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $Output -Force}
foreach($file in @('HdPrepare.h','HdPrepareIntegration.zh-CN.md')){Copy-Item -LiteralPath (Join-Path $root ('HD/sapi/'+$file)) -Destination (Join-Path $Output 'sdk') -Force}
Copy-Item -Path (Join-Path $PSScriptRoot 'licenses/*') -Destination (Join-Path $Output 'licenses') -Force
Copy-Item -Path (Join-Path $root 'docs/*') -Destination (Join-Path $Output 'docs') -Force
$src=Join-Path $root 'out/EdgeGecV2-CorrespondingSource'
New-Item -ItemType Directory -Path $src -Force|Out-Null
foreach($folder in @('EdgeGecV2','NaturalVoiceSAPIAdapter','licenses')){New-Item -ItemType Directory -Path (Join-Path $src $folder) -Force|Out-Null}
Copy-Item -Path (Join-Path $root 'EdgeGecV2/*') -Destination (Join-Path $src 'EdgeGecV2') -Force
foreach($file in @('EdgeGec.h','EdgeGecTables.h')){Copy-Item -LiteralPath (Join-Path $root ('NaturalVoiceSAPIAdapter/'+$file)) -Destination (Join-Path $src 'NaturalVoiceSAPIAdapter') -Force}
foreach($file in @('LGPL-3.0.edge-tts.txt','GPL-3.0.txt')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('licenses/'+$file)) -Destination (Join-Path $src 'licenses') -Force}
Copy-Item -LiteralPath (Join-Path $root 'EdgeGecV2/BUILD.txt') -Destination (Join-Path $src 'BUILD.txt') -Force
Compress-Archive -Path (Join-Path $src '*') -DestinationPath (Join-Path $Output 'EdgeGecV2-CorrespondingSource.zip') -Force
$files=@(Get-ChildItem -LiteralPath $Output -Recurse -File|Where-Object{$_.Name -ne 'package.json'}|Sort-Object FullName|ForEach-Object{@{Path=$_.FullName.Substring($Output.Length+1).Replace('\','/');Sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}})
@{Version='0.2.9-secv2-hd.1';Base='7194eca5b78d8e477793c159d731d62b6357ffbf';RequiresUpstreamInstallation=$false;Experimental=$true;SDKDistributionApprovalVerified=$false;ThirdPartyLicensingNotice='Embedded Speech SDK distribution requires separate approval under section 3; no approval was supplied. Models and Windows system runtime are excluded.';Files=$files}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $Output 'package.json') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $Output 'Installer.exe') -Destination $BundleRoot -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '安装说明.txt') -Destination $BundleRoot -Force
Compress-Archive -LiteralPath $BundleRoot -DestinationPath ($BundleRoot+'.zip') -Force
Write-Output "独立安装测试包：$BundleRoot.zip（根目录只保留安装器、中文说明及 data）"
