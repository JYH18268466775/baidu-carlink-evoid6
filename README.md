# CarLink：宝马 NBT EVO（ID6，QNX）上跑百度 CarLife

在 NBT EVO 车机上从 iDrive 菜单启动 CarLife 投屏：画面、车机喇叭出声、iDrive 旋钮控制，支持 **USB 有线** 和 **Wi‑Fi 无线**（三星车联 / CarLife+）。

> 私有仓库。内含原厂车机二进制、上游第三方库（见各自 NOTICE / 许可）、实车日志和录音，仅供个人研究自用，勿公开。

## 当前状态（2026-11-13）

| 功能 | 状态 |
|---|---|
| 有线 USB 投屏（AOA） | 实车可用，读线程 + 自动恢复，11 分钟无断（暂停开发） |
| 无线投屏（手机连车机热点 uap0） | 实车可用：画面、声音 0 断流、旋钮、MENU 快速隐藏，按键延迟 60–130 ms |
| 车机喇叭音频 | 可用（44.1 kHz 立体声，600 ms 抖动缓冲） |
| iDrive 旋钮 → CarLife | 可用（MOST/CCS 监听 → 通道 6 硬键） |
| iDrive 菜单入口 / 前台独占 | 可用（上游 NbtAad 插件 + 自己的监督脚本 `/opt/aa/nbt-aad`） |
| 无线时手机仍能上网 | **未解决**：正在研究让车机连手机热点，见 `docs/wireless-phone-hotspot.md` |
| 倒车/泊车让出屏幕、触控 | 未做 |

## 目录

| 路径 | 内容 |
|---|---|
| `build/live-input-candidate/` | **当前主线**：源码、脚本、构建、测试都以这里为准 |
| `build/live-input-candidate/src/` | 车机端 C 程序：`carlife_qnx_usb_capture.c`（接收/解码/音频/无线）、`most_watch`（旋钮）、`tcp_probe`、`sniff`、`bssrole`、`cp_purge` 等 |
| `build/live-input-candidate/carlife-menu/` | iDrive 菜单包：监督脚本 `package/nbt-aad`、上传脚本、离线测试 |
| `build/live-input-candidate/idrive-hold/` | 前台独占插件安装/卸载脚本（上游 NbtAad，PolyForm Noncommercial） |
| `build/live-input-candidate/tools/` | 构建（`build-linux.sh` / `build-qnx-probes.ps1`）、启动脚本、`check-build.cjs`、`tests/` |
| `build/live-input-candidate/build/` | 已编译的 ARM ELF（上车用） |
| `build/live-30s-candidate/` 等 | 历史已验证基线，回退用 |
| `car-session.ps1`、`car-*.cmd` | 电脑上一键上车步骤（双击运行，日志写入 `carlink-logs\`） |
| `carlink-logs/` | 实车日志；`fetched/` 是从车机取回做逆向的原厂二进制 |
| `car-captures/` | 录到的音频、车机 libasound |
| `src/`、`tools/`、`bin/` | 早期 Codex 阶段的源码与工具（`src/` 不是实车基线） |
| `references/`、`samples/` | 只读夹具：QNX 库、上游 NBT-AAD 源码、CarLife 抓包 |
| `docs/` | 文档：`project/`（Claude 项目文档）、`history/`（早期交接记录）、`wireless-phone-hotspot.md`（最新进展） |

## 构建

Linux（与 Windows MATLAB LLVM 输出逐字节一致）：

```sh
cd build/live-input-candidate
sudo apt install clang-15 lld-15
bash tools/build-linux.sh
```

Windows：

```powershell
cd build\live-input-candidate
powershell -ExecutionPolicy Bypass -File tools\build-qnx-probes.ps1 -Toolchain <LLVM15 bin 目录>
node tools\check-build.cjs
```

目标参数：`--target=armv7-none-eabi -mfpu=neon -mfloat-abi=softfp`，libc 用仅链接用的桩库，运行时 socket 函数通过 dlopen 取。

## 离线测试

在仓库根目录（Linux/WSL，需要 gcc、clang、node、python3）：

```sh
bash build/live-input-candidate/carlife-menu/tests/carlife-menu-test.sh   # 监督脚本 162 项（含无线）
bash tools/tests/wifi-capture.sh                                          # 模拟手机的无线会话 16 项
bash tools/tests/run-input-offline.sh                                     # 全部离线检查
```

## 上车（电脑上，项目根目录）

SSH：`root@127.0.0.1 -p 6969`（经车机调试转发），密码只由本人输入。

1. `car-1-upload.cmd`：上传 7 个文件到车机。
2. `car-8-wifi-on.cmd`：打开无线 + 声音 + 旋钮 + 日志开关；`car-9-wifi-off.cmd` 关闭。
3. 手机连车机热点和蓝牙 → 三星车联点"开始连接" → iDrive 菜单启动 CarLife。
4. `car-3-collect.cmd`：收日志。其余步骤（wifi-check / scan / sniff / dhcp-* / wifi-hw / fetch / bss-get）见 `car-session.ps1` 开头。

## 安全约定

- 每次实车测试前必须停车并由本人确认。
- 不贴出车机热点密码（`/var/opt/conn/carplay/iap2*.cfg`、hostapd.conf）；`fetch` 步骤会拒绝这些路径。
- 不改车机防火墙和原厂配置，除非本人明确同意；原厂 CarPlay/AA 始终可恢复（重启车机即还原）。
- `references/`、`samples/` 只读。
