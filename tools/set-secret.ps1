# Saves a secret in Windows Credential Manager for Usage Monitor ({{cred:...}} in .conf files).
# The value is typed hidden and never written to the console or the command history.
#
#   powershell -ExecutionPolicy Bypass -File tools\set-secret.ps1                    (Claude sessionKey)
#   powershell -ExecutionPolicy Bypass -File tools\set-secret.ps1 -Target UsageMonitor/ISP -User myname
# From Bitwarden (bwprx puts the key in an env var, nothing is printed):
#   bwprx-client run --query Soniox-organization --env SECRET=password -- powershell -ExecutionPolicy Bypass -File tools\set-secret.ps1 -Target UsageMonitor/Soniox -User api-key -FromEnv SECRET
param(
    [string]$Target = 'UsageMonitor/Claude',
    [string]$User = 'sessionKey',
    [string]$FromEnv = ''   # read the value from this environment variable instead of asking
)
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class CredW {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct CREDENTIAL {
    public uint Flags; public uint Type; public string TargetName; public string Comment;
    public long LastWritten; public uint CredentialBlobSize; public IntPtr CredentialBlob;
    public uint Persist; public uint AttributeCount; public IntPtr Attributes;
    public string TargetAlias; public string UserName; }
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool CredWrite(ref CREDENTIAL c, uint flags);
}
'@
if ($FromEnv) {
    $plain = [Environment]::GetEnvironmentVariable($FromEnv)
    $sec = New-Object Security.SecureString
    if ($plain) { foreach ($ch in $plain.Trim().ToCharArray()) { $sec.AppendChar($ch) } }
    $plain = $null
} else {
    $sec = Read-Host -AsSecureString "Paste the value for $Target (input is hidden), then press Enter"
}
if ($sec.Length -eq 0) { Write-Host 'Empty value, nothing saved.'; exit 1 }
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToGlobalAllocUnicode($sec)
try {
    $c = New-Object CredW+CREDENTIAL
    $c.Type = 1                      # CRED_TYPE_GENERIC
    $c.TargetName = $Target
    $c.UserName = $User
    $c.Persist = 2                   # CRED_PERSIST_LOCAL_MACHINE
    $c.CredentialBlob = $ptr
    $c.CredentialBlobSize = [uint32]($sec.Length * 2)  # UTF-16 bytes
    if (-not [CredW]::CredWrite([ref]$c, 0)) {
        throw "CredWrite failed, error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    Write-Host "Saved $Target ($($sec.Length) characters)."
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeGlobalAllocUnicode($ptr)
}
