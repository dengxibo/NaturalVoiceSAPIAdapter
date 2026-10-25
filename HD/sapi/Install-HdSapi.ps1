param(
    [string]$Root = $PSScriptRoot,
    [string]$BinaryRoot,
    [ValidateSet('CurrentUser','LocalMachine')][string]$Scope = 'CurrentUser',
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
if (-not $BinaryRoot) { $BinaryRoot = $Root }
$BinaryRoot = [IO.Path]::GetFullPath($BinaryRoot)
$classId = '{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8C}'
$owner = 'TtsApplication.HdSapiBridge'
$hive = if ($Scope -eq 'CurrentUser') { [Microsoft.Win32.RegistryHive]::CurrentUser } else { [Microsoft.Win32.RegistryHive]::LocalMachine }
if ($Scope -eq 'LocalMachine' -and -not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'LocalMachine installation requires an administrator PowerShell window.'
}
$voices = @(
    @{Id='Xiaoxiao';Folder='XiaoxiaoHD';Language='804;409';Locale='zh-CN';Gender='Female'},
    @{Id='Yunyi';Folder='YunyiHD';Language='804;409';Locale='zh-CN';Gender='Male'},
    @{Id='Andrew';Folder='AndrewHD';Language='409';Locale='en-US';Gender='Male'},
    @{Id='Ava';Folder='AvaHD';Language='409';Locale='en-US';Gender='Female'}
)
$runtime = $null
if (-not $Uninstall) {
    $runtime = @(Get-AppxPackage 'MicrosoftWindows.Client.Core')[0].InstallLocation
    if (-not $runtime) { throw 'System SpeechSynthesizerExtension package was not found.' }
    if (-not (Test-Path -LiteralPath (Join-Path $BinaryRoot 'HdSpeechHost.exe'))) { throw 'HdSpeechHost.exe is missing.' }
    foreach ($voice in $voices) {
        $voice.Path = Join-Path $Root ('models/' + $voice.Folder)
        $tokenFile = Join-Path $voice.Path 'Tokens.xml'
        if (-not (Test-Path -LiteralPath $tokenFile)) { throw "Voice model missing: $tokenFile" }
        [xml]$xml = Get-Content -LiteralPath $tokenFile -Raw -Encoding utf8
        $voice.Name = $xml.SelectSingleNode('//Token/String[@name=""]').GetAttribute('value')
        if (-not $voice.Name) { throw "Voice name missing: $tokenFile" }
    }
}
foreach ($architecture in @(@{View=[Microsoft.Win32.RegistryView]::Registry64;Folder='x64'},@{View=[Microsoft.Win32.RegistryView]::Registry32;Folder='x86'})) {
    $engine = Join-Path $BinaryRoot ($architecture.Folder + '/HdSapiEngine.dll')
    if (-not $Uninstall -and -not (Test-Path -LiteralPath $engine)) { throw "Engine missing: $engine" }
    $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, $architecture.View)
    try {
        foreach ($registeredClassId in @($classId,'{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8E}')) {
        $classPath = 'Software\Classes\CLSID\' + $registeredClassId
        $existing = $registry.OpenSubKey($classPath)
        if ($existing) {
            try { if ($existing.GetValue('Owner') -ne $owner) { throw 'Refusing to overwrite an unrelated COM class.' } }
            finally { $existing.Dispose() }
        }
        if ($Uninstall) {
            if ($existing) { $registry.DeleteSubKeyTree($classPath, $false) }
        } else {
            $class = $registry.CreateSubKey($classPath)
            try { $class.SetValue('', 'Local HD SAPI Bridge'); $class.SetValue('Owner', $owner) } finally { $class.Dispose() }
            $server = $registry.CreateSubKey($classPath + '\InprocServer32')
            try { $server.SetValue('', $engine); $server.SetValue('ThreadingModel', 'Both') } finally { $server.Dispose() }
        }
        }
        foreach ($voice in $voices) {
            $tokenPath = 'Software\Microsoft\Speech\Voices\Tokens\TtsApplicationHD-' + $voice.Id
            $existingToken = $registry.OpenSubKey($tokenPath)
            if ($existingToken) {
                try { if ($existingToken.GetValue('CLSID') -ne $classId) { throw 'Refusing to overwrite an unrelated voice token.' } }
                finally { $existingToken.Dispose() }
            }
            if ($Uninstall) { if ($existingToken) { $registry.DeleteSubKeyTree($tokenPath, $false) }; continue }
            $token = $registry.CreateSubKey($tokenPath)
            try {
                $display = $voice.Name + ' [Local SAPI HD]'
                $token.SetValue('', $display); $token.SetValue('CLSID', $classId)
                $token.SetValue('RuntimeRoot', $runtime); $token.SetValue('ModelPath', $voice.Path)
                $token.SetValue('VoiceName', $voice.Name); $token.SetValue('Locale', $voice.Locale)
                $token.SetValue('HelperPath', (Join-Path $BinaryRoot 'HdSpeechHost.exe'))
                $attributes = $token.CreateSubKey('Attributes')
                try {
                    $attributes.SetValue('Name', $display); $attributes.SetValue('Language', $voice.Language)
                    $attributes.SetValue('Gender', $voice.Gender); $attributes.SetValue('Age', 'Adult')
                    $attributes.SetValue('Vendor', 'TtsApplication'); $attributes.SetValue('NaturalVoiceType', 'Narrator;Local;HD')
                } finally { $attributes.Dispose() }
            } finally { $token.Dispose() }
        }
    } finally { $registry.Dispose() }
}
if ($Uninstall) { Write-Output "Removed this bridge's own COM class and four voice tokens ($Scope)." }
else { Write-Output "Registered four Local SAPI HD voices for x64 and x86 ($Scope). Restart the reading application to refresh its voice list." }
