# Updates the changed CarLife files in the car with ONE ssh login (one password): the session supervisor (nbt-aad), the
# display script, the knob watcher and the USB capture program. Files are joined into one stream and cut apart on the car with dd (the car has no tar).
# Run it only while CarLife is stopped (Force Stop in the CarLife menu): a running script must not be overwritten.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$updates = @(
    @{ src = "$root\package\nbt-aad";                              name = 'clmenu-nbt-aad';     dst = '/opt/aa/nbt-aad' },
    @{ src = "$root\..\build\carlife_qnx_usb_capture.elf";         name = 'clmenu-capture.elf'; dst = '/opt/aa/carlink/carlife_qnx_usb_capture.elf' },
    @{ src = "$root\..\tools\carlife_visible_first_frame.sh";    name = 'clmenu-display.sh'; dst = '/opt/aa/carlink/carlife_visible_first_frame.sh' },
    @{ src = "$root\..\build\carlife_qnx_most_watch.elf";         name = 'clmenu-watch.elf'; dst = '/opt/aa/carlink/carlife_qnx_most_watch.elf' },
    @{ src = "$root\tools\watch-env-bisect.sh";                  name = 'watch-env-bisect.sh'; dst = $null },
    @{ src = "$root\..\build\carlife_qnx_tcp_probe.elf";        name = 'clmenu-tcpprobe.elf'; dst = $null },
    @{ src = "$root\..\tools\carlife_usb_visible.sh";           name = 'clmenu-launch.sh';   dst = '/opt/aa/carlink/carlife_usb_visible.sh' }
)
$ms = New-Object System.IO.MemoryStream
$rc = 'cd /dev/shmem;'
$cp = ''
foreach ($u in $updates) {
    if (-not (Test-Path -LiteralPath $u.src)) { throw "Missing $($u.src)" }
    $full = (Resolve-Path -LiteralPath $u.src).Path
    $bytes = [System.IO.File]::ReadAllBytes($full)
    $ms.Write($bytes, 0, $bytes.Length)
    $hash = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.Substring(0, 16)
    Write-Host ("{0,8}  {1}  sha256 {2}...  -> {3}" -f $bytes.Length, $u.name, $hash, $u.dst)
    $rc += " dd of=$($u.name) bs=1 count=$($bytes.Length) 2>/dev/null;"
    if ($u.dst) { $cp += " cp /dev/shmem/$($u.name) $($u.dst); chmod +x $($u.dst);" } else { $cp += " chmod +x /dev/shmem/$($u.name);" }
}
$rc += ' mount -o remount rw /net/hu-omap/fs/sda0;' + $cp + ' ls -l /opt/aa/nbt-aad /opt/aa/carlink/carlife_qnx_usb_capture.elf /opt/aa/carlink/carlife_visible_first_frame.sh /opt/aa/carlink/carlife_qnx_most_watch.elf /opt/aa/carlink/carlife_usb_visible.sh'
$all = $ms.ToArray()
$ssh = (Get-Command ssh -ErrorAction Stop).Source
Write-Host "Uploading $($updates.Count) files, $($all.Length) bytes (enter the password once)"
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ssh
$psi.Arguments = '-o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 root@127.0.0.1 "' + $rc + '"'
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$p = [System.Diagnostics.Process]::Start($psi)
$in = $p.StandardInput.BaseStream
$in.Write($all, 0, $all.Length)
$in.Flush()
$in.Close()
$p.WaitForExit()
Write-Host "ssh exit code: $($p.ExitCode)"
