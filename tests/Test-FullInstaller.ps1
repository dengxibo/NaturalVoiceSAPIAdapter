param([string]$Package=(Join-Path $PSScriptRoot '../dist/Standalone'))
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath (Join-Path $Package 'data/Install-Full.ps1')){
 $probe=Start-Process -FilePath (Join-Path $Package 'Installer.exe') -ArgumentList '/check' -WindowStyle Hidden -Wait -PassThru
 if($probe.ExitCode){throw 'Package launcher cannot find data script'}
 $Package=Join-Path $Package 'data'
}
$installer=Join-Path $Package 'Install-Full.ps1'
$id=[Guid]::NewGuid().ToString()
$registry="Software\NaturalVoiceFullTests\$id"
$target=Join-Path $PSScriptRoot ('../out/installer-test-'+$id)
$parameters=@{Source=$Package;Destination=$target;TestRegistryRoot=$registry}
function Need($ok,$message){if(-not $ok){throw $message}}
try{
 $root=[Microsoft.Win32.Registry]::CurrentUser
 $oldPath="$registry\Registry64\Software\Classes\CLSID\{013AB33B-AD1A-401C-8BEE-F6E2B046A94E}\InprocServer32"
 $key=$root.CreateSubKey($oldPath);$key.SetValue('','C:\OldAdapter\old.dll');$key.SetValue('PreservedNumber',42,[Microsoft.Win32.RegistryValueKind]::DWord);$key.Dispose()
 & $installer @parameters -Action Plan | Out-File (Join-Path $PSScriptRoot '../out/installer-plan.json') -Encoding utf8
 & $installer @parameters -Action Install
 $state=Get-Content (Join-Path $target 'installation-state.json') -Raw|ConvertFrom-Json
 Need (-not $state.Uninstalled) 'Install was not committed'
 $probe=Start-Process -FilePath (Join-Path $target 'Installer.exe') -ArgumentList '/check' -WindowStyle Hidden -Wait -PassThru
 Need ($probe.ExitCode -eq 0) 'Installed maintenance launcher cannot find root script'
 Need ($state.HDVoices.Count -eq 4) 'Expected four installed HD voices on this test machine'
 $active=$root.OpenSubKey($oldPath);try{Need ($active.GetValue('') -eq (Join-Path $state.BinaryRoot 'x64/NaturalVoiceSAPIAdapter.dll')) 'Wrong active binary'}finally{$active.Dispose()}
 'PASS isolated install, manifest and four HD models'
 & $installer @parameters -Action Install
 'PASS repeat upgrade uses immutable files'
 & (Join-Path $target 'Install-Full.ps1') -Action Install -Destination $target -TestRegistryRoot $registry
 'PASS installed maintenance entry can reinstall its active version'
 $failed=$false
 try{& $installer @parameters -Action Install -TestFailAfterRegistration}catch{$failed=$true}
 Need $failed 'Fault injection did not fail'
 $active=$root.OpenSubKey($oldPath);try{Need ($active.GetValue('') -eq (Join-Path $state.BinaryRoot 'x64/NaturalVoiceSAPIAdapter.dll')) 'Rollback failed'}finally{$active.Dispose()}
 'PASS failure restores previous active registration'
 # Another installer taking ownership must prevent destructive uninstall.
 $key=$root.OpenSubKey($oldPath,$true);$key.SetValue('','C:\Another\adapter.dll');$key.Dispose()
 $blocked=$false;try{& $installer @parameters -Action Uninstall}catch{$blocked=$true}
 Need $blocked 'Uninstall overwrote another owner'
 $key=$root.OpenSubKey($oldPath,$true);$key.SetValue('',(Join-Path $state.BinaryRoot 'x64/NaturalVoiceSAPIAdapter.dll'));$key.Dispose()
 'PASS later installer ownership protected'
 & $installer @parameters -Action Uninstall
 $key=$root.OpenSubKey($oldPath);try{Need ($key.GetValue('') -eq 'C:\OldAdapter\old.dll') 'Initial registration not restored';Need ($key.GetValue('PreservedNumber') -eq 42) 'Typed initial value lost'}finally{$key.Dispose()}
 $hd=$root.OpenSubKey("$registry\Registry64\Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHD-Xiaoxiao");Need ($null -eq $hd) 'Fresh HD token left after uninstall'
 'PASS uninstall restores initial registration and typed values'
 # Fresh installation failure also leaves no active component registration.
 $parameters.Destination=$target+'-fresh'
 $failed=$false;try{& $installer @parameters -Action Install -TestFailAfterRegistration}catch{$failed=$true};Need $failed 'Fresh fault not raised'
 $key=$root.OpenSubKey($oldPath);try{Need ($key.GetValue('') -eq 'C:\OldAdapter\old.dll') 'Fresh rollback did not preserve prior install'}finally{$key.Dispose()}
 'PASS fresh failure restores original installation'
}finally{
 [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($registry,$false)
 # Test files are retained with a unique path for inspection; no external models touched.
}
