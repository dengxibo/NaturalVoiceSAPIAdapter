param(
 [string]$Package=(Join-Path $PSScriptRoot '../dist/Standalone'),
 [string]$ModelRoot=(Join-Path $PSScriptRoot '../../../models')
)
$ErrorActionPreference='Stop'
$data=Join-Path $Package 'data'
$manifest=Get-Content (Join-Path $data 'package.json') -Raw -Encoding utf8|ConvertFrom-Json
if($manifest.RequiresUpstreamInstallation -ne $false){throw 'Package still declares an upstream prerequisite'}
foreach($file in @('AdvancedInstaller.exe','TtsApplication.exe','SpeechSDKPatcher.exe')){if($manifest.Files.Path -match ([regex]::Escape($file)+'$')){throw "Old installer/sample payload remains: $file"}}
$id=[Guid]::NewGuid().ToString()
$registry="Software\NaturalVoiceFullTests\$id"
$target=Join-Path $PSScriptRoot ('../out/standalone-fresh-'+$id)
$parameters=@{Source=$data;Destination=$target;TestRegistryRoot=$registry;IgnoreLegacyInstallations=$true;ModelRoots=@([IO.Path]::GetFullPath($ModelRoot))}
$classes=@('{013AB33B-AD1A-401C-8BEE-F6E2B046A94E}','{B8B9E38F-E5A2-4661-9FDE-4AC7377AA6F6}','{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8C}','{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8E}')
try{
 # No old adapter fixture. Model discovery explicitly bypasses old registrations.
 & (Join-Path $data 'Install-Full.ps1') @parameters -Action Install
 $state=Get-Content (Join-Path $target 'installation-state.json') -Raw -Encoding utf8|ConvertFrom-Json
 if(@($state.InitialRegistry|Where-Object{$null -ne $_.Tree}).Count){throw 'Fresh test had prior registrations'}
 if($state.HDVoices.Count -ne 4){throw 'Expected models from explicit folder and Windows packages'}
 foreach($view in @('Registry64','Registry32')){
  foreach($cls in $classes){
   $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("$registry\$view\Software\Classes\CLSID\$cls\InprocServer32")
   try{if(-not $key){throw 'Required class missing'};$path=[string]$key.GetValue('');if(-not $path.StartsWith([string]$state.BinaryRoot+'\') -or -not (Test-Path -LiteralPath $path)){throw 'Class depends on an external adapter'}}finally{if($key){$key.Dispose()}}
  }
  foreach($voice in @('Xiaoxiao','Yunyi','Andrew','Ava')){
   $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("$registry\$view\Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHD-$voice")
   try{
    if(-not ([string]$key.GetValue('HelperPath')).StartsWith([string]$state.BinaryRoot+'\')){throw 'External helper dependency'}
    if(([string]$key.GetValue('ModelPath')).Contains('TtsApplication HD SAPI')){throw 'Old adapter model location was used'}
   }finally{$key.Dispose()}
  }
 }
 'PASS fresh empty-registry install; all x86/x64 classes and helpers are self-contained'
 'PASS four HD models discovered without reading old adapter registration'
 $probe=Start-Process -FilePath (Join-Path $target 'Installer.exe') -ArgumentList '/check' -WindowStyle Hidden -PassThru -Wait
 if($probe.ExitCode){throw 'Fresh maintenance entry failed'}
 & (Join-Path $target 'Install-Full.ps1') -Action Install -Destination $target -TestRegistryRoot $registry -IgnoreLegacyInstallations -ModelRoots $parameters.ModelRoots
 'PASS independent maintenance reinstall'
 & (Join-Path $data 'Install-Full.ps1') @parameters -Action Uninstall
 foreach($view in @('Registry64','Registry32')){foreach($cls in $classes){$key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("$registry\$view\Software\Classes\CLSID\$cls");if($key){$key.Dispose();throw 'Fresh uninstall left registration'}}}
 'PASS fresh uninstall leaves no adapter registration'
}finally{[Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($registry,$false)}
