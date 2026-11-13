# carlife 状态（更新 2026-11-12 23:50）

## 已在车上验证
- 连续投屏稳定：读线程 + 补丁 1–5 下画面 11 分钟无断；补丁触发多次均自动恢复。
- **音频能从车机喇叭出声**（车机时间 14:10）。路径：手机 CarLife 音频通道 3 → 接收程序解析 → 环形缓冲 → 播放线程 → 车机自带 libasound 垫片（/opt/mm/lib/libasound/libasound.so）→ 声卡 card 1 device 1。前提：监督脚本先 `audioconn -b 0xb8` 建立浏览器音源路由，会话结束 `audioconn -r 0xb8` 释放（均 status 0）。
- 音频格式：44100 Hz、立体声、16 位。
- **抖动缓冲（2026-11-12 23:23 实测）**：播放设备自己先吞约 115 ms，150 ms 预缓冲太小，每 10 s 断流 61 次；改成 600 ms 后整个会话累计 13 次，最长空 ~100 ms。日志行：`Audio player: queue A..B bytes, ran dry N times (longest empty >= X ms)`。可用环境变量 CARLINK_AUDIO_PREBUFFER_MS 调（上限约 680 ms）。
- **旋钮在菜单启动的会话里已通**（23:23）：监听 `carlife_qnx_most_watch.elf --watch 0 --emit` → 输入文件 → 通道 6。日志 `Knob: ROTARY delta=±N -> CarLife ±N`、PUSH→OK、BACK→BACK、TILT_RIGHT→RIGHT（已见一次）、MENU 不映射；iDrive 隐藏时 39 条被拦下，没有发给手机；169 条动作 0 写失败。需开关文件 carlink-watch-enable。
- **MENU 隐藏画面**：LayerManager 的 `get surface 1510` 显示 `on layer:` 为空时画面仍在，必须 `set surface 1510 visibility 0` 才消失。上一版（v15）实测：MENU 后画面消失，延迟约 1 s，iDrive 立即可用；MENU 时声音仍卡一下（平时播放不卡）；旋钮反应对但有一点点延迟（用户 23:26 反馈"勉强可以用"）。

## v16（离线完成并通过测试，尚未上车）
- 旋钮监听程序在 MENU/MEDIA 按下的瞬间直接 `set surface 1510 visibility 0` 并写 hide 文件（`fast_hide`，环境变量 CARLINK_WATCH_SHOWN、CARLINK_FAST_HIDE_LMC 由监督脚本传入）→ 目标：消除约 1 s 延迟。
- 音频断流后重新缓冲门限 = 预缓冲的一半（300 ms，之前 600 ms），断流前 60 次记带时间戳（用 `time()`）→ 目标：MENU 时不卡。
- 监督脚本 nbt-aad 21920 字节；会话中每 10 s 备份日志并 sync（`/opt/aa/carlink/keep/`）。
- 离线测试：监督脚本 136 项通过；接收程序 usb-resync 64 项；探针 tcp-probe 11 项。完整套件唯一已知旧问题：most-watch 的 "mutation tilt-direction was not caught"（需要 UPSTREAM_SRC）。
- ELF（Linux 构建，见下）：capture 29808 字节 SHA256 072623A6…，most_watch 16036 字节 82732164…，tcp_probe 5884 字节；已写入电脑 `build\live-input-candidate\build\`。
- 上传：`carlife-menu\update-carlife-menu.ps1`（v4，现在传 6 个文件：nbt-aad、capture、显示脚本、旋钮监听、watch-env-bisect.sh、clmenu-tcpprobe.elf）。

## 构建（可以在云端直接编译）
- 云端装 clang-15 / lld-15（apt），用 `--target=armv7-none-eabi -mfpu=neon -mfloat-abi=softfp` + build-qnx-probes.ps1 里的同样参数与链接步骤。
- 验证：这样编出的 usb_status、cp_purge、ctl、udp_probe、mmr_replay 与用户 Windows 工具链（MATLAB LLVM）编出的文件逐字节相同；用户工具链默认就是 softfp + NEON（不带这两个参数，capture 会因为 `= {0}` 产生 `__aeabi_memclr4/8` 而链接失败）。
- 脚本：云端 scratchpad 的 build-linux.sh（把 ps1 翻译成 bash）；check-build 通过后把 ELF 写回电脑并核对大小。

## 关键事实
- 声卡库垫片依赖 liba2i_client.so，在 /opt/mm/lib：接收程序先按全路径预加载，监督脚本在播放模式把 /opt/mm/lib 追加到 LD_LIBRARY_PATH 末尾。
- snd_pcm_channel_params_t 偏移（从车机 libasound.so.2 反汇编）：channel 0、mode 4、interleave 0x18、format 0x1c（S16_LE=4）、rate 0x20、voices 0x24、start_mode 0x1d8、stop_mode 0x1dc、frag_size 0x1e8、frags_min 0x1ec、frags_max 0x1f0，其余全零。打开模式 1=PLAYBACK；frag 4096×5。
- /dev/shmem 不能建目录；锁用 `set -C; : > file`。车机没有 scp、cksum、ssh、head；传文件走电脑 ssh。
- **旋钮监听退出崩溃（已修）**：exit() 会跑 DSI 库退出处理而崩；改为 `_exit()`。
- 开关文件（/dev/shmem/，重启后丢）：carlink-readthread-enable、carlink-audio-enable、carlink-audio-play-enable、carlink-keep-log-enable、carlink-watch-enable。
- 车机重启过两次（一次空闲，一次旋钮监听退出崩溃之后），原因未明。

## 无线 CarLife 现状（当时）
- 车机热点已在运行：uap0 = 172.16.224.1/24，DHCP 租约 `/var/opt/conn/dhcpd.uap0.leases`；热点配置在 /var/opt/conn/carplay/iap2.cfg、iap2_Wifi.cfg（含密码，只让用户自己读，不要贴出来）。
- 包过滤 pf 默认全丢弃。uap0 上入站 TCP 仅放行 5000/5001/5010/6000/6030/6100/6200/7000/7001/7100 和 49512–65535；出站源端口同样；入站 UDP 7999 被拦。方案：不改防火墙，车机本地端口绑 49512 以上去连手机。
- 后续进展见 wireless-carlife-status.md 与 ../wireless-phone-hotspot.md。

## 待做
- v16 实车测试（MENU 延迟、MENU 声音、旋钮延迟）。
- 延迟测量（毫秒时间戳）；音量/静音；隐藏画面时是否释放音频路由；其他音源是否恢复。
- 查重启原因（用 `/opt/aa/carlink/keep/` 的 10 s 备份）。
- 触控。
- 每次上车须用户授权、车停稳；测完 Force Stop、拔手机重插再启动；必要时重启车机恢复原厂 CarPlay。
- 往电脑提交文件后必须核对大小（曾出现旧内容）。
