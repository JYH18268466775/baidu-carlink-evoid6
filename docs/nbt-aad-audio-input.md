# NBT-AAD 音频与 iDrive 输入梳理

依据交接包里的 `nbt-aad` v1.9.36-combo 本机二进制、符号清单及对应版本的 `main.cpp`。下列地址是本机 ARM ELF 的虚拟地址。CarLife 列的消息来自旧版车机端实现，三星手机对这些消息的实际行为仍待联调。

## 音频

| NBT-AAD 模式 | 车机路径 | 对 CarLink 的意义 |
| --- | --- | --- |
| `bt` | 默认模式。手机走 A2DP，`BtAudio` 用 `audioconn` 请求蓝牙音源。 | 如果手机的 CarLife 会话本身选择蓝牙音频，可验证这条路径。 |
| `browser` | 接收媒体、导航和系统音，重采样和混音，经原厂 `libasound` 写 PCM。由 `audioconn` 申请、释放 Browser 音源。 | USB PCM 播放的首选参考。可从单路媒体开始，把采样率、声道数、位深作为输入，路由和播放分别确认。 |
| `a2i` | 直接调用 `liba2i_client.so`。二进制含 `/input_stereo_AirPlayMain`、`/input_stereo_AirPlayAlt`、`/input_stereo_4` 和 A2I 的 Open、Start、Out_Write、Stop、Close。 | 另一路候选。原厂 CarPlay 也涉及 AirPlay 端点，使用前需要确认资源所有权和退出后的恢复。 |

