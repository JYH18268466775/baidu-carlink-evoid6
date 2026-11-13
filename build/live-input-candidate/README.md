# live-input-candidate（2026-11-11，离线已做完，未构建 ELF，未上车）

基于已实车通过的 `build/live-30s-candidate/`，只加输入相关改动。**2026-11-12 起已合并宽屏显示**：视频请求 1280×480@30（`video_init` 与日志行），`src/carlife_qnx_mmr_connect.c` 把手机实际返回的宽高传给 `tools/carlife_visible_first_frame.sh`，脚本等比例居中（1280×480 铺满；仍返回 720p 时 853×480 居中留黑边）。这些改动来自 `live-wide-candidate/`，尚未实车验证；手机拒绝或不回复 480 高度时会超时失败，没有自动退回 720p 的重试。因此 replay ELF 的参照改为 `live-wide-candidate/build`，`tools/check-build.cjs` 已按此调整。本目录与 30 秒候选一样自包含（源码、脚本、固定夹具、上传脚本），除下列文件外与它逐字节相同：`src/carlife_qnx_usb_capture.c`、`tools/carlife_usb_visible.sh`、`upload-input-test.cmd`，以及新增的 `tools/check-build.cjs`、`BENCH-CHECKLIST.md`、本文件。

**下一步只剩需要你本人做的事**：在 Windows 的 LLVM 15 工具链上构建并自检，然后在你授权后按 `BENCH-CHECKLIST.md` 上车。

## 改动

1. **写入串行化**（`src/carlife_qnx_usb_capture.c`）：`send_record(fd, channel, type, body, size)` 把外层头和内层体两次 `write` 放在原子自旋锁里。`send_cmd` 是它的通道 1 版本。锁用编译器原子指令，ARM 目标码内联为 `ldrex`。
2. **硬键发送** `send_hardkey`：通道 6、类型 `0x00068008`、正文 `08 <code>`，与 `src/carlife_idrive_keys.cjs` 一致。
3. **台架输入线程**（可选）：`--stream-mdev|--stream-visible /dev/oapN --input-file /dev/shmem/NAME`。线程轮询这个普通文本文件，每行一个动作，用 `echo OK >> /dev/shmem/NAME` 追加。动作：`OK BACK LEFT RIGHT UP DOWN`，以及 `+N` / `-N`（1–32 步，正为 `0x06`，负为 `0x07`，方向映射仍是候选）。
   - 不带 `--input-file` 时行为与基线相同，不创建线程。
   - 用普通文件轮询而不是 FIFO：线程永不阻塞，只用已在用的 `open`（`O_RDONLY`=0）、`read`、`usleep`，不依赖 `O_NONBLOCK` 或 FIFO 语义，收尾时可以直接 `pthread_join`。
   - 名称只允许 `/dev/shmem/` 下 48 个字符以内的 `[A-Za-z0-9._-]`；文件必须已存在（启动脚本会创建并清空）。
   - **门控**：第一帧视频入队（`capture->frames` 变为 1）之前读到的行一律丢弃，不会在之后补发。
   - **丢弃**：未知词、超出 32 步、空行、超过 32 字符的行、没有换行结束的最后一行（它会和之后追加的内容拼在一起，`echo` 总带换行所以实际不受影响）。
   - **限速**：每 20 ms 最多读 64 字节，每个键发送后等 5 ms。
   - **收尾**：接收结束后先在锁内置 `usb_closed` 再 `close(fd)`，之后任何写入直接返回失败。写失败时输入线程自行退出。输入文件打不开返回 14（此时尚未打开 USB）；输入线程在**第一帧视频入队时**才创建（握手阶段不存在这个线程，因为带线程的版本在车机上连续 3 次卡在握手、不带的 1 次通过）；线程创建失败只记日志，显示继续，没有按键。
   - **日志**：门控打开时 `Key input enabled: first video frame queued`，每个发出的动作 `Input sent: ...`，被拒绝的行 `Ignored unrecognized input line`。
