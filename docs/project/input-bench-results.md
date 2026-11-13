# 输入候选台架结果（更新 2026-11-12 晚）

## 已确认（用户实车目测）
- 手机响应通道 6 硬键：旋转 `+1`（SELECTOR_NEXT 0x06）= 向右，`-1`（SELECTOR_PREVIOUS 0x07）= 向左；RIGHT/LEFT 与 +1/-1 效果一致；OK 选中；BACK 返回。方向映射不需要对调。
- 真实 iDrive 旋钮 → CarLife 已打通（MOST 监听 `--emit` → 输入文件 → 通道 6）：旋转、按下 OK、BACK 在投屏画面上都有对应动作。
- **前台独占已验证**：装上上游 iDrive 应用并用替身守护进程后，投屏期间车机原生菜单不再跟着旋钮动，CarLife 画面按键正常。
- 手机屏幕本身不显示界面是正常的；看车机屏幕上的 CarLife 画面。

## 显示尺寸（2026-11-12 结论：保持 853×480 居中黑边）
- 请求 1280×480，手机仍回 `Phone encoder: 1280x720 @ 30 fps`（它只给 16:9）。显示脚本按实际尺寸等比例居中：`Display fit: phone 1280x720, destination 213,0 853x480`。
- 协议查证（Apache-2.0 的 Apollo CarLife-Vehicle-Lib）：`CarlifeVideoEncoderInfo` 只有 width/height/frameRate，`HU_INFO` 是 Android Build 字符串，没有边距/insets/可视区域；官方车机把自己 SurfaceView 的宽高原样发给手机，由手机决定编码尺寸。上游 NBT-AAD 的 crop 模式（margin 1280×480 + insets）是 Android Auto 协议功能，CarLife 没有对应物。
- 已实现并离线测试、可选开关（环境变量 `CARLINK_DISPLAY_FIT`）：`fit`（默认）、`stretch`、`crop`（1:1，上下各切 120 行）、`crop:N`。用户实车试过拉伸/裁切，均觉得不好看，**默认保持 fit**。
- 未做：探测手机是否接受其他更宽的尺寸（1920×720 等），需给 capture 加 `--video-size`，每个尺寸要插拔一次，预期收益低。
- 中断投屏（Ctrl+C）会留下孤立的 surface 1510，之后启动会被拒绝；清理：`LayerManagerControl get surface 1510` 查 `created by pid`，再 `sh carlife_visible_first_frame.sh --cleanup <pid>`。投屏让它自己跑完或拔手机结束。

## MOST 监听（只读，实车）
- 本车使用旧版 CCS 协议：FBlock E5，inst 0x01，旋转 fkt C10，按下/倾斜/硬键 fkt C20。解码与上游 CcsDecoder 在 20/51/14 个夹具事件上完全一致。
- 注册：DSI 请求 0xF 得 clientId（每次递增）、deviceId 0x205A/0x205B；通信状态 1。
- 映射：ROTARY ±N → +N/-N；PUSH down → OK；BACK down → BACK；倾斜 → UP/DOWN/LEFT/RIGHT；MENU 等不映射。

## 前台独占（已装，实车有效）
- 做法：借用上游 NBT-AAD 的 iDrive 插件 `libNbtAad.so`（rhmiapp-43-combo，个人自用，PolyForm Noncommercial；`build/live-input-candidate/idrive-hold/NOTICE.txt`）。它把 iDrive 在投屏期间停在自己的空白状态（hmiState 13）。
- 我们不用上游守护进程，只用一个 sh 替身 `/opt/aa/nbt-aad`：写 `/dev/shmem/nbt-aad-state` = `connected <pid> 1`，应用即认为投屏中。投屏由我们自己的程序负责（后来替身已发展为完整监督脚本 carlife-menu/package/nbt-aad）。
- 安装位置：`/opt/hmi/ID5/data/ro/Plugins/NbtAad`（5 个文件）和 `/opt/aa/nbt-aad`；安装需把 sda0 临时 remount rw，并 `OnOffDSICommander appreset`。回滚：`idhold_uninstall.sh --yes` + appreset。脚本在 idrive-hold/tools，离线测试 idrive-hold/tests/idhold-test.sh。
- 注意：`/dev/shmem` 不支持子目录，上传文件用平铺名 `idhold-<名>`；每次 appreset 后原厂 CarPlay 会重新创建 `cpvideoctx`，需再跑 purge。
- 已知局限：替身不监听 UDP 52790，应用发来的 hide/show/gear/pdc 都没有效果；因此倒车/泊车让出屏幕等还没做。

## 握手卡死的原因（取证已确认）
- 现象：RX #2 缺 10 字节，之后只收到 0x00010026，60 秒看门狗到期；有时是视频流中途 `Invalid AOA header ... status 12` 退出（exit 65）。
- 取证：NBTMediaMainApp 线程 `AndroidDevCtrl_Port0` 与接收程序同时读 `devc-serusb-aoap` 服务的同一个 `/dev/oap*`。
- 上游做法：①专用读线程只做 read+入队；②根治 NBT_USB_DIRECT：经 libusbdi/io-usb 直读。
- 本项目后来已做读线程（见 offline-work-status.md）。

## 清除原厂 cpvideoctx
- `carlife_qnx_cp_purge.elf` + `carlife_cp_purge.sh`（只接受恰好一个 `cpvideoctx<数字>`、手机未插、必须 `--yes`），实车成功。

## 待做
- 替身增强：监听 UDP 52790（hide/show/gear/pdc），倒车/泊车让出屏幕和输入（开车前必须做）。
- 倾斜四方向实车验证；MENU/OPTIONS 等硬键映射。
- 车机重启后复核原厂 CarPlay 恢复。

（台架清单与输入候选交接说明见 `build/live-input-candidate/BENCH-CHECKLIST.md` 和 `build/live-input-candidate/README.md`。）
