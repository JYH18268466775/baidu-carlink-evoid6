# Uploads the CarLife iDrive menu files to /dev/shmem/clmenu-* with ONE ssh login (one password).
# The car has no tar: the files are joined into one stream and cut apart on the car with dd (exact sizes, bs=1).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$P = Join-Path $root '..\idrive-hold\package'
$B = Join-Path $root '..\build'
$files = @(
    "$P\libNbtAad.so", "$P\NbtAad.manifest", "$P\NbtAad.xml", "$P\NbtAad_ALL_Images.zip",
    "$root\package\NbtAad_ALL_Texts.zip", "$root\package\nbt-aad",
    "$B\carlife_qnx_usb_capture.elf", "$B\carlife_qnx_mmr_replay.elf", "$B\carlife_qnx_most_watch.elf",
    "$B\carlife_qnx_cp_purge.elf", "$B\carlife_qnx_ctl.elf",
    "$root\..\tools\carlife_usb_visible.sh", "$root\..\tools\carlife_visible_first_frame.sh",
    "$root\..\tools\carlife_cp_purge.sh", "$root\..\src\carlife_qnx_replay_guard.sh",
    "$root\tools\clmenu_install.sh", "$root\tools\clmenu_uninstall.sh"
)
$ms = New-Object System.IO.MemoryStream
$rc = 'cd /dev/shmem;'
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f" }
    $full = (Resolve-Path -LiteralPath $f).Path
    $bytes = [System.IO.File]::ReadAllBytes($full)
    $ms.Write($bytes, 0, $bytes.Length)
    $name = [System.IO.Path]::GetFileName($full)
    $rc += " dd of=clmenu-$name bs=1 count=$($bytes.Length) 2>/dev/null;"
    Write-Host ("{0,8}  clmenu-{1}" -f $bytes.Length, $name)
}
$rc += ' ls -l clmenu-*'
$tmp = Join-Path $env:TEMP 'clmenu.bin'
[System.IO.File]::WriteAllBytes($tmp, $ms.ToArray())
Write-Host "Uploading $($files.Count) files, $($ms.Length) bytes (enter the password once)"
$sshArgs = '-o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 root@127.0.0.1 "' + $rc + '"'
Start-Process -FilePath 'ssh' -ArgumentList $sshArgs -RedirectStandardInput $tmp -NoNewWindow -Wait
