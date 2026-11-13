# 无线 CarLife 调研与设计（2026-11-12，只读调研，未上车）

## 1. 结论
可行性中等偏高：车机侧协议和有线几乎一样，只是把 AOA 的多路复用换成 6 条 TCP 连接。真正的未知数在车机的网络（Wi‑Fi）和手机端怎么发起无线连接。

## 2. 协议（来自 Apollo CarLife-Android-Vehicle 源码，WifiConnectManager / ConnectManager / CommonParams）
- 车机端是 TCP **客户端**，手机端是服务器。
- 手机先在局域网发 **UDP 广播到 7999**，车机收到后记下手机 IP（`SERVER_WIFI_URL`）。
- 车机随后连 6 个 TCP 口：主控 7240、视频 8240、音频 9240、TTS 9241、语音 9242、触摸/按键 9340。
- 这 6 路和 USB 里 AOA 通道的对应关系一致（命令/视频/音频/TTS/VR/触摸），消息体不变：同样的 `HU_PROTOCOL_VERSION`、`VIDEO_ENCODER_INIT` 等，按键仍是通道 6（9340）消息 0x00068008。
- 第三方项目 CarPToCarL 也印证：Android 版 CarLife+ 引擎在本机暴露 127.0.0.1:7240 等端口，连接类型 `CONNECT_TYPE=5` 表示 Wi‑Fi；车机侧要发 FEATURE_CONFIG_RESPONSE（CONNECT_TYPE=5）、视频编码参数（宽、高、帧率）。
- 手机需自行在 CarLife+ 里点“无线连接”，工具不能代替。蓝牙不是视频传输通道（Apollo 里蓝牙只用于 HFP 电话和配对）。

## 3. NBT 车机网络条件（来自上游 NBT-AAD 源码）
- 车机有 Wi‑Fi 热点：CarPlay 热点接口 `uap0`（172.16.224.x），或 Wi‑Fi Direct 组 `wfd0`（172.16.222.x）。SSID/密码在 `iap2.cfg` / `iap2_Wifi.cfg`、`WFDPass.conf`。
- 上游无线 Android Auto 就是让手机连这个热点，再由车机监听 TCP 5277。
- 所以拓扑应该是：**手机连车机热点 → 手机广播 UDP 7999（目标 172.16.224.255 或 255.255.255.255）→ 车机连手机 TCP**。如果手机的 CarLife+ 只会走“手机开热点、车机当客户端”（百度客服写的做法），则需要车机 Wi‑Fi 能做 station 模式，这点 NBT 未必支持，要上车只读确认。
- 取得热点密码需要读车机文件（只读）；让手机自动连热点要靠用户手动在手机上输入一次密码（Wi‑Fi Direct 的 WFDPass 已有先例）。

## 4. 实现方案（离线可做的部分）
1. 新增 `carlife_qnx_wifi_capture.c`（或给现有 capture 加 `--wifi` 模式）：
   - 通过 dlopen 取 libsocket 的 `socket/bind/recvfrom/connect/recv/send/close`（与 ctl 小程序同一做法）。
   - 绑 UDP 7999，等一个包，记手机 IP（可限定在 uap0/wfd0 的 /24 内，防止外来包）。
   - 依次连 7240/8240/9240/9241/9242/9340；握手（版本、认证、特性、视频参数）逻辑复用现有 `handle_cmd`，只是帧头来源由 AOA 信封变成各 socket 自己的流。
   - 视频 socket 的 12 字节头 + H.264 体直接进现有 `capture_video`/环形缓冲，后面的 mdev/Screen 路径完全不变。
   - 按键写 9340。
2. 监督脚本 `nbt-aad` 增加“无线”分支：热点里出现手机（或收到 7999 广播）就开会话；有线/无线二选一，互斥。
3. 菜单：沿用“CarLife”入口，无线是自动检测，不增加菜单项（或在 Settings 页加一项，后续再看）。
4. 主机测试：假手机（Python）在 loopback 上起 6 个 TCP 服务 + 发 UDP 广播，回放已录握手/视频，验证握手、视频帧、按键。

## 5. 必须上车只读确认的事（需你授权，车停稳）
- `ifconfig` / `ls /dev/*wl*`：有没有 uap0、wfd0，是否支持 station。
- 车机热点现在是否常开（无 CarPlay 授权的车可能没有，要用 Wi‑Fi Direct）。
- 手机 CarLife+ 在车机热点网络里点“无线连接”后，是否真的在 7999 发广播（可用车机 `nc`/抓包不一定有，改用我们的监听程序记录来源）。
- 车机防火墙/路由是否放行这些端口。

## 6. 风险
- 手机 CarLife+ 版本不同，端口/广播行为可能变化；Android 版是否真的发 7999 广播没有源码证据（Apollo 车机端的 Wi‑Fi 分支写的是 iOS），上述 CarPToCarL 说明引擎本地有同样端口，但广播是否一致待验证。
- 帧率/分辨率仍由手机引擎决定（与有线相同的 16:9 限制）。
- 无线时和 CarPlay/Android Auto 争用车机 Wi‑Fi 热点，需遵守“不抢占已有投屏会话”的原则。

来源：Apollo CarLife-Android-Vehicle 源码（本地克隆）、上游 NBT-AAD 源码（wireless_setup / tcp_server）、GitHub CarPToCarL（jjuugk）、百度智能云问答 cloud.baidu.com/ask/1781。
