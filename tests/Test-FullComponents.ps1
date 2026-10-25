param(
 [string]$Package=(Join-Path $PSScriptRoot '../dist/Standalone'),
 [string]$Python='python',
 [string]$MSBuild,
 [switch]$NoBuild,
 [string]$OrdinaryModelRoot='C:\TTS',
 [string]$HdXiaoxiaoPath
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath (Join-Path $Package 'data/package.json')){$Package=Join-Path $Package 'data'}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $HdXiaoxiaoPath){$base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64);try{$key=$base.OpenSubKey('Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHD-Xiaoxiao');try{if($key){$HdXiaoxiaoPath=[string]$key.GetValue('ModelPath')}}finally{if($key){$key.Dispose()}}}finally{$base.Dispose()}}
if(-not $HdXiaoxiaoPath){throw '组件测试需要本机已经可用的四个 HD 模型/token。此脚本不下载模型或安装生产类。'}
if(-not $MSBuild){$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath;$MSBuild=Join-Path $vs 'MSBuild/Current/Bin/MSBuild.exe'}
$logs=Join-Path $root 'out/full-tests'
New-Item -ItemType Directory -Path $logs -Force|Out-Null
function Run($exe,$parameters,$name){& $exe @parameters *> (Join-Path $logs ($name+'.txt'));if($LASTEXITCODE){Get-Content (Join-Path $logs ($name+'.txt')) -Tail 15;throw "测试失败：$name"};"PASS $name"}
function Build($project,$platform,$outDir,$extra=@()){
 & $MSBuild (Join-Path $root $project) /p:Configuration=Release "/p:Platform=$platform" "/p:OutDir=$outDir\" /nologo /v:minimal @extra
 if($LASTEXITCODE){throw "测试构建失败：$project $platform"}
}
if(-not $NoBuild){foreach($arch in @(@{Platform='x64';Folder='x64'},@{Platform='Win32';Folder='x86'})){
 $outDir=Join-Path $root ('out/tests/'+$arch.Folder)
 foreach($source in @('sapi_smoke.cpp','VoiceInventory.cpp')){Build 'tests/SapiProbe.vcxproj' $arch.Platform $outDir @("/p:ProbeSource=$source")}
 Build 'tests/GecCli.vcxproj' $arch.Platform $outDir
 foreach($project in @('PrepareProbe','Test-HdSapiEvents')){Build ('HD/tools/'+$project+'.vcxproj') $arch.Platform (Join-Path $root ('HD/bin/tests/'+$arch.Platform))}
}}
$registered=$false
try{
 & (Join-Path $root 'HD/tools/Register-HdSapiEventTests.ps1') -BinaryRoot $Package -HelperPath (Join-Path $Package 'hd/HdSpeechHost.exe');$registered=$true
 foreach($arch in @(@{Platform='x64';Folder='x64'},@{Platform='Win32';Folder='x86'})){
  $probe=Join-Path $root ('out/tests/'+$arch.Folder+'/sapi_smoke.exe')
  $dll=Join-Path $Package ($arch.Folder+'/NaturalVoiceSAPIAdapter.dll')
  Run $Python @((Join-Path $root 'tests/test_gec.py'),(Join-Path $root ('out/tests/'+$arch.Folder+'/gec_cli.exe'))) ('gec-'+$arch.Folder)
  Run (Join-Path $root ('out/tests/'+$arch.Folder+'/VoiceInventory.exe')) @($dll,(Join-Path $OrdinaryModelRoot 'Xiaoxiao'),$HdXiaoxiaoPath) ('inventory-'+$arch.Folder)
  foreach($voice in @('en-US-AriaNeural','zh-CN-XiaoxiaoNeural','local')){Run $probe @($dll,$voice,'60000',(Join-Path $logs ($arch.Folder+'-'+$voice+'.wav'))) ($arch.Folder+'-'+$voice)}
  foreach($voice in @('Xiaoxiao','Yunxi')){Run $probe @($dll,('local:'+$voice),'60000',(Join-Path $logs ($arch.Folder+'-ordinary-'+$voice+'.wav')),'--local-basic') ($arch.Folder+'-ordinary-'+$voice)}
  Run $probe @($dll,'en-US-AriaNeural','100') ('cancel-'+$arch.Folder)
  Run (Join-Path $root ('HD/bin/tests/'+$arch.Platform+'/PrepareProbe.exe')) @('HKEY_CURRENT_USER\Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHDTest-Xiaoxiao') ('prepare-'+$arch.Folder)
  foreach($voice in @('Xiaoxiao','Yunyi','Andrew','Ava')){
   Run (Join-Path $root ('HD/bin/tests/'+$arch.Platform+'/Test-HdSapiEvents.exe')) @((Join-Path $Package ($arch.Folder+'/HdSapiEngine.dll')),('HKEY_CURRENT_USER\Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHDTest-'+$voice),'unused','--smoke') ('HD-'+$arch.Folder+'-'+$voice)
  }
 }
}finally{if($registered){& (Join-Path $root 'HD/tools/Register-HdSapiEventTests.ps1') -Remove}}
Write-Output "组件测试完成：$logs。联网测试需要网络；测试不安装完整发行版。"