4. **启动脚本**（`tools/carlife_usb_visible.sh`）：设置 `CARLINK_INPUT_FILE=/dev/shmem/NAME` 才启用；插手机前就校验名称（不合法返回 2，什么都不启动）；实时显示分支启动前清空该文件并传 `--input-file`；`--diagnose-usb` 忽略它并提示。不设置时脚本行为与 30 秒候选相同。
5. **构建自检**（`tools/check-build.cjs`，`upload-input-test.cmd` 第一步自动运行）：replay、status 两个源码未变的 ELF 必须与已验证文件逐字节一致（证明工具链可信）；capture ELF 必须不同于已验证版本；三个文件必须是完整的 ARM ELF。
6. 没有新增 libc 导入：基线与本版各 17 个未定义符号，集合相同，`libc-link-only.S` 无需修改。

## 离线验证（`bash tools/tests/run-input-offline.sh`，末行 `ALL OFFLINE CHECKS PASSED`）

需要 gcc、clang、node、sh 和 bash，在 Linux/WSL 下运行；现有 Windows 测试用 MSVC 替身，二者互不替代。接收程序的测试连续运行 8 次全部稳定；加日志后重新跑过一次。

- 真实接收源码经 `-D` 改名接入模拟 I/O：4 个线程并发 1200 条记录始终是相邻的“外层头+内层体”；头/体写失败、短写、超长正文都释放锁。
- 输入线程：门控、按序输出、拒绝行、64 字节读取上限、相邻两键间隔 ≥ 4.5 ms、关闭后不写、写失败即停、路径规则、入口参数与提前失败时描述符都被关闭、各类日志内容。
- 变异检查：分别去掉锁、门控、关闭保护、按键间隔，对应测试都会失败。启动脚本测试同样做过三个变异（不传参数、不清空文件、不校验名称），都被抓到。
- 门控点：用合成的 SPS/PPS/IDR 驱动真实的 `capture_video`，只有参数集时 `input_ready` 为 0，第一帧入队后为 1。
- 启动脚本：把所有 `/dev/` 路径重写到临时目录，用假接收/回放程序记录命令行，分别在 `sh`（dash）和 `bash` 下通过：不设变量时不传 `--input-file`、合法名称传入并清空旧内容、八种非法名称返回 2 且什么都没启动、`--diagnose-usb` 忽略输入。
- 构建自检脚本：用真实已验证 ELF 作夹具，接受正常重建，拒绝工具链不一致、未重新编译、被截断、文件缺失。
- C 输出与 `carlife_idrive_keys.cjs` 逐字节一致；握手 `send_cmd` 对 0–56 字节 57 种正文与 `live-30s-candidate` 源码逐字节一致。
- 按构建脚本的 ARM32 选项加 `-Werror` 编译通过（仅编译）。

## 没有验证、也没有做的

- **没有 ELF，且本机无法产出可上车的 ELF**：本机只有 clang 18，它会把 `= {0}` 编译成 `__aeabi_memclr4/8` 调用，连已实车通过的基线源码也链接不过（LLVM 15 不会）。所以本机的 ARM 编译只当语法与警告检查。请用你的 LLVM 15 运行本目录 `tools/build-qnx-probes.ps1`（输出在本目录 `build/`），再运行 `node tools\check-build.cjs`。该脚本的 PowerShell 部分没有改动。
- **QNX 行为未验证**：`usleep`、`/dev/shmem` 文本文件被追加后 `read` 能否读到新数据，都没有在车机上确认。
- **“第一帧入队”只是最低门槛**：它不证明手机界面已可操作；README 里记录过首帧可能是黑的。手机是否响应通道 6 的键码、旋转方向是否正确，全部未知。
- **没有前台所有权**：只要文件里有行、且第一帧已入队，键就会发给手机，即使显示窗口已结束、iDrive 已恢复。台架测试时画面恢复后不要再写入。
- MOST/CcsInput 监听、倒车/泊车时让 iDrive 自己处理输入，仍是空白，也无法离线完成（没有配套源码，需要车上抓取）。

## 备注：`src/` 不是实车基线

`src/carlife_qnx_usb_capture.c`（14:31）含版本补发、`BAD_VIDEO_HEADER` 恢复、完整媒体记录跳过等后来回滚的实验改动，与实车通过的 `live-30s-candidate/src/` 不同。继续开发请以候选目录里的源码为准。
