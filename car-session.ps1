# CarLink car session helper: each step logs in to the car once (you type the password) and saves everything it prints to
# carlink-logs\<step>-<time>.txt next to this script, so Claude can read the result directly from this folder.
# Steps: upload, prep, collect, wifi-check, wifi-scan (needs -Phone N), sniff (-Phone N). Without -Step a menu is shown.
# Nothing here reads the hotspot password or changes the firewall. upload/prep/wifi-scan ask that the car is parked first.
param(
    [string] $Step = '',
    [int] $Phone = 0
)
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$logs = Join-Path $root 'carlink-logs'
$null = New-Item -ItemType Directory -Force -Path $logs
$ssh = (Get-Command ssh -ErrorAction SilentlyContinue).Source
if (-not $ssh) { Write-Host 'ssh not found (Windows OpenSSH client is needed).'; exit 1 }

function Confirm-Parked {
    $a = Read-Host 'Car parked and CarLife stopped/allowed for this step? Type y to continue'
    if ($a -ne 'y') { Write-Host 'Cancelled.'; exit 2 }
}

function New-Log([string] $name) {
    $file = Join-Path $logs ("{0}-{1}.txt" -f $name, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Set-Content -LiteralPath $file -Value ("# {0}  {1}" -f $name, (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding UTF8
    return $file
}

function Write-Both([string] $file, [string] $text) {
    Write-Host $text
    Add-Content -LiteralPath $file -Value $text -Encoding UTF8
}

# One ssh login; every output line is shown and saved. The remote command must not contain double quotes.
function Invoke-Car([string] $file, [string] $remote) {
    Write-Host 'Logging in to the car (enter the password once)...'
    & $ssh -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 root@127.0.0.1 $remote 2>&1 |
        ForEach-Object { Write-Both $file ($_.ToString()) }
    Write-Both $file ("# ssh exit code: {0}" -f $LASTEXITCODE)
}

if (-not $Step) {
    Write-Host '1 upload      upload the 6 CarLife files (CarLife must be Force Stopped)'
    Write-Host '2 prep        create the 5 enable files and show installed file sizes'
    Write-Host '3 collect     after a test: audio, latency, knob and supervisor logs'
    Write-Host '4 wifi-check  read-only network facts (hotspot, leases, arp, tools)'
    Write-Host '5 wifi-scan   probe the phone''s CarLife ports (phone on the car hotspot, CarLife+ in WLAN mode)'
    Write-Host '6 sniff       listen on the car hotspot for 60 s while you press Start on the phone, probe in between'
    Write-Host '7 wifi-try    first wireless CarLife session (60 s): connects to the phone over the hotspot'
    Write-Host '8 wifi-on     menu start uses wireless: create carlink-wifi-enable plus audio, knob and keep-log switches'
    Write-Host '9 wifi-off    menu start back to USB: remove carlink-wifi-enable'
    Write-Host '10 dhcp-info  read-only: what the car hotspot hands to phones (DHCP config, leases, routes)'
    Write-Host '11 dhcp-who   read-only: which program starts dhcpd and writes its config'
    Write-Host '12 dhcp-fix   CHANGES the hotspot: stop announcing gateway 0.0.0.0 (backup kept; undone by 13 or a restart)'
    Write-Host '13 dhcp-undo  put the original hotspot DHCP config back'
    Write-Host '14 dhcp-start start the hotspot DHCP server (dhcpd) if it is not running'
    Write-Host '15 wifi-hw    read-only: Wi-Fi chip, driver, interfaces, Wi-Fi Direct and station (client) support'
    Write-Host '16 wifi-drv   read-only: Wi-Fi driver files, mount options, wpa_supplicant version and help'
    Write-Host '17 fetch      copy one car file (binary safe) to carlink-logs\fetched (no password files)'
    Write-Host '18 conn-libs  read-only: which connectivity library handles the Wi-Fi modes'
    Write-Host '19 bss-get    ask the Wi-Fi driver which role uap0 has (hotspot or client); changes nothing'
    $choice = Read-Host 'Step number'
    $Step = @{ '1' = 'upload'; '2' = 'prep'; '3' = 'collect'; '4' = 'wifi-check'; '5' = 'wifi-scan'; '6' = 'sniff'; '7' = 'wifi-try'; '8' = 'wifi-on'; '9' = 'wifi-off'; '10' = 'dhcp-info'; '11' = 'dhcp-who'; '12' = 'dhcp-fix'; '13' = 'dhcp-undo'; '14' = 'dhcp-start'; '15' = 'wifi-hw'; '16' = 'wifi-drv'; '17' = 'fetch'; '18' = 'conn-libs'; '19' = 'bss-get' }[$choice]
    if (-not $Step) { Write-Host 'Unknown choice.'; exit 2 }
}

switch ($Step) {
    'upload' {
        Confirm-Parked
        $file = New-Log 'upload'
        $script = Join-Path $root 'build\live-input-candidate\carlife-menu\update-carlife-menu.ps1'
        & powershell -NoProfile -ExecutionPolicy Bypass -File $script 2>&1 | ForEach-Object { Write-Both $file ($_.ToString()) }
    }
    'prep' {
        Confirm-Parked
        $file = New-Log 'prep'
        Invoke-Car $file ('cd /dev/shmem; for f in readthread audio audio-play keep-log watch; do : > carlink-$f-enable; done; ' +
            'ls -l carlink-*-enable; ls -l /opt/aa/nbt-aad /opt/aa/carlink/*.elf /opt/aa/carlink/*.sh /dev/shmem/clmenu-tcpprobe.elf; date')
    }
    'collect' {
        $file = New-Log 'collect'
        Invoke-Car $file ('cd /dev/shmem; date; ls -l carlink-*.log; ' +
            'echo ==AUDIO; grep -E ''Audio player|Audio:|Audio format'' carlink-*.log; ' +
            'echo ==LATENCY; grep -E ''Latency|Input sent'' carlink-*.log | tail -n 200; ' +
            'echo ==KNOB; grep -E ''Knob:|Fast hide|Emitted|Summary'' carlink-*.log | tail -n 200; ' +
            'echo ==SUPERVISOR; tail -n 150 carlink-supervisor.log; ' +
            'echo ==SESSION; tail -n 250 carlink-session.log; ' +
            'echo ==KEEP; ls -l /opt/aa/carlink/keep/')
    }
    'wifi-check' {
        $file = New-Log 'wifi-check'
        Invoke-Car $file ('date; ifconfig uap0; echo ==LEASES; grep -E ''lease|hostname|ends'' /var/opt/conn/dhcpd.uap0.leases | tail -n 40; ' +
            'echo ==ARP; arp -an; echo ==TOOLS; which tcpdump nc telnet ftp wget curl; ls /dev/bpf*; ' +
            'ls -l /dev/shmem/clmenu-tcpprobe.elf')
    }
    'wifi-scan' {
        if ($Phone -lt 2 -or $Phone -gt 254) {
            $Phone = [int](Read-Host 'Last number of the phone IP, 172.16.224.N (see wifi-check leases); N')
        }
        if ($Phone -lt 2 -or $Phone -gt 254) { Write-Host 'N must be 2..254.'; exit 2 }
        Confirm-Parked
        $file = New-Log ("wifi-scan-{0}" -f $Phone)
        Invoke-Car $file ("cd /dev/shmem; date; arp -an; ./clmenu-tcpprobe.elf --scan 172.16.224.{0}; echo probe-exit=`$?" -f $Phone)
    }
    'sniff' {
        if ($Phone -lt 2 -or $Phone -gt 254) {
            $Phone = [int](Read-Host 'Last number of the phone IP, 172.16.224.N; N')
        }
        if ($Phone -lt 2 -or $Phone -gt 254) { Write-Host 'N must be 2..254.'; exit 2 }
        Confirm-Parked
        $file = New-Log ("sniff-{0}" -f $Phone)
        $elf = Join-Path $root 'build\live-input-candidate\build\carlife_qnx_sniff.elf'
        $bytes = [System.IO.File]::ReadAllBytes($elf)
        Write-Both $file ("sniffer {0} bytes" -f $bytes.Length)
        # Upload the listener to /dev/shmem through stdin, then: listen 60 s in the background, probe after 20 s, print all.
        $remote = 'cat > /dev/shmem/clmenu-sniff.elf; chmod +x /dev/shmem/clmenu-sniff.elf; cd /dev/shmem; ls -l clmenu-sniff.elf; ' +
            '(./clmenu-sniff.elf uap0 60 > clmenu-sniff.out 2>&1 &); sleep 2; ' +
            'echo ==== NOW press the Start-connection button on the phone. Probe follows in 20 s.; sleep 20; ' +
            ('./clmenu-tcpprobe.elf --scan 172.16.224.{0}; ' -f $Phone) +
            'echo ==== keep the phone trying, listening until 60 s; sleep 42; echo ==SNIFF; cat clmenu-sniff.out'
        Write-Host 'Logging in to the car (enter the password once)...'
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $ssh
        $psi.Arguments = '-o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 root@127.0.0.1 "' + $remote + '"'
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $p = [System.Diagnostics.Process]::Start($psi)
        $in = $p.StandardInput.BaseStream
        $in.Write($bytes, 0, $bytes.Length)
        $in.Flush()
        $in.Close()
        while ($null -ne ($line = $p.StandardOutput.ReadLine())) { Write-Both $file $line }
        $p.WaitForExit()
        Write-Both $file ("# ssh exit code: {0}" -f $p.ExitCode)
    }
    'wifi-try' {
        if ($Phone -lt 2 -or $Phone -gt 254) {
            $Phone = [int](Read-Host 'Last number of the phone IP, 172.16.224.N; N')
        }
        if ($Phone -lt 2 -or $Phone -gt 254) { Write-Host 'N must be 2..254.'; exit 2 }
        Confirm-Parked
        $file = New-Log ("wifi-try-{0}" -f $Phone)
        $cap = [System.IO.File]::ReadAllBytes((Join-Path $root 'build\live-input-candidate\build\carlife_qnx_usb_capture.elf'))
        $lau = [System.IO.File]::ReadAllBytes((Join-Path $root 'build\live-input-candidate\tools\carlife_usb_visible.sh'))
        Write-Both $file ("receiver {0} bytes, launcher {1} bytes" -f $cap.Length, $lau.Length)
        # Both files go through stdin (dd takes exactly their sizes); the session itself runs the normal live display.
        $remote = ('cd /dev/shmem; dd bs=1 count={0} of=clmenu-wcap.elf 2>/dev/null; dd bs=1 count={1} of=clmenu-wlaunch.sh 2>/dev/null; ' -f $cap.Length, $lau.Length) +
            'chmod +x clmenu-wcap.elf; ls -l clmenu-wcap.elf clmenu-wlaunch.sh; ' +
            'cp /opt/aa/carlink/carlife_qnx_mmr_replay.elf /opt/aa/carlink/carlife_visible_first_frame.sh /dev/shmem/; ' +
            '. /etc/profile >/dev/null 2>&1; [ -f /root/.profile ] && . /root/.profile >/dev/null 2>&1; ' +
            'for f in /opt/sys/lib/libilmClient.cpp-ne.so.1 /opt/sys/lib/*/libilmClient.cpp-ne.so.1 /opt/*/lib/libilmClient.cpp-ne.so.1 /usr/lib/libilmClient.cpp-ne.so.1; do [ -e $f ] && LD_LIBRARY_PATH=${f%/*}:$LD_LIBRARY_PATH && echo ilm-lib=$f; done; export LD_LIBRARY_PATH; echo LD_LIBRARY_PATH=$LD_LIBRARY_PATH; ' +
            'echo ==CONTEXTS; ls /pps/services/multimedia/renderer/context; ' +
            'L=/opt/sys/bin/LayerManagerControl; echo ==LAYER-BEFORE; $L get layer 0x80000000 2>&1; $L get surface 256 2>&1; $L get surface 1510 2>&1; ' +
            'echo ==RESTORE-iDrive-layer; $L set layer 0x80000000 render order 0x100; $L set surface 256 visibility 1; ' +
            'echo ==LAYER-AFTER; $L get layer 0x80000000 2>&1; $L get surface 256 2>&1; ' +
            'echo ==== NOW press Start connection on the phone. The car waits up to 30 s for it.; ' +
            ('CARLINK_WIFI_PHONE=172.16.224.{0} sh ./clmenu-wlaunch.sh ./clmenu-wcap.elf ./carlife_qnx_mmr_replay.elf 2>&1; echo launch-exit=$?; ' -f $Phone) +
            'echo ==CONTEXTS-AFTER; ls /pps/services/multimedia/renderer/context'
        Write-Host 'Logging in to the car (enter the password once)...'
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $ssh
        $psi.Arguments = '-o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 root@127.0.0.1 "' + $remote + '"'
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $p = [System.Diagnostics.Process]::Start($psi)
        $in = $p.StandardInput.BaseStream
        $in.Write($cap, 0, $cap.Length)
        $in.Write($lau, 0, $lau.Length)
        $in.Flush()
        $in.Close()
        while ($null -ne ($line = $p.StandardOutput.ReadLine())) { Write-Both $file $line }
        $p.WaitForExit()
        Write-Both $file ("# ssh exit code: {0}" -f $p.ExitCode)
    }
    'wifi-on' {
        Confirm-Parked
        $file = New-Log 'wifi-on'
        Invoke-Car $file ('cd /dev/shmem; for f in wifi audio-play keep-log watch; do : > carlink-$f-enable; done; rm -f carlink-wifi-phone; ' +
            'ls -l carlink-*-enable; ls -l /opt/aa/nbt-aad /opt/aa/carlink/carlife_qnx_usb_capture.elf /opt/aa/carlink/carlife_usb_visible.sh; date')
    }
    'wifi-off' {
        $file = New-Log 'wifi-off'
        Invoke-Car $file 'cd /dev/shmem; rm -f carlink-wifi-enable carlink-wifi-phone; ls -l carlink-*-enable; date'
    }
    'dhcp-info' {
        $file = New-Log 'dhcp-info'
        Invoke-Car $file ('date; echo ==CONF; cat /var/opt/conn/dhcpd.uap0.conf; echo ==LEASES; tail -n 60 /var/opt/conn/dhcpd.uap0.leases; ' +
            'echo ==ROUTES; netstat -rn 2>&1 | grep -v ::; echo ==IFCONFIG; ifconfig -a 2>&1 | grep -E ''^[a-z]|inet ''; ' +
            'echo ==DHCPD; pidin -p dhcpd arg 2>/dev/null; ls -l /var/opt/conn/')
    }
    'dhcp-who' {
        $file = New-Log 'dhcp-who'
        Invoke-Car $file ('date; echo ==PROC; pidin -p dhcpd family 2>&1; pidin -F ''%a %b %N'' 2>/dev/null | grep -i -E ''dhcp|conn|wifi|wlan|uap|carplay|iap|hostap|nw'' ; ' +
            'echo ==WHO-WRITES; for d in /opt/conn/bin /opt/conn/lib /opt/conn/etc /opt/conn/cfg /etc /etc/system /opt/sys/etc /opt/sys/bin /usr/sbin /opt/wlan /opt/conn; do ' +
            '[ -d $d ] || continue; for f in $d/*; do [ -f $f ] && grep -l -e dhcpd.uap0 -e ''option routers'' $f 2>/dev/null; done; done; ' +
            'echo ==SCRIPTS; for f in $(grep -l -e dhcpd.uap0 /etc/* /opt/conn/bin/* /opt/conn/etc/* 2>/dev/null); do echo ---- $f; grep -n -e dhcpd -e routers $f 2>/dev/null | grep -v -i pass | head -20; done; ' +
            'echo ==HOSTAPD; ls -l /opt/conn/bin 2>&1 | head -60')
    }
    'dhcp-fix' {
        Confirm-Parked
        $file = New-Log 'dhcp-fix'
        Invoke-Car $file ('C=/var/opt/conn/dhcpd.uap0.conf; B=/dev/shmem/dhcpd.uap0.conf.orig; ' +
            'restart_dhcpd() { old=$(cat /tmp/dhcpd.pid.uap0 2>/dev/null); echo old dhcpd pid=$old; [ x$old != x ] && kill $old; ' +
            'n=0; while [ $n -lt 20 ]; do sleep 1; n=$((n+1)); new=$(cat /tmp/dhcpd.pid.uap0 2>/dev/null); ' +
            'if [ x$new != x ] && [ x$new != x$old ] && kill -0 $new 2>/dev/null; then echo dhcpd restarted by the car after $n s, pid $new; return 0; fi; done; ' +
            'echo the car did not restart dhcpd in 20 s: starting it with the same arguments; ' +
            'D=; for d in /opt/conn/bin/dhcpd /usr/sbin/dhcpd /sbin/dhcpd /bin/dhcpd /opt/conn/sbin/dhcpd; do [ -x $d ] && D=$d && break; done; echo binary=$D; ' +
            'rm -f /tmp/dhcpd.pid.uap0; [ x$D != x ] && $D -cf $C -lf /var/opt/conn/dhcpd.uap0.leases -pf /tmp/dhcpd.pid.uap0 uap0 > /dev/shmem/dhcpd-start.log 2>&1; echo start-exit=$?; sleep 2; cat /dev/shmem/dhcpd-start.log; ' +
            'echo now pid=$(cat /tmp/dhcpd.pid.uap0 2>/dev/null); }; ' +
            'date; [ -f $B ] || cp $C $B; echo ==BACKUP; ls -l $B; echo ==BEFORE; cat $C; ' +
            'grep -v ''option routers'' $B > /dev/shmem/dhcpd.new && cp /dev/shmem/dhcpd.new $C; echo ==AFTER; cat $C; ' +
            'restart_dhcpd; echo ==PROC; pidin -p dhcpd arg 2>&1; echo ==CONF-NOW; cat $C')
    }
    'dhcp-start' {
        $file = New-Log 'dhcp-start'
        Invoke-Car $file ('C=/var/opt/conn/dhcpd.uap0.conf; B=/dev/shmem/dhcpd.uap0.conf.orig; ' +
            'restart_dhcpd() { old=$(cat /tmp/dhcpd.pid.uap0 2>/dev/null); echo old dhcpd pid=$old; [ x$old != x ] && kill $old; ' +
            'n=0; while [ $n -lt 20 ]; do sleep 1; n=$((n+1)); new=$(cat /tmp/dhcpd.pid.uap0 2>/dev/null); ' +
            'if [ x$new != x ] && [ x$new != x$old ] && kill -0 $new 2>/dev/null; then echo dhcpd restarted by the car after $n s, pid $new; return 0; fi; done; ' +
            'echo the car did not restart dhcpd in 20 s: starting it with the same arguments; ' +
            'D=; for d in /opt/conn/bin/dhcpd /usr/sbin/dhcpd /sbin/dhcpd /bin/dhcpd /opt/conn/sbin/dhcpd; do [ -x $d ] && D=$d && break; done; echo binary=$D; ' +
            'rm -f /tmp/dhcpd.pid.uap0; [ x$D != x ] && $D -cf $C -lf /var/opt/conn/dhcpd.uap0.leases -pf /tmp/dhcpd.pid.uap0 uap0 > /dev/shmem/dhcpd-start.log 2>&1; echo start-exit=$?; sleep 2; cat /dev/shmem/dhcpd-start.log; ' +
            'echo now pid=$(cat /tmp/dhcpd.pid.uap0 2>/dev/null); }; ' +
            'date; echo ==BEFORE; pidin -p dhcpd arg 2>&1; ' +
            'if pidin -p dhcpd arg 2>/dev/null | grep -q dhcpd.uap0; then echo dhcpd already running; else ' +
            'D=; for d in /usr/sbin/dhcpd /opt/conn/bin/dhcpd /sbin/dhcpd; do [ -x $d ] && D=$d && break; done; rm -f /tmp/dhcpd.pid.uap0; ' +
            '$D -cf $C -lf /var/opt/conn/dhcpd.uap0.leases -pf /tmp/dhcpd.pid.uap0 uap0 > /dev/shmem/dhcpd-start.log 2>&1; echo start-exit=$?; sleep 2; cat /dev/shmem/dhcpd-start.log; fi; ' +
            'echo ==AFTER; pidin -p dhcpd arg 2>&1; echo ==CONF; cat $C')
    }
    'dhcp-undo' {
        $file = New-Log 'dhcp-undo'
        Invoke-Car $file ('C=/var/opt/conn/dhcpd.uap0.conf; B=/dev/shmem/dhcpd.uap0.conf.orig; ' +
            'restart_dhcpd() { old=$(cat /tmp/dhcpd.pid.uap0 2>/dev/null); echo old dhcpd pid=$old; [ x$old != x ] && kill $old; ' +
            'n=0; while [ $n -lt 20 ]; do sleep 1; n=$((n+1)); new=$(cat /tmp/dhcpd.pid.uap0 2>/dev/null); ' +
            'if [ x$new != x ] && [ x$new != x$old ] && kill -0 $new 2>/dev/null; then echo dhcpd restarted by the car after $n s, pid $new; return 0; fi; done; ' +
            'echo the car did not restart dhcpd in 20 s: starting it with the same arguments; ' +
            'D=; for d in /opt/conn/bin/dhcpd /usr/sbin/dhcpd /sbin/dhcpd /bin/dhcpd /opt/conn/sbin/dhcpd; do [ -x $d ] && D=$d && break; done; echo binary=$D; ' +
            'rm -f /tmp/dhcpd.pid.uap0; [ x$D != x ] && $D -cf $C -lf /var/opt/conn/dhcpd.uap0.leases -pf /tmp/dhcpd.pid.uap0 uap0 > /dev/shmem/dhcpd-start.log 2>&1; echo start-exit=$?; sleep 2; cat /dev/shmem/dhcpd-start.log; ' +
            'echo now pid=$(cat /tmp/dhcpd.pid.uap0 2>/dev/null); }; ' +
            'date; if [ -f $B ]; then cp $B $C; echo restored from $B; else echo no backup: nothing to restore; fi; cat $C; ' +
            'restart_dhcpd; pidin -p dhcpd arg 2>&1')
    }
    'wifi-hw' {
        $file = New-Log 'wifi-hw'
        # Read-only. Lines that look like secrets (pass, psk, key, secret) are filtered out on the car.
        Invoke-Car $file ('date; S=''pass|psk|key|secret|pin''; echo ==IFCONFIG; ifconfig -a 2>&1 | grep -E ''^[a-z]|inet |status''; ' +
            'echo ==IO-PKT; pidin -F ''%a %N'' 2>/dev/null | grep -i -E ''io-pkt|wpa|hostap|wlan|mlan|wifi|wl|p2p''; for p in $(pidin -F ''%a %N'' 2>/dev/null | grep -i io-pkt | sed ''s/^ *\([0-9]*\).*/\1/''); do pidin -p $p arg 2>&1; done; ' +
            'echo ==DLL; ls /lib/dll /usr/lib/dll /opt/conn/lib/dll /lib/dll/pci 2>/dev/null | grep -i -E ''mlan|uap|wfd|sd8|88w|wifi|wlan|wl|bcm|ath|marvell|brcm''; ' +
            'echo ==TOOLS; for t in wpa_supplicant wpa_cli hostapd hostapd_cli iw iwconfig wl wpa_passphrase dhcp.client dhcpcd udhcpc; do for d in /usr/sbin /usr/bin /sbin /bin /opt/conn/bin /opt/conn/sbin; do [ -x $d/$t ] && echo found $d/$t; done; done; ' +
            'echo ==CONNBIN; ls /opt/conn/bin; echo ==CONNCFG; ls -l /var/opt/conn /var/opt/conn/carplay /opt/conn/etc 2>&1; ' +
            'echo ==WFD-DHCP; cat /var/opt/conn/dhcpd.wfd0.conf; ' +
            'echo ==WIFIDIR; grep -v -i -E $S /var/opt/conn/wifidir_init.conf; echo ==PSS; grep -i -E ''mode|sta|client|p2p|wfd|wlan|wifi|band|chan|iface|interface'' /var/opt/conn/pss_config.cfg | grep -v -i -E $S; ' +
            'echo ==PF-NOT-UAP0; pfctl -sr 2>&1 | grep -v uap0; echo ==DHCPCLIENT; pidin -p dhcp.client arg 2>&1')
    }
    'wifi-drv' {
        $file = New-Log 'wifi-drv'
        Invoke-Car $file ('date; S=''pass|psk|key|secret|pin''; echo ==IOPKT-MEM; pidin -p 8203 mem 2>&1 | grep -i -E ''devn|wlan|mlan|8787|mrvl|marvell|\.so''; ' +
            'echo ==DLLS; for d in /lib/dll /usr/lib/dll /opt/conn/lib /opt/conn/lib/dll /opt/sys/lib /proc/boot /lib; do ls $d 2>/dev/null | grep -i -E ''devn|wlan|mlan|8787|mrvl|marvell|sd8|wifi'' | sed s,^,$d/,; done; ' +
            'echo ==IONET; ls -l /dev/io-net 2>&1; ls /dev | grep -i -E ''wl|mlan|uap|wfd|mwu|sdio''; ' +
            'echo ==WPA-V; /opt/conn/bin/wpa_supplicant -v 2>&1; echo ==WPA-H; /opt/conn/bin/wpa_supplicant -h 2>&1; ' +
            'echo ==MWU; grep -v -i -E $S /opt/conn/bin/mwu_dev.conf; echo ==WFD-DEFAULTS; grep -v -i -E $S /opt/conn/bin/wifidirect_defaults.conf; ' +
            'echo ==CONNCFG; grep -v -i -E $S /opt/conn/bin/Connectivity.cfg | grep -i -E ''wlan|wifi|mlan|uap|wfd|sta|mode|8787|mount|driver|iface''; ' +
            'echo ==PSS-NET; grep -i -E ''^NetworkingService|^ConnMngr|^WlanConn|^Wlan'' /var/opt/conn/pss_config.cfg | grep -v -i -E $S; ' +
            'echo ==MWUD; pidin -p mwud arg 2>&1; pidin -p wifidir_app arg 2>&1; pidin -p wpa_supplicant arg 2>&1')
    }
    'fetch' {
        $path = Read-Host 'Car file path to copy (for example /lib/dll/devnp-xyz.so)'
        if ($path -notmatch '^/[A-Za-z0-9_./+-]+$' -or $path -match 'iap2|WFDPass|WFDWps|pss_config|shadow|passwd|\.\.') { Write-Host 'Refused: bad path or a password file.'; exit 2 }
        $out = Join-Path $logs 'fetched'
        $null = New-Item -ItemType Directory -Force -Path $out
        $dest = Join-Path $out (($path.TrimStart('/')) -replace '/', '__')
        Write-Host 'Logging in to the car (enter the password once)...'
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $ssh
        $psi.Arguments = '-o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 root@127.0.0.1 "cat ' + $path + '"'
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $p = [System.Diagnostics.Process]::Start($psi)
        $fs = [System.IO.File]::Create($dest)
        $p.StandardOutput.BaseStream.CopyTo($fs)
        $fs.Close()
        $p.WaitForExit()
        $file = $dest
        Write-Host ("copied {0} bytes, ssh exit code {1}" -f (Get-Item -LiteralPath $dest).Length, $p.ExitCode)
    }
    'conn-libs' {
        $file = New-Log 'conn-libs'
        Invoke-Car $file ('date; echo ==LIBS; ls -l /opt/conn/lib; echo ==HITS; for f in /opt/conn/lib/*.so* /opt/conn/bin/*; do [ -f $f ] && grep -l -e mountparam -e num_client -e NetworkingService_ -e WlanDefault $f 2>/dev/null; done; ' +
            'echo ==DATA; ls -l /opt/conn/data /tmp/scp 2>&1')
    }
    'bss-get' {
        Confirm-Parked
        $file = New-Log 'bss-get'
        $elf = [System.IO.File]::ReadAllBytes((Join-Path $root 'build\live-input-candidate\build\carlife_qnx_bssrole.elf'))
        Write-Both $file ("role tool {0} bytes" -f $elf.Length)
        $remote = ('cd /dev/shmem; dd bs=1 count={0} of=clmenu-bssrole.elf 2>/dev/null; chmod +x clmenu-bssrole.elf; ls -l clmenu-bssrole.elf; ' -f $elf.Length) +
            'echo ==BEFORE; pidin -p io-pkt-v6-hc 2>&1 | grep -c io-pkt; ifconfig uap0 2>&1; ' +
            'echo ==GET; ./clmenu-bssrole.elf --get uap0; echo get-exit=$?; ' +
            'echo ==GET-G; ./clmenu-bssrole.elf --get-g uap0; echo get-g-exit=$?; ' +
            'echo ==SLOG; sloginfo 2>/dev/null | grep -i -E ''mv8787|woal|drvspec|bss_role|role|private|kauth|perm'' | tail -n 25; ' +
            'echo ==AFTER; pidin -p io-pkt-v6-hc 2>&1 | grep -c io-pkt; ifconfig uap0 2>&1; arp -an 2>&1 | grep uap0'
        Write-Host 'Logging in to the car (enter the password once)...'
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $ssh
        $psi.Arguments = '-o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 root@127.0.0.1 "' + $remote + '"'
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $p = [System.Diagnostics.Process]::Start($psi)
        $in = $p.StandardInput.BaseStream
        $in.Write($elf, 0, $elf.Length)
        $in.Flush()
        $in.Close()
        while ($null -ne ($line = $p.StandardOutput.ReadLine())) { Write-Both $file $line }
        $p.WaitForExit()
        Write-Both $file ("# ssh exit code: {0}" -f $p.ExitCode)
    }
    default { Write-Host "Unknown step '$Step'. Use upload, prep, collect, wifi-check or wifi-scan."; exit 2 }
}
Write-Host ''
Write-Host "Saved: $file"
