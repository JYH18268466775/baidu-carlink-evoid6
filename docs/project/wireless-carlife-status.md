# 无线 CarLife 现状（2026-11-13 18:00，已实车跑通）

## 结论
- 手机（三星 S25 Ultra，三星车联）连车机热点 uap0（172.16.224.x）+ 蓝牙，按"开始连接"后，**手机作为服务端开 6 个 CarLife 端口** 7240/8240/9240/9241/9242/9340（其他端口回 RST）。车机作 TCP 客户端去连，握手与 USB 完全相同（协议 1.0、手机编码 1280×720@30）。手机等不到车机时显示"连接超时"。
- 防火墙不用改：车机从本地端口 ≥49512（50400 起）出去，原厂 pf 规则放行。
- 17:39 第一次 60 秒投屏成功（472 帧）；17:56 从 iDrive 菜单启动的无线会话成功：画面、声音（整段 0 次断流）、旋钮（旋转/按下/左倾）、MENU 快速隐藏都正常，结束后音频路由释放、退出码 0。
- 旋钮延迟：按键写给手机后，界面变化的大帧约 60–130 ms 到达车机（如 OK 后 67 ms 收到 73 KB 帧）。

## 实现
- 接收程序 carlife_qnx_usb_capture.c：`--stream-wifi 172.16.224.N`（6 个 socket，各通道记录与 AOA 内层相同、无外层 8 字节头；按键走 9340），首个端口最多等 30 s；`--wifi-wait IP[,IP]`（CARLINK_WIFI_WAIT 秒内找到 7240 开着的手机并打印地址，不发数据）。
- 启动脚本 carlife_usb_visible.sh：`CARLINK_WIFI_PHONE=IP` 时跳过 USB 检测，用 `--stream-wifi`。
- 监督脚本 nbt-aad（24533 字节）：开关 `/dev/shmem/carlink-wifi-enable`（或 CARLINK_WIFI=1）；手机地址从 `arp -an` 的 uap0 条目自动取（或 `/dev/shmem/carlink-wifi-phone` 指定）；会话结束后不要求拔插，回到等待，手机再按"开始连接"即重连。
- 测试：模拟手机 wifi-capture.sh 16 项；监督脚本 162 项（含 26 项无线）。

## 坑
- SSH 非交互执行时没有 LD_LIBRARY_PATH，LayerManagerControl 报找不到 libilmClient.cpp-ne.so.1 → 需先 `. /etc/profile`（/opt/sys/lib）。
- QNX 的 POLLOUT = 2（4 是 POLLRDBAND），第一版探针因此全部显示"无应答"。
- 车机 /dev/bpf 可用（无 tcpdump），抓包工具 carlife_qnx_sniff.elf（uap0，只读）。
- 编译：云端 clang-15 + `-mfpu=neon -mfloat-abi=softfp` 与用户 Windows 工具链逐字节一致。

## 电脑上的一键步骤（项目根目录，结果在 carlink-logs\）
car-1-upload（7 个文件）、car-3-collect、car-4-wifi-check、car-5-wifi-scan、car-6-sniff、car-7-wifi-try（60 s 单独试）、car-8-wifi-on（无线+声音+旋钮+日志开关）、car-9-wifi-off。

## 待做
- 用户体验反馈（画面流畅度、声音、延迟）；长时间稳定性。
- 自动化：手机每次要手动按"开始连接"；是否能由车机触发（蓝牙侧）未知。
- 热点上的 iPhone（172.16.224.101）每 5 s 会被探一次 7240（无害）。
