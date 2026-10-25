param([switch]$Remove,[string]$BinaryRoot,[string]$HelperPath)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$classId = '{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8D}'
foreach ($arch in @(@{View=[Microsoft.Win32.RegistryView]::Registry64;Folder='x64'},@{View=[Microsoft.Win32.RegistryView]::Registry32;Folder='Win32'})) {
    $dll=if($BinaryRoot){Join-Path $BinaryRoot ($(if($arch.Folder -eq 'Win32'){'x86'}else{'x64'})+'/HdSapiEngine.dll')}else{Join-Path $project ('bin/sapi/'+$arch.Folder+'/HdSapiEngine.dll')}
    $helper=if($HelperPath){$HelperPath}else{Join-Path $project 'bin/probes/NativeHdProbe.exe'}
    $user = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,$arch.View)
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$arch.View)
    try {
        $preparePath='Software\Classes\CLSID\{D8A46B78-87DF-4DDE-9E2A-468CE2F31D90}'
        $existingPrepare=$user.OpenSubKey($preparePath)
        if($existingPrepare){try{if($existingPrepare.GetValue('Owner') -ne 'TtsApplication.HdSapiBridge.Tests'){throw 'Unrelated prepare test class exists.'}}finally{$existingPrepare.Dispose()}}
        if($Remove){$user.DeleteSubKeyTree($preparePath,$false)}else{
            $key=$user.CreateSubKey($preparePath);$key.SetValue('Owner','TtsApplication.HdSapiBridge.Tests');$key.Dispose()
            $key=$user.CreateSubKey($preparePath+'\InprocServer32');$key.SetValue('',$dll);$key.SetValue('ThreadingModel','Both');$key.Dispose()
        }
        $classPath = 'Software\Classes\CLSID\' + $classId
        $existing = $user.OpenSubKey($classPath)
        if ($existing) { try { if ($existing.GetValue('Owner') -ne 'TtsApplication.HdSapiBridge.Tests') { throw 'Unrelated test class exists.' } } finally { $existing.Dispose() } }
        if ($Remove) { $user.DeleteSubKeyTree($classPath,$false) }
        else {
            $key = $user.CreateSubKey($classPath); $key.SetValue('Owner','TtsApplication.HdSapiBridge.Tests'); $key.Dispose()
            $key = $user.CreateSubKey($classPath+'\InprocServer32'); $key.SetValue('',$dll); $key.SetValue('ThreadingModel','Both'); $key.Dispose()
        }
        foreach ($name in @('Xiaoxiao','Yunyi','Andrew','Ava')) {
            $path = 'Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHDTest-'+$name
            $existing = $user.OpenSubKey($path)
            if ($existing) { try { if ($existing.GetValue('CLSID') -ne $classId) { throw 'Unrelated test token exists.' } } finally { $existing.Dispose() } }
            if ($Remove) { $user.DeleteSubKeyTree($path,$false); continue }
            $source = $machine.OpenSubKey('Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHD-'+$name)
            if (-not $source) { throw 'Installed voice is missing.' }
            $target = $user.CreateSubKey($path)
            try {
                foreach ($field in @('','RuntimeRoot','ModelPath','VoiceName','Locale')) { $target.SetValue($field,$source.GetValue($field)) }
                $target.SetValue('CLSID',$classId); $target.SetValue('HelperPath',$helper)
                $attributes = $source.OpenSubKey('Attributes'); $copy = $target.CreateSubKey('Attributes')
                try { foreach ($field in $attributes.GetValueNames()) { $copy.SetValue($field,$attributes.GetValue($field)) } } finally { $attributes.Dispose(); $copy.Dispose() }
            } finally { $source.Dispose(); $target.Dispose() }
        }
    } finally { $user.Dispose(); $machine.Dispose() }
}
