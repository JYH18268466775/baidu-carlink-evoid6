# 台架测试清单：手机是否响应 CarLife 硬键（需要你明确授权后才执行）

目的只有一个：在停车台架上确认“接收程序发出的通道 6 硬键，手机界面会不会响应，旋转方向是否符合预期”。它不验证 MOST/旋钮接线、前台所有权、音频或长时稳定性。AGENTS.md 的规则全部适用：保持 AA 和原厂 CarPlay 可恢复，发送前确认目标和清理路径。

## A. 上车前（全部在电脑上，已可离线完成）

1. 在 Linux/WSL 运行 `bash tools/tests/run-input-offline.sh`，末行必须是 `ALL OFFLINE CHECKS PASSED`。
2. 用与已验证候选相同的 LLVM 15 工具链运行本目录 `tools/build-qnx-probes.ps1`（构建输出在本目录 `build/`）。本机没有 LLVM 15，所以这一步无法替你完成。
3. 运行 `node tools\check-build.cjs`。它要求 replay、status 两个 ELF 与 `live-30s-candidate` 的已验证文件逐字节一致（证明工具链可信），capture ELF 不同于已验证版本，且三个文件都是完整的 ARM ELF。任何一项 FAIL 都不要上传。
4. 记下 check 打印的 capture ELF SHA-256，写进 README。

## B. 上传与确认（沿用 30 秒候选的做法）

1. 运行 `upload-input-test.cmd`（它先自检，不通过就不上传）。会覆盖车机 `/dev/shmem/` 里同名的两个 ELF 和三个脚本；之前通过的 30 秒组合仍可用 `build\live-30s-candidate\upload-30s-test.cmd` 恢复。
2. 用 `ls -l /dev/shmem/*.elf` 核对两个 ELF 的大小与电脑上一致（以前出现过被截成 0 字节的情况）。
3. 查看 `df`/剩余内存（上次检查 FreeMem 276 MB / 2048 MB）。

## C. 开跑前的状态检查

- 已停车，AA、原厂 CarPlay 已退出，手机未插。
- `ls /pps/services/multimedia/renderer/context` 为空；没有上一轮残留的 surface 1510（启动脚本内置的检查会拦截）。
- 准备第二个 SSH 窗口用于写入按键文件。
- 想好中止方式：直接拔手机线。已实测检测到 USB 读失败后约 1 秒内恢复 iDrive。

## D. 执行

窗口 1：

```sh
cd /dev/shmem
chmod +x carlife_qnx_usb_capture.elf carlife_qnx_mmr_replay.elf
CARLINK_INPUT_FILE=/dev/shmem/carlife-keys sh carlife_usb_visible.sh ./carlife_qnx_usb_capture.elf ./carlife_qnx_mmr_replay.elf
echo "live exit=$?"
ls /pps/services/multimedia/renderer/context
```

按提示插入已解锁的测试手机。日志出现 `Key input enabled: first video frame queued` 之后，才会处理按键；这是 30 秒显示窗口内的事，窗口结束、iDrive 恢复后就不要再写入。

窗口 2（画面出现后，每条间隔几秒，一次一条）：

```sh
echo RIGHT >> /dev/shmem/carlife-keys
echo LEFT  >> /dev/shmem/carlife-keys
echo +1    >> /dev/shmem/carlife-keys
echo -1    >> /dev/shmem/carlife-keys
echo OK    >> /dev/shmem/carlife-keys
echo BACK  >> /dev/shmem/carlife-keys
```

先用方向键，最后才发 `OK` 和 `BACK`，因为它们会真正操作手机界面。

## E. 记录什么（把三件事分开写）

1. **程序发出了没有**：窗口 1 的 `Input sent: RIGHT` 之类日志。这只说明写入 USB 成功，不说明手机处理了。出现 `USB command write failed` 说明写失败。
2. **手机界面有没有变化**：由你在车机屏幕上目测；每个键单独记一条。
3. **旋转方向**：`+1` 和 `-1` 在界面上各自向哪个方向移动。当前映射是候选，如果反了，只需对调 `0x06`/`0x07`。

同时照旧记录：握手结果、缺块次数、退出码、renderer context 列表、surface 1510 是否自动清除。

## F. 出问题时

- 拔手机线（已验证的提前恢复路径）。
- 图层未恢复时：`/opt/sys/bin/LayerManagerControl set layer 0x80000000 render order 0x100` 和 `/opt/sys/bin/LayerManagerControl set surface 256 visibility 1`。
- 残留窗口：`sh carlife_visible_first_frame.sh --cleanup PID`（PID 为 replay 子进程）。
- 回到已验证基线：`build\live-30s-candidate\upload-30s-test.cmd`。

## 这次测试不能证明的

只要有任何一个键让手机界面发生了变化，才算“手机响应通道 6 硬键”这一点成立；没有变化也不能直接断定键码无效，可能是首帧门控、界面状态或键码映射的问题，需要按日志分开判断。
