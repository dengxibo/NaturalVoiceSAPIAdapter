[CmdletBinding()]
param(
 [ValidateSet('Install','Uninstall','Plan','Configure')][string]$Action='Install',
 [string]$Source=$PSScriptRoot,
 [string]$Destination=(Join-Path $env:ProgramFiles 'NaturalVoiceSAPIAdapter Full'),
 [string[]]$ModelRoots=@(),
 [switch]$IgnoreLegacyInstallations,
 [string]$TestRegistryRoot,
 [switch]$TestFailAfterRegistration
)
$ErrorActionPreference='Stop'
$owner='dengxibo.NaturalVoiceSAPIAdapter.Full'
$engine='{013AB33B-AD1A-401C-8BEE-F6E2B046A94E}'
$enumerator='{B8B9E38F-E5A2-4661-9FDE-4AC7377AA6F6}'
$hdEngine='{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8C}'
$prepare='{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8E}'
$Destination=[IO.Path]::GetFullPath($Destination)
$Source=[IO.Path]::GetFullPath($Source)
if($TestRegistryRoot -and $TestRegistryRoot -notmatch '^Software\\NaturalVoiceFullTests\\[a-zA-Z0-9-]+$'){throw '测试注册表必须位于专用隔离目录。'}
if($TestFailAfterRegistration -and -not $TestRegistryRoot){throw '故障注入仅允许隔离测试。'}
$stateFile=Join-Path $Destination 'installation-state.json'
$classes=@($engine,$enumerator,$hdEngine,$prepare)
$paths=@($classes|ForEach-Object{'Software\Classes\CLSID\'+$_})
$paths+= 'Software\Microsoft\Speech\Voices\TokenEnums\NaturalVoiceEnumerator'
$voiceIds=@('Xiaoxiao','Yunyi','Andrew','Ava')
$paths+=@($voiceIds|ForEach-Object{'Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHD-'+$_})
$uninstallKey='Software\Microsoft\Windows\CurrentVersion\Uninstall\NaturalVoiceSAPIAdapterFull'
$paths+=$uninstallKey
function Open-Hive($view){
 $hive=if($TestRegistryRoot){[Microsoft.Win32.RegistryHive]::CurrentUser}else{[Microsoft.Win32.RegistryHive]::LocalMachine}
 [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,[Microsoft.Win32.RegistryView]$view)
}
function Actual-Path($view,$path){if($TestRegistryRoot){return "$TestRegistryRoot\$view\$path"};return $path}
function Save-Tree($key){
 $values=@($key.GetValueNames()|ForEach-Object{@{Name=$_;Kind=[int]$key.GetValueKind($_);Value=$key.GetValue($_,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}})
 $children=@($key.GetSubKeyNames()|ForEach-Object{$child=$key.OpenSubKey($_);try{@{Name=$_;Tree=(Save-Tree $child)}}finally{$child.Dispose()}})
 return @{Values=$values;Children=$children}
}
function Load-Tree($key,$tree){
 foreach($value in $tree.Values){
  $data=$value.Value
  switch([int]$value.Kind){3{$data=[byte[]]$data}4{$data=[int]$data}7{$data=[string[]]$data}11{$data=[long]$data}}
  $key.SetValue([string]$value.Name,$data,[Microsoft.Win32.RegistryValueKind][int]$value.Kind)
 }
 foreach($child in $tree.Children){$sub=$key.CreateSubKey($child.Name);try{Load-Tree $sub $child.Tree}finally{$sub.Dispose()}}
}
function Snapshot {
 $entries=@()
 foreach($view in @('Registry64','Registry32')){
  $base=Open-Hive $view
  try{foreach($path in $paths){$key=$base.OpenSubKey((Actual-Path $view $path));try{$entries+=@{View=$view;Path=$path;Tree=if($key){Save-Tree $key}else{$null}}}finally{if($key){$key.Dispose()}}}}finally{$base.Dispose()}
 }
 return ,$entries
}
function Restore($entries){
 foreach($item in $entries){$base=Open-Hive $item.View;try{$path=Actual-Path $item.View $item.Path;$base.DeleteSubKeyTree($path,$false);if($null -ne $item.Tree){$key=$base.CreateSubKey($path);try{Load-Tree $key $item.Tree}finally{$key.Dispose()}}}finally{$base.Dispose()}}
}
function Write-Key($view,$path,$values){
 $base=Open-Hive $view
 try{$key=$base.CreateSubKey((Actual-Path $view $path));try{foreach($name in $values.Keys){$key.SetValue($name,[string]$values[$name],[Microsoft.Win32.RegistryValueKind]::String)}}finally{$key.Dispose()}}finally{$base.Dispose()}
}
function Read-Value($view,$path,$name){$base=Open-Hive $view;try{$key=$base.OpenSubKey((Actual-Path $view $path));try{if($key){return $key.GetValue($name)}}finally{if($key){$key.Dispose()}}}finally{$base.Dispose()}}
function Owned-Path($relative){
 if([IO.Path]::IsPathRooted($relative)){throw '清单包含绝对路径。'}
 $full=[IO.Path]::GetFullPath((Join-Path $Source $relative))
 if(-not $full.StartsWith($Source.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw '清单路径越界。'}
 return $full
}
function Find-HdVoices {
 $roots=@($ModelRoots)+@((Join-Path $Destination 'models'),'C:\TTS')
 if(-not $IgnoreLegacyInstallations){foreach($id in $voiceIds){
  $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
  try{$key=$base.OpenSubKey('Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHD-'+$id);try{if($key){$roots+= [string]$key.GetValue('ModelPath')}}finally{if($key){$key.Dispose()}}}finally{$base.Dispose()}
 }}
 $packages=@(Get-AppxPackage 'MicrosoftWindows.Voice.*' -ErrorAction SilentlyContinue)
 $roots+=@($packages|ForEach-Object{$_.InstallLocation})
 $folders=@()
 foreach($root in ($roots|Where-Object{$_}|Select-Object -Unique)){
  if(Test-Path -LiteralPath (Join-Path $root 'Tokens.xml')){$folders+=$root}
  elseif(Test-Path -LiteralPath $root -PathType Container){$folders+=@(Get-ChildItem -LiteralPath $root -Directory|Where-Object{Test-Path -LiteralPath (Join-Path $_.FullName 'Tokens.xml')}|ForEach-Object{$_.FullName})}
 }
 $found=@{}
 foreach($folder in ($folders|Select-Object -Unique)){
  try{
   [xml]$xml=Get-Content -LiteralPath (Join-Path $folder 'Tokens.xml') -Raw -Encoding utf8
   $name=$xml.SelectSingleNode('//Token/String[@name=""]').GetAttribute('value')
   foreach($id in $voiceIds){if($name -match ('^Microsoft '+$id+' \(Natural HD\)') -and -not $found.ContainsKey($id)){$found[$id]=@{Id=$id;Name=$name;Path=[IO.Path]::GetFullPath($folder)}}}
  }catch{Write-Warning "跳过无法识别的模型目录：$folder"}
 }
 return @($found.Values|Sort-Object Id)
}
$old=$null
if(Test-Path -LiteralPath $Destination){
 if(Test-Path -LiteralPath $stateFile){$old=Get-Content -LiteralPath $stateFile -Raw -Encoding utf8|ConvertFrom-Json;if($old.Owner -ne $owner){throw '目标目录不属于本发行版。'}}
 elseif($Action -ne 'Plan'){throw '目标目录存在但没有本发行版所有权标记。'}
}
if(-not (Test-Path -LiteralPath (Join-Path $Source 'package.json')) -and $old -and -not $old.Uninstalled){$Source=[string]$old.BinaryRoot}
if($Action -eq 'Configure'){Start-Process -FilePath (Join-Path $Source 'VoiceSettings.exe');return}
if($Action -eq 'Uninstall'){
 if(-not $old -or $old.Uninstalled){throw '此目录没有有效安装。'}
}else{
 if(-not [Environment]::Is64BitOperatingSystem){throw '整合发行版目前仅验证 x64 Windows；保留上游旧平台源码，但不声明本包支持。'}
 $manifest=Get-Content -LiteralPath (Join-Path $Source 'package.json') -Raw -Encoding utf8|ConvertFrom-Json
 foreach($entry in $manifest.Files){$file=Owned-Path $entry.Path;if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.Sha256){throw "文件校验失败：$($entry.Path)"}}
 $version=(Get-FileHash -LiteralPath (Join-Path $Source 'package.json')).Hash.Substring(0,16).ToLowerInvariant()
 $binaryRoot=Join-Path $Destination ('versions/'+$version)
 $runtime=@(Get-AppxPackage 'MicrosoftWindows.Client.Core' -ErrorAction SilentlyContinue|Select-Object -First 1).InstallLocation
 $voices=@()
 if($runtime -and (Test-Path -LiteralPath (Join-Path $runtime 'SpeechSynthesizerExtension.dll'))){
  $runtimeVersion=[Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $runtime 'SpeechSynthesizerExtension.dll')).FileVersion
  if($runtimeVersion -eq '2608.11000.0.0'){$voices=@(Find-HdVoices)}else{Write-Warning "HD 扩展版本未验证：$runtimeVersion；本次仅安装联网及普通语音引擎。"}
 }
 if($Action -eq 'Plan'){@{Action=$Action;Destination=$Destination;BinaryRoot=$binaryRoot;HDVoices=$voices;Runtime=$runtime;Files=$manifest.Files.Count}|ConvertTo-Json -Depth 8;return}
}
if(-not $TestRegistryRoot -and -not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw '请从 Installer.exe 启动并通过 Windows 管理员确认。'}
$before=Snapshot
try{
 if($Action -eq 'Uninstall'){
  # Restore prior registrations only if the active class still belongs to this
  # installed version. Never overwrite another installer's later changes.
  foreach($view in @('Registry64','Registry32')){foreach($id in $classes){
   $current=Read-Value $view ('Software\Classes\CLSID\'+$id+'\InprocServer32') ''
   if($current -and -not $current.StartsWith([string]$old.BinaryRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw '其他安装已接管语音注册，请先处理该安装，避免卸载时覆盖。'}
  }}
  Restore $old.InitialRegistry
  $old.Uninstalled=$true
  $old|ConvertTo-Json -Depth 100|Set-Content -LiteralPath $stateFile -Encoding utf8
  Write-Output '已卸载本发行版的注册，并恢复安装前的语音注册。模型、用户设置及供已打开读者使用的版本文件保留。关闭读者后可删除本安装目录。'
  return
 }
 New-Item -ItemType Directory -Path $binaryRoot -Force|Out-Null
 foreach($entry in $manifest.Files){
  $target=Join-Path $binaryRoot $entry.Path
  New-Item -ItemType Directory -Path (Split-Path $target) -Force|Out-Null
  if(Test-Path -LiteralPath $target){if((Get-FileHash -LiteralPath $target).Hash -ne $entry.Sha256){throw "已存在的版本文件不同：$target"}}
  else{Copy-Item -LiteralPath (Owned-Path $entry.Path) -Destination $target}
 }
 if(-not $Source.Equals($binaryRoot,[StringComparison]::OrdinalIgnoreCase)){Copy-Item -LiteralPath (Join-Path $Source 'package.json') -Destination (Join-Path $binaryRoot 'package.json') -Force}
 foreach($arch in @(@{View='Registry64';Folder='x64'},@{View='Registry32';Folder='x86'})){
  $dll=Join-Path $binaryRoot ($arch.Folder+'/NaturalVoiceSAPIAdapter.dll')
  foreach($id in @($engine,$enumerator)){
   Write-Key $arch.View ('Software\Classes\CLSID\'+$id) @{''='NaturalVoiceSAPIAdapter Full';Owner=$owner}
   Write-Key $arch.View ('Software\Classes\CLSID\'+$id+'\InprocServer32') @{''=$dll;ThreadingModel='Both'}
  }
  Write-Key $arch.View 'Software\Microsoft\Speech\Voices\TokenEnums\NaturalVoiceEnumerator' @{CLSID=$enumerator}
  foreach($id in @($hdEngine,$prepare)){
   Write-Key $arch.View ('Software\Classes\CLSID\'+$id) @{''='Local HD SAPI';Owner='TtsApplication.HdSapiBridge'}
   Write-Key $arch.View ('Software\Classes\CLSID\'+$id+'\InprocServer32') @{''=(Join-Path $binaryRoot ($arch.Folder+'/HdSapiEngine.dll'));ThreadingModel='Both'}
  }
  foreach($voice in $voices){
   $path='Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHD-'+$voice.Id
   $display=$voice.Name+' [Local SAPI HD]'
   $locale=if($voice.Id -in @('Xiaoxiao','Yunyi')){'zh-CN'}else{'en-US'}
   $language=if($locale -eq 'zh-CN'){'804;409'}else{'409'}
   $gender=if($voice.Id -in @('Yunyi','Andrew')){'Male'}else{'Female'}
   Write-Key $arch.View $path @{''=$display;CLSID=$hdEngine;RuntimeRoot=$runtime;ModelPath=$voice.Path;VoiceName=$voice.Name;Locale=$locale;HelperPath=(Join-Path $binaryRoot 'hd/HdSpeechHost.exe')}
   Write-Key $arch.View ($path+'\Attributes') @{Name=$display;Language=$language;Gender=$gender;Age='Adult';Vendor='TtsApplication';NaturalVoiceType='Narrator;Local;HD'}
  }
 }
 if($TestFailAfterRegistration){throw '测试注入：注册后失败。'}
 # Stable maintenance launcher, copied only after all registration succeeded.
 foreach($file in @('Installer.exe','Install-Full.ps1')){Copy-Item -LiteralPath (Join-Path $Source $file) -Destination (Join-Path $Destination $file) -Force}
 $initial=if($old -and -not $old.Uninstalled){$old.InitialRegistry}else{$before}
 $new=@{Owner=$owner;BinaryRoot=$binaryRoot;InitialRegistry=$initial;Uninstalled=$false;HDVoices=@($voices|ForEach-Object{$_.Id});InstalledUtc=[DateTime]::UtcNow.ToString('o')}
 $new|ConvertTo-Json -Depth 100|Set-Content -LiteralPath ($stateFile+'.tmp') -Encoding utf8
 Move-Item -LiteralPath ($stateFile+'.tmp') -Destination $stateFile -Force
 Write-Key 'Registry64' $uninstallKey @{DisplayName='NaturalVoiceSAPIAdapter 完整版';DisplayVersion=$manifest.Version;Publisher='dengxibo / gexgd0419';InstallLocation=$Destination;UninstallString=('"'+(Join-Path $Destination 'Installer.exe')+'" /uninstall')}
 Write-Output "安装完成：联网 secv2、普通本地引擎、HD 引擎及公开预合成服务（32/64 位）。已注册 HD 音色：$($voices.Count)。请重启朗读软件。"
 if(-not $voices.Count){Write-Output '未找到可用 HD 模型/已验证系统扩展。普通及联网引擎已安装；模型请通过 Windows 设置或自己的本地包准备，再运行安装器。'}
}catch{
 Restore $before
 if($old){$old|ConvertTo-Json -Depth 100|Set-Content -LiteralPath $stateFile -Encoding utf8}
 elseif(Test-Path -LiteralPath $Destination){@{Owner=$owner;Uninstalled=$true;InitialRegistry=@();BinaryRoot=''}|ConvertTo-Json|Set-Content -LiteralPath $stateFile -Encoding utf8}
 throw
}