`readBrowserConfig`（`0x116b04`）的反汇编显示 Browser 的缺省连接 ID 是 `0xB8`，请求采样率缺省是 48 kHz。二进制列出了 `snd_pcm_open`、`snd_pcm_plugin_params`、`prepare`、`write`、`flush`、`close`，以及 `/opt/mm/lib/libasound/libasound.so`。这些是本机版本的静态配置。实际能否打开设备、授予路由及出声，需要分别确认。 [NBT-AAD 音频模式](../references/upstream/milestone-7/src/main.cpp#L1726) · [本机二进制静态清单](../references/nbt-aad/nbt-aad%281%29.static.txt)

| CarLife 通道 | 初始化 | 数据 |
| --- | --- | --- |
| 3，媒体 | `0x00030001` | `0x00030006`，手机到车机 |
| 4，导航语音 | `0x00040001` | `0x00040003`，手机到车机 |
| 5，语音交互 | `0x00050002` | `0x00050003`，手机到车机。车机麦克风上行是 `0x00058001`。 |

播放端应以每路 INIT 报告的采样率、声道数和位深配置 PCM。首版可先做媒体 INIT/DATA/STOP，以及断线时释放 PCM 和音源。导航混音、压低媒体音量和麦克风上行可分开接入。AA 的音频通道号和确认机制属于 AA 会话层。 [CarLife 车机端消息定义](https://github.com/674809/carlife/blob/master/CarLife-Vehicle-Lib/LibSource/include/CTranRecvPackageProcess.h#L170)

### 电脑上提取媒体 PCM

`src/carlife_media_pcm.cjs` 可检查明文媒体通道抓包，按 INIT 解析格式，并把 DATA 原样输出。以下命令在 Windows CMD 中执行。输入如果已分离成 CarLife 媒体通道字节流，去掉 `--aoa`。

```cmd
node src\carlife_media_pcm.cjs --aoa < usb-capture.bin > media.pcm
node tools\tests\media-pcm.cjs
```

格式和字节数打印在终端。输出是无文件头的 PCM，暂停和恢复记录可供播放器使用。提取工具按收到的数据顺序拼接，遇到格式切换会报错。本机测试使用合成音频，现有首帧采集脚本只保存 H.264，真实媒体抓包和 Browser 播放仍待后续联调。

## iDrive 输入

本地适配器 `src/carlife_idrive_adapter.cjs` 接收已归一化事件 `{type, ms, ...}`，输出独立 outer/inner 包。`focus` 的 `active` 必须来自未来的真实前台所有权判断；初始不转发。`key` 使用 `key` 和布尔 `down`，过滤重复按下，BACK 在 800 ms 内释放才输出短按，长按不发给手机（本地退出动作尚未实现）。`rotate` 接收 -32..32 的 `delta`，防止异常计数生成大量报文；`disconnect` 清空状态并关闭转发。`cancel:true` 取消按键。运行 `node tools/tests/idrive-adapter.cjs` 可复核。此模块不是 MOST 监听器，也没有阻止原厂 iDrive 响应；不能直接声称物理旋钮已可控制 CarLife。现有参考仅有 main.cpp 的回调及初始化，尚缺配套 CcsInput/MOST 实现，下一步需补全接口后接入独立候选会话，USB 控制写入必须与握手写入串行化。

车端链路是 ZBE 控制器 → `MOSTSubSystem.DSYSMOSTServer` → `most::Client` → `CcsInput` → 旋转增量或按键事件。NBT-AAD 通过监听得到一份事件，iDrive 仍会收到原事件。本机 `CcsInput::onController`（`0x15d6fc`）反汇编确认会检查 CCS FBlock `0xE5` 和状态操作 `0x0C`。静态默认实例为 `0x03` 和 `0x01`，可配置。旋钮计数分支计算有符号差值，首个样本建立基线。 [NBT-AAD 输入注册](../references/upstream/milestone-7/src/main.cpp#L1764) · [本机二进制静态清单](../references/nbt-aad/nbt-aad%281%29.static.txt)

| iDrive 动作 | CarLife 通道 6 的候选映射 |
| --- | --- |
| 旋钮按下 | `OK 0x14` |
| BACK 短按 | `BACK 0x0E` |
| 旋钮上、下、左、右推 | `MOVE_UP 0x17`、`MOVE_DOWN 0x18`、`MOVE_LEFT 0x15`、`MOVE_RIGHT 0x16` |
| 旋转 | 保留车端有符号增量，按刻度转成 `SELECTOR_NEXT 0x06` 或 `SELECTOR_PREVIOUS 0x07`，方向和手机实际行为待验证。 |

旧版 CarLife 硬键消息 `0x00068008` 的数据体只有 `keycode`，候选实现可将一次按键动作编码为一次消息。AA 的相对旋转消息和按下、释放事件需要在边界处适配。第一轮可只转发旋转、按下和 BACK 短按。 [CarLife 键码表](https://github.com/674809/carlife/blob/master/CarLife-Android-Vehicle/src/com/baidu/carlifevehicle/CommonParams.java#L221)

`src/carlife_idrive_keys.cjs` 已提供离线编码。它接收已经解析好的单次按键动作或带符号的旋转刻度，输出 CarLife 通道 6。每条消息包含独立的 AOA 外层头和 CarLife 内层消息，USB 接入时应分别写入。以下命令在 Windows CMD 生成供检查的字节流，并运行回归测试：

```cmd
node src\carlife_idrive_keys.cjs OK BACK +2 -1 > idrive-keys.bin
node tools\tests\idrive-keys.cjs
```

当前离线约定是正向增量映射 NEXT、负向映射 PREVIOUS。是否符合实际旋钮方向，以及手机是否处理这两个键码，需要实测。 [CarLife 硬键封包](https://github.com/674809/carlife/blob/master/CarLife-Vehicle-Lib/LibSource/core/CTranRecvPackageProcess.cpp#L3054) · [CarLife AOA 外层封包](https://github.com/674809/carlife/blob/master/CarLife-Android-Vehicle/src/com/baidu/carlifevehicle/connect/AOAConnectManager.java#L471)

输入转发应以投屏前台所有权为条件。NBT-AAD 在倒车、泊车、驾驶模式提示或用户退出时让 iDrive 处理输入。其 HMI 插件还负责维持投屏所需的 iDrive 状态，避免底层界面同时响应。音量状态来自 AudioMaster `0x20 / 0x0C08`，可用于显示。触屏另走 HMI 的 UDP 30567 通道，可单独联调。 [投屏前台条件](../references/upstream/milestone-7/src/main.cpp#L2127) · [上游 iDrive 焦点修复](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto/pull/127)
