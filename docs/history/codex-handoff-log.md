# CarLink · Codex 交接

## 最新实测：手机 USB 实时画面更新通过

用户要求暂缓缺包优化，优先输入控制，并选择宽屏优先协商 1280×480、仍返回 720p 时保留黑边。本地已加入规范化 iDrive 输入适配与测试（尚无 MOST 接线、前台所有权和实际 USB 控制发送）。另建 `build/live-wide-candidate/`，不覆盖已实车通过的 30 秒组合。宽屏候选修改视频请求为 1280×480@30，MMR 将实际协商宽高传给显示脚本，脚本等比例居中。720p 返回时显示 853×480（取整），左右留边；1280×480 则铺满。手机拒绝/不回复仍沿用超时失败退出，尚未实现自动重试 720p 请求，不能称为已验证支持宽屏。QNX 构建及几何测试通过，需实车确认手机返回尺寸和实际画面。只上传变化的三个文件用 `build/live-wide-candidate/upload-wide-test.cmd`，要求车机仍保留当前 30 秒 launcher 和 replay guard。

30 秒到时退出已实车通过：用户重启后报告成功，完整日志 `/dev/oap01`、`carlifebench1663093` 确认握手、首帧、显示阶段完成，打印 `30-second display completed; stopping this USB session.`，随后主动终止 capture，mdev 接收 164 帧，renderer POST=1，停止/解绑/销毁均成功，surface 1510 自动删除，`live exit=0` 且 renderer context 列表为空。`Terminated` 位于主动停止分支，未出现 Alarm clock。无时间戳，不能量化到时至终端退出延迟。大量 Missing AOA 和 IDR 恢复仍存在，不能把正常退出等同于流畅性或 USB 稳定性通过。用户怀疑多文件上传导致失败，现有证据不能确认因果；空间检查主要分区有余量，检查时 FreeMem 276 MB / 2048 MB。

最新握手失败的本地复现：`tools/tests/usb-315-boundary.cjs` 用合成输入重现 RX #1 仅版本响应外层头、RX #2 声明 8 字节控制记录、RX #3 实际 315 字节内层命令，得到同样的 `0a07416e 64726f69` 非法外层头、`inner pending cmd=8` 和退出 12。307 字节正文只保留日志已知前缀，其余为填充，0x10004 类型为假设，不是实车完整抓包。正确声明 315 字节的记录在 8 种 USB 切分位置以及分成两个外层记录时均通过并输出参考 IDR；缺版本响应不被误判成功。未修改运行 ELF。用户观察重启车机并重新上传后曾好转，两项操作未分开验证；空间不足、USB 会话状态和其他读取者均未据此确认。

2026-11-11 长测结束修正（本地通过，待车机验证）：13:12:49 开始显示、13:13:20 恢复 iDrive，USB 仍到 13:13:48 累计 546 帧后触发 60 秒 watchdog。可见窗口与 900 帧采集目标不同步。30 秒候选现仅在显示满时且 iDrive 恢复验证通过、未检测断线时创建会话完成标记。回放停止继续送帧，启动脚本监控标记并终止本次 capture 子进程，避免等待 900 帧或 60 秒。提前 EOF、无首帧、非法帧、mdev 错误和 capture 真实失败仍保留非零状态。备用恢复与表面归属验证保持原规则。USB ELF SHA-256 仍为 `9E1AA3F27F27EE88B4175150F284CD07E802E83AE205A2597EECD3EC6AA0C400`；MMR ELF 更新为 `D08E88B0C82663837384C93DA87ABFA686511FAB88474F3BD2CB0CF05B9E09A7`。QNX 编译、实际 replay C 循环的 Windows EOF/错误测试、Bash 正常到时终止阻塞采集及失败保留、900 帧逐字节回归均通过。上传入口仍为 `build/live-30s-candidate/upload-30s-test.cmd`。QNX 下标记读取和 TERM 结束的实际延迟待测，不等同于已解决 USB 缺包或帧率问题。

561 帧长测日志补充：iDrive 在 USB 60 秒超时前已恢复，恢复后 RX #1072 又报视频缺块，随后从 IDR 恢复。已构建长测 capture 的低频进度日志（每 60 帧、每 512 次非空 USB 读取），可和显示/恢复时刻对照，区分 USB 仍有读取但未产生视频帧与读取停止。QNX 构建和 900 帧 Windows 模拟通过，视频数据逐字节一致。仅更新 30 秒候选的 capture ELF，显示和接收限时未改变。

30 秒候选复测保留完整输出到车机 `/dev/shmem/carlife-30s.log`。过滤日志确认 `Alarm clock`、capture 退出 142、mdev 接收 561 帧、最终 65，属于 60 秒 USB 看门狗到期前未达到 900 帧目标。过滤结果没有其他明确终止错误。需查看既有日志里的缺包、IDR 恢复和超时前最后的事件，判断是否在恢复中停住或只是帧产出不足。第一轮 606 帧的具体退出原因仍未知。

30 秒候选首次实车 `carlifebench3326068`，用户报告画面正常且结束后返回 iDrive。日志确认 mdev 接收 606 帧、停止/解绑/销毁成功、surface 自动删除，退出 65。记录出图与恢复通过、900 帧完整送达未通过。现有尾部日志从 mdev 计数开始，需查看其前面的 USB 停止、解析、缓冲或超时信息才能定位提前结束。无需重复已完成的现场操作。

30 秒停车测试候选 `build/live-30s-candidate/` 已构建。沿用通过实车的协议实现及拔线/清理脚本，候选将可见模式上限调为 900 帧、30 秒显示窗口、60 秒 USB 限时，append-only 缓冲上限增至 16 MiB，45 秒备用显示恢复。Windows 模拟通过 900 帧视频与时间戳逐字节比对，以及拔线检测、备用计时取消和清理；QNX 两个 ELF 已构建。手机实车的持续可见画面、缓冲占用和出图耗时待验证。使用 `upload-30s-test.cmd` 成套上传，短时基线仍可通过 `build/live-unplug-candidate/upload-unplug-test.cmd` 恢复。

新版备用计时取消脚本实车通过拔线退出：`carlifebench2130039` 在 `/dev/oap06` 建立 1280×720@30 会话，12:53:52 切换显示，12:53:54 USB read errno 3，12:53:55 触发提前恢复、确认 iDrive 图层恢复、显示脚本返回 0、surface 1510 自动删除，最终回放退出 65（拔线提前结束，mdev 13 帧）。按秒日志上从 USB 报错至恢复约 1 秒，恢复到终端退出同秒；此前清理阶段的 16 秒等待未复现。时间戳为终端收到日志的时间，未直接量化物理拔线到显示恢复。保留 `build/live-unplug-candidate/upload-unplug-test.cmd` 组合：基线 ELF + 已实测显示/清理脚本。USB 缺包仍可见，长时稳定性及音频/控制待验证。

拔线标时实测 `carlifebench10449014`：12:37:36 同一秒内 USB read/close 报错、检测到停止、图层恢复成功；mdev 共接收 32 帧。12:37:52 自动清理 1510，退出 65。16 秒位于已确认图层恢复之后，现有日志没有进一步标时以确认具体等待点。拔线候选的显示脚本现使用可被 TERM 中断的异步计时等待并关闭备用进程的输出描述符，图层恢复确认成功后退出时不重复恢复。Windows Bash 模拟保留实际 20 秒备用计时，在 8 秒内退出且只恢复一次；QNX 退出时间待复测，仅需更新显示脚本。

拔线候选实测 `carlifebench9470067` 在 `/dev/oap016` 出图后，USB read errno 3、close errno 9、capture 退出 11，mdev 接收 14 帧后提前结束。显示脚本明确打印 `USB disconnected or capture stopped; restoring iDrive now.`，随后确认图层恢复，renderer 退出 65，surface 1510 自动删除。实车确认提前恢复分支生效，但用户测得约 16 秒且原日志无时间戳，不能判定延迟在哪一段。下一次可沿用现有文件，仅在输出层给显示、USB 错误、恢复开始和完成、清理标时，并在组内打印真实退出码，避免额外上传或改动二进制。

拔线候选实测用户报告约 16 秒才恢复，尚未提供该轮日志。新增本地测试验证启动脚本把 OAP 路径和失败标记路径传到 replay 子进程、捕获失败写标记、结束后移除标记。QNX 实车上是否进入提前恢复分支仍待用 `USB disconnected or capture stopped; restoring iDrive now.` 与恢复/清理日志确认，避免将回到 iDrive 与终端退出时间混用。

拔线恢复候选 `build/live-unplug-candidate/` 从已实测的自动清理脚本派生，使用基线 ELF 原件。启动脚本传递本次 OAP 节点路径和捕获失败标记。显示脚本在切换前及显示期间每秒检查两者，节点消失或捕获失败后恢复 iDrive。正常显示仍限 10 秒，保留退出清理。模拟验证节点消失、捕获失败标记、切换前断线、正常等待、PID 与清理路径，车机实际恢复延迟待测。仅上传候选中的两份脚本。

继续使用车机现有基线 ELF 和自动清理脚本，用户确认本轮出图并返回 iDrive。此轮未提供计数、退出码或画面更新细节，沿用此前完整日志作为 300 帧和自动清理证据。本轮未复现握手卡住，未执行线程快照。

握手卡住时补抓只读线程状态：`pidin arg | grep -E '[N]BTMediaMainApp|[d]evc-serusb-aoap|[c]arlife_qnx_usb_capture'; pidin -p NBTMediaMainApp threads; pidin -p carlife_qnx_usb_capture.elf threads`。在 USB 接收端仍等待版本回复时从另一 SSH 窗口执行。根据 QNX 6.5 pidin 文档，`threads` 显示线程名、状态和 Blocked；`REPLY` / `SEND` 的 Blocked 是目标进程 PID。对照当轮 `devc-serusb-aoap -b ... -d ...`，可查两方是否同时等待同一个驱动。单次快照只代表采样时刻，即使匹配也不能证明哪个读取者拿到了缺失的数据。运行文件与脚本无需更新。

基线 USB 独立诊断 `/dev/oap013` 握手成功，300 帧、退出 0。视频通道报告 4 次缺块、3 次从 IDR 恢复。`devc-serusb-aoap` PID 8085619 参数 `-b 0 -d 13 -l 16384` 与该节点对应。`pidin -p NBTMediaMainApp fds` 仅打印 PID 221233 和进程名，未列出连接或文件描述符。这是正常握手轮的查询，无法确认或排除其他读取者，更不能代表握手失败时的状态。

自动清理脚本重连实测，两次均无需手工删除 1510。第一次 `/dev/oap08` 缺 10 字节控制包，未收到版本确认便 USB 读失败 errno 3，回放 65，查询无 surface。第二次 `/dev/oap08`、`carlifebench6983795` 握手成功，视频从 IDR 恢复，接收并送入 300 帧，恢复图层并销毁 context，自动删除 1510，独立查询无记录，最终退出 0。此轮记录完成日志，未附屏幕观感。正常与异常两条自动清理路径均已实测，仍需定位首次 USB 握手中断。

自动清理候选实车通过异常退出路径：`carlifebench4190323` 回放退出 65，renderer context 销毁成功，随后打印 `Removed detached CarLife surface 1510.`；独立查询返回 `No surface with ID 1510 found`，启动脚本保留了回放退出码 65。用户给出的尾部日志没有捕获侧错误或收帧计数，不能据此定位回放退出原因。下一轮直接重连可验证无需手动删除窗口。

已准备自动清理候选 `build/live-cleanup-candidate/`。仅替换两份脚本，使用实车基线的两个 ELF。启动脚本取得实际 replay 子进程 PID，等待退出后检查 surface 1510 的创建者、native surface 无效值、图层归属为空和 iDrive 基线，再删除并查验。`--cleanup PID` 可清除前一轮已知归属的同类残留。模拟验证管道输入与 PID、正常删除、无记录、归属不符、有效窗口、图层占用、删除未生效与退出码保留。待 QNX 实测自动删除以及下一轮直接重连。候选不包含 USB 握手或拔线延时修改。

同次车机开机下，手动清理前一轮残留 surface 1510 后重插复测 `/dev/oap04`、`carlifebench3182705`。版本握手通过，控制通道多次收到 `0x00010035` 22216 字节正文，实车确认基线长控制消息处理通过。中途缺包两次从 IDR 恢复，最终接收及送入 mdev 300 帧，图层恢复，context 销毁，退出 0。此轮日志证明完成回放与清理，未单独报告实物屏幕观感。重连仍依赖手动清除上一轮遗留 surface，后续优先验证私有 surface 自动清理。

回滚组合在当前车机复测恢复出图。首次运行被 `cpvideoctx16838` 占用检查拦截，后续运行时用户确认画面正常。此次未收到完整退出日志，收帧数、退出码与恢复耗时尚未记录。保留 `build/verified-live-baseline/` 作为实车基线。

恢复实测基线：在 `build/verified-live-baseline/` 重建 `carlifebench9244784` 当时的运行组合，两个 ELF 分别与历史 SHA-256 `DBD98F7323C12F06BDDC989B6BC3BF0B813FAE455A619E9EF26E0EDE9149CF83` 和 `FBCB43D991602E9270686EE4CF280308A3F0D1200BB4F9140EFD422B46BDB279` 一致，配套脚本由历史改动还原并通过语法检查。`upload-baseline.cmd` 可在 Windows CMD 一次上传两个 ELF 和三个脚本。此基线用固定 10 秒恢复图层，USB 仍可能缺包。最近 QNX 文件描述符查询未显示 `oap` 或 `serusb` 路径，无法确认是否共享读取。此组合尚未在当前重启后的车机复测。

版本补发实车 `/dev/oap05`：RX #2 再次缺 10 字节命令，收到 `0x00010004` 后打印一次补发，随后仅见 `0x00010026`，无版本确认，45 秒超时，capture 142 / replay 65。补发在该会话未恢复握手。下一步保持手机插着，查看 `ls /dev/oap*` 和 `pidin -p NBTMediaMainApp fds` 是否显示同一设备，作为读竞争排查证据；未输出设备路径也可能是 QNX 无法显示文件名，不能单凭这一点排除竞争。无需重新上传或重复回放。

车机重启后 `/dev/oap02` 实测在版本阶段超时，RX #2 遇到一个 10 字节缺失的控制包，之后收到了 `0x0001001b`、`0x00010018`、`0x00010004` 和 `0x00010026`，未见版本确认或视频初始化。45 秒 watchdog 结束捕获（142）。新增仅在等待版本确认、已检测到控制包缺失、随后收到 `0x00010004` 或 `0x00010026` 时补发一次版本请求，仍须收到有效确认才进入视频初始化。Windows 模拟覆盖同类读边界、一次补发和后续视频，原回归与 QNX 编译通过。只更新 USB capture ELF，手机是否在同一次连接回应补发仍待车机实测。

复测 `carlifebench11698289` 用户见黑屏且恢复有延迟。mdev 接收 2 帧，实际 RX #18 以完整 media data 头开始（3584 字节正文，3596 字节 USB 读取），前一外层音频头只声明 20 字节，原解析误把剩余 PCM 当作外层头并退出。日志确认恢复通知已触发，切换后两次 LayerManager 查询仍在通知检查之前。新版在实时视频模式识别并跳过这类精确匹配的音频读取，并移除切换后的两次查询。回归使用同样 20/3596 字节长度，验证视频输出逐字节一致；显示模拟确认切换后若已有失败标记则直接恢复。ARM 编译通过。仍需验证车机实际画面及正常出图后的拔线恢复时间，更新 capture ELF 和显示脚本即可。

拔线新版复测 `carlifebench10870897` 在显示前因 USB 视频坏包提前结束。mdev 仅接收 1 帧，接收端报 channel 2 外层长度 51206、无效内层视频头 `3f22fdfd / 80dbfe30`。失败标记触发 `USB capture stopped before display switch`，退出 65，context 清理为空。该次验证了切换前取消，尚未验证显示后的拔线恢复速度。接收端现对已经检测到视频缺包、正在等待 IDR 的会话按有效 AOA 外层长度丢弃含坏视频头的包，继续等待下一个关键帧。按实车尺寸及头字段构造分包，Windows 回归验证输出与缺包恢复参考逐字节一致，QNX 编译通过。只需更新 capture ELF。

拔线实测 `carlifebench10014832`：USB 读失败 errno 3，送帧止于 36 帧，显示脚本仍等固定 10 秒后恢复 iDrive，退出 65。新版 USB 启动脚本在接收程序失败退出时写入每次会话独立的失败标记，renderer 传递会话号，显示脚本在切换前和显示期间每秒检查，发现标记就恢复。正常显示仍限 10 秒，保留备用恢复，退出清理标记。已通过 ARM 构建和模拟拔线前/显示中恢复测试。车机实际恢复延迟待复测，包含接收程序退出与 LayerManager 执行耗时。只更新 replay ELF、USB 启动脚本和显示脚本。

10 秒实时可见测试 `carlifebench9244784` 通过。用户确认画面有变化，手机协商 1280×720@30，接收端和 mdev 均完成 300 帧，退出码 0，iDrive 图层恢复且 renderer context 列表为空。图层恢复发生在最后一批收帧之前，不能用 300 帧推算物理屏幕显示帧数。期间命令、视频和音频通道仍有 AOA 缺块，视频两次从 IDR 恢复。短时实时显示已验证，长时稳定性、主动拔线恢复、音频和 iDrive 控制待验证。

新版 USB 诊断实车在 `/dev/oap09` 收满 300 帧并退出 0。视频通道 RX #404、#405 报缺块，随后从 IDR 恢复。该次日志没有 `0x00010035`，所以长命令修复仍以本地复现和回归为证；300 帧 USB 路径在此会话通过。下一步复用已上传文件做 10 秒实时可见验证，观察画面变化与 iDrive 恢复。

USB 诊断实车在 189 帧后退出 12：channel 1 外层长度 26886，内层 `0x00010035` 正文 26878，首个读取长度 16384。命令缓冲原为 4096 字节，必然拒绝该尺寸。本地模拟按相同长度和分包复现旧版退出，再将命令缓冲调整为 8 字节头加 16 位长度字段的最大正文。新版在模拟中收完命令并接收后续 IDR，原 USB 回归和 QNX 构建通过。只需更新 capture ELF 并用原 `--diagnose-usb` 复测。实车另有一次 channel 3 缺块并恢复，早前的外层 AOA 错误根因仍待查。

USB 诊断版已构建。上游 NBT-AAD `usb_accessory.cpp` 记录过 `NBTMediaMainApp` 与接收程序同时读取 `/dev/oap*` 时丢失整次 bulk IN 传输。本项目现有接收缓冲区同为 64 KiB，超过上游记录的 16 KiB 驱动传输上限。实车这次的读取者尚未核实。新增 `sh carlife_usb_visible.sh ./carlife_qnx_usb_capture.elf --diagnose-usb`，复用 300 帧/45 秒接收路径，输出送 `/dev/null`，保留日志，无需 renderer 或空闲 mdev。遇到无效 AOA 头时记录同次读取开头的两个字段，判断该读取是否恰好匹配一条完整 CarLife 音频消息。Windows 模拟确认能区分全零输入和长度/类型匹配的合成音频消息，原回归通过，QNX ELF 编译通过。该版用于采证，尚未修改恢复策略。

USB 本地复现补充：已在 `tools/tests/usb-capture-host.cjs` 增加“channel 3 外层长度 20，下一次 read 返回 3596 个零字节”的合成用例，现有 host 探针准确在 +20 复现 Invalid AOA header；正确封装的 3596 字节 channel 3 数据则不会阻止后续视频 IDR。原有握手/失败/20 帧阻塞管道回归全通过。此结果仅复现日志形状，真实 3596 字节内容未采到，不能证明实际音频负载为零，也不能确定是漏读外层头、驱动分包还是当前解析策略误判。未以跳过任意字节的方式改动接收器，尚无可宣布修复的新 USB 二进制。

300 帧/10 秒实时候选实测失败（carlifebench6647920）：用户无画面，mdev 只接收 2 帧。多次 Missing AOA payload 后，RX #21+20 出现全零无效 AOA 头，RX #21 长 3596，前次 RX 长 8、上一 AOA channel=3 length=20。接收结束，live exit=65。图层切换/恢复成功，context 清理为空。不能归因于操作或窗口未启动，优先调查 AOA 读边界、音频通道丢块后的帧同步及共享 oap 读取；音频通道 3 处错误是线索，不代表已确定根因。暂停重复可见实测，不能将忽略错误或扫描任意 payload 当成修复。

2026-11-11 手机 USB 实时可见首测通过出图：用户确认出现 CarLife 界面，日志 `carlifebench3178608` 协商 1280×720@30，Queued 150 live USB frames，mdev accepted frames=150，首次 POST=1，图层恢复脚本成功，live exit=0，context 列表为空。期间多次 Missing AOA payload（命令/视频/音频通道），并两次记录 IDR 恢复，说明传输仍有不完整数据，具体原因未确认。界面出现已由用户确认，实时画面更新流畅度以及物理恢复 iDrive 尚需用户补充；不能据此宣称稳定、无丢包或音频可用。下一阶段优先排查 USB 接收完整性及断线恢复，不重复已通过的离线回放。

实时可见候选已构建（未实车验证）：`tools/carlife_usb_visible.sh CAPTURE_ELF REPLAY_ELF`，提示后插入手机，使用新增 `--stream-visible` / `--visible-stream` 接收最多 150 帧，沿用 45 秒 USB watchdog、4 MiB 有界 spool、后台按 30 fps 送帧、首次 POST 后 5 秒图层显示及恢复。原 20 帧隐藏模式保持不变。程序编译、新脚本语法与既有图层恢复模拟通过；150 帧 USB 接收、断线与实时可见行为仍待实测。首帧可能黑，观察整个显示时段；成功退出仍必须配合用户确认内容变化和 iDrive 恢复。传输更新两个 ELF 和新脚本，保留 guard 和 visible_first_frame 脚本。

2026-11-11 连续可见回放已获用户确认通过：重新上传此前被截断为 0 字节的 ELF 后，`--visible-clip carlife-visible-clip.mdev` 能显示变化的画面，约 5 秒后恢复 iDrive，clip exit=0，renderer context 列表为空。用户明确确认四项均满足。本结果覆盖下文连续回放“尚未上车”的历史状态。不再重复离线回放；下一阶段为有时限的手机 USB 实时可见流及断线恢复。现有手机 USB 实时 20 帧隐藏测试与本次离线可见连续回放分别成立，尚不能合并宣称实时可见投屏通过。

2026-11-11 连续可见回放候选已构建，尚未上车。新增 `--visible-clip PATH`，文件解析限最多 180 帧、约 2 MiB，模式要求 30 fps。后台线程按 30 fps 送帧，主线程观察首次 POST 并执行已有 5 秒显示/恢复脚本，随后等待送帧线程结束再清理 renderer。单帧入口继续要求恰好 1 帧。`build/carlife-visible-clip.mdev` 为参考记录 21–170，共 150 帧、1141791 字节，时间戳归零；独立解码第 21/81/141 个参考 IDR 成功，第 141 帧相对第 21 帧有 703737 个亮度像素差异超过 10，确认片段存在明显内容变化。ARM 编译和原恢复脚本回归通过，线程与连续显示的实机行为待验证。执行 `./carlife_qnx_mmr_replay.elf --visible-clip ./carlife-visible-clip.mdev`，该入口内部沿用 mdev 空闲、surface 冲突和图层基线检查；旧 show 脚本提示仍称 saved phone image。

用户确认 `carlifebench12935254` 显示了 `carlife-visible-content.mdev` 的实际画面，并恢复 iDrive。日志显示 mdev 接收 1 帧、renderer 首次 POST、surface 1510 切入主图层、5 秒后恢复 256、退出码 0，renderer context 列表为空。这是实际可见内容已通过的里程碑，不再重复单帧测试。复用当前父窗口初始化、同 ID 视频输出和图层恢复方案。下一步验证有时限的连续可见视频，再接手机实时可见流；现有手机实时 USB 20 帧只验证了接收与首次 POST。声音、iDrive 输入及长时间稳定性未验证。LayerManager 的 surface 1510 记录可能仍残留，不能把 context 列表为空等同于所有图形记录都已移除。

以下保留排查历史，涉及“待实车确认”的段落以本节最新结果为准。

已准备非黑测试文件 `build/carlife-visible-content.mdev`（75467 字节）。从只读参考样本 `samples/carlife-g9980-mdev-replay.bin` 提取第 21 个视频记录的 IDR，保留三个初始化记录，将该 IDR 时间戳归零，验证记录序列为 5,2,4,3。对应 SPS/PPS+IDR 已经 Media Foundation 独立解码，1280×720，亮度 13–239；PNG 目视确认是 CarLife 首页，有百度地图/高德地图、天气和播放卡片。原样本第 1 个 IDR 同样为黑。预览 `build/carlife-visible-content.png`。只上传这个 mdev 文件即可复用现有可见测试程序，手机无需连接。车机需要先移除已结束测试留下的 surface 1510，再运行 `sh carlife_visible_first_frame.sh ./carlife_qnx_mmr_replay.elf ./carlife-visible-content.mdev`。这张图来自既有参考采集，并非当前三星会话的新抓帧；车机显示内容仍待验证。

重要更正：用户在 `carlifebench12361814` 同 ID 可见回放仍见黑色后，提出采集帧本身可能为黑。已将原始 `build/carlife-first-idr.1716310.h264` 用 Windows Media Foundation 实际解码并导出 NV12 和 PNG。1280×720 的全部 921600 个亮度样本均为 16，预览确实为纯黑。此前只验证解码成功及尺寸，没有检查内容，因此不能再把该文件显示黑色作为出图失败证据，也不能据此证明视频内容已正确显示。暂停重复该黑帧测试，下一步应使用本地解码确认有内容的帧。`tools/decode-first-idr-win.cpp` 已增加可选 NV12 输出参数，并检查输出布局/stride；预览在 `build/carlife-first-idr.1716310.png`。本次车机切换/恢复、POST 和 context 清理成功，实质图像内容验证仍未完成。

第三轮 `carlifebench11501654` 实测出现空窗口，surface 1510 创建者为探针 PID 11501654，主图层切换和恢复成功，单帧接收和 renderer POST 成功，exit=0，context 清理为空。用户未看到手机内容，因此可见视频仍未通过。对照 NBT-AAD 输出 URL，发现上轮父窗口为数字 ID 1510，但 renderer 输出仍用 `carlifebench...-video`。最新改为父窗口和输出同用 1510，保持已补齐的父窗口初始化，与 NBT-AAD 的同 ID 绑定方式一致。已重新编译并通过恢复脚本回归，实际内容需再验证；只更新 ELF。历史独立 child ID 的记录仅描述旧版本。

第二轮 `carlifebench11063382` 确认失败在 surface 注册：mdev 接收一帧，show 脚本查询 5 次后仍没有 surface 1510，shell 原始状态 19456（退出 76），最终退出 73，清理为空。对照本机 NBT-AAD `createWindow` 的 `0x125390` 起始反汇编，之前只有 renderer 子窗口数字 ID，缺少应用父窗口的视频缓冲和首次提交。新版将 1510 ID 放到父窗口，子窗口恢复独立名字，并补充 display 绑定、VIDEO usage=8、BMW source-size 属性 40、RGBA format=8、单缓冲创建/填充/提交；仍保持隐藏和后台 z-order，之后才接收视频并调用已有 5 秒显示脚本。父窗口 POST 不计入 renderer 首帧证据。新增 API 链接车机 libscreen 成功，恢复脚本回归通过，尚待实车确认。此次只需上传 ELF，脚本沿用上一版。

第一轮可见实测 `carlifebench10440790` 未显示画面，单帧 mdev 接收成功，Screen 显示设置和读取 API 均返回 0，但在 LayerManager 的显示提示之前退出 73，context 清理为空。旧日志未打印可见性读回值和 shell 状态，无法区分提前要求读回 1 或 show 阶段基线不符。新版将切换前的读回值作为诊断，保留 API 错误中止和图层基线保护，并打印 LayerManager 调用状态、失败时基线以及切换后的 surface/layer 状态。编译和恢复脚本模拟通过，待实车验证；尚不能认定已修复物理出图。

用户实车读回确认屏幕 1280×480，主图层 `0x80000000` 的 render order 只有 `256(0x100)`，surface 256 可见。`get layer` 在完整输出后还有 on-screen 查询错误，预检据必要字段判断，不把这一项错误当作整层不存在。

新增 `--visible-file PATH`，仅接受保存的单帧 mdev。保持已有隐藏模式默认行为。可见模式使用独立数字窗口 ID / surface 1510，若已存在则拒绝，进入前检查 mdev 空闲和 iDrive 基线。收到首次 POST 后调用 `tools/carlife_visible_first_frame.sh --show`，等待该 surface 出现，短时切换主图层到它，居中显示为 854×480，保持 5 秒，恢复原来的 256 顺序和可见性。另有 15 秒后台恢复命令；它保障图层恢复，不保证任意系统故障下 renderer 清理。正常路径随后隐藏私有窗口、停止 renderer、解绑并销毁 context。不会改 surface 1280/1282 或屏幕总图层顺序。

可见性参数继续使用 BMW ABI 的 43，已对照本机 NBT-AAD `VideoManager::setVisible` 地址 `0x126338` 的参数，与公共 SDK 的 51 不同。新模式已编译，脚本模拟验证正常恢复、切换失败恢复、基线改变拒绝、surface 缺失/冲突及 layer 查询末尾附带错误；物理出图和恢复仍待这次监督实测。

上传 `build/carlife_qnx_mmr_replay.elf` 和 `tools/carlife_visible_first_frame.sh` 到 `/dev/shmem/`，保留原有 guard。停车并退出投屏后，在车机运行：

```sh
cd /dev/shmem
chmod +x carlife_qnx_mmr_replay.elf
sh carlife_visible_first_frame.sh ./carlife_qnx_mmr_replay.elf ./carlife-first-idr.1716310.mdev
echo "visible exit=$?"
ls /pps/services/multimedia/renderer/context
```

无需连接手机，观察保存画面是否出现、约 5 秒后是否返回 iDrive。即使 exit=0，也需人工确认可见画面；此测试不验证连续视频。若屏幕未恢复，可独立执行 `/opt/sys/bin/LayerManagerControl set layer 0x80000000 render order 0x100` 和 `/opt/sys/bin/LayerManagerControl set surface 256 visibility 1`。

本地脚本回归 `node tools/tests/visible-first-frame.cjs`，可用 `CARLINK_BASH` 指定 Git Bash。

更新日期 2026-11-10。目标是让 BMW NBT EVO ID6 通过 USB 接收手机 CarLife，并复用车机的 QNX Screen、MM-Renderer 和 mdev 视频链路。

## 当前进度

| 阶段 | 结果 |
| --- | --- |
| ARM32 加载器 | 已验证。加载器 ELF 在 `_init_libc` 返回后主动以 73 退出。 |
| QNX Screen | 实机创建窗口、设置隐藏、分配缓冲区、提交一帧和清理均成功。提交前后 `visible=0`，现场无闪动。 |
| MM-Renderer | 实机连接、独立 context、输出绑定及解绑均通过。 |
| CarLife H.264 回放 | 20 帧参考样本和三星手机实采的单帧均通过实机回放。实采单帧被 mdev 接收后，私有窗口 `carlifebench6250582-video` 从送帧前无 POST 变为收到 POST，`visible=0`。回放退出码 0，renderer 列表恢复为空。可见画面和车机输出像素内容尚待验证。 |
| 解码器交接 | AA 出图时使用 H.264 1280×720，断开后同一 context 被重配为 CarPlay 的 AVC1 1280×480。曾用 iDrive 的 AA Force Stop 使 renderer 列表清空，完成受控回放，随后用户确认 AA 恢复正常。CarPlay 真机出图尚未验证。 |
| 实时数据转换 | `src/carlife_live_video.cjs` 支持已经分离的明文视频流和带通道头的明文 AOA 数据。用现有 313 条视频消息验证，生成的 mdev 文件与参考回放逐字节一致。AOA 分包和交织通道测试使用合成 USB 外层，手机实时握手尚未验证。 |
| 手机 USB | Windows 识别 Galaxy S25 Ultra 为 `04E8:6860` 的 MTP 设备。临时 libusb 能枚举设备，但打开返回 `LIBUSB_ERROR_NOT_FOUND`，因此未发出 AOA 请求。没有更换 Windows 驱动。 |
| QNX USB 接收 | Galaxy S25 Ultra 已实测从 `/dev/android05` 切换到 `/dev/oap05`，通过 CarLife 1.0 明文握手，协商 1280×720 @ 30 fps，并采到 SPS、PPS 和首个 IDR。实车文件已通过 Windows 解码，输出一张 1280×720 NV12 图像。生成的单帧 mdev 文件与参考回放的初始化消息及首帧逐字节一致。 |
| 音频和 iDrive | 已对照 NBT-AAD 源码、符号和本机反汇编，整理 Browser PCM、A2I、MOST/CCS 以及 CarLife 消息和键码映射。CarLife 端尚未联调。见 [接口梳理](docs/nbt-aad-audio-input.md)。 |

## 在 Codex 里继续

把解压后的 `CarLink-Codex` 作为本地项目文件夹，选择在这台电脑上运行。先读本文件与 `AGENTS.md`。保持已经通过的实机结果，下次集中验证 USB 识别、CarLife 握手和首帧采集。

项目目录包含：

- `src/` 当前探针和流式转换源码。早期提取、回放及探针代码在 `src/legacy/`。
- `bin/` 当前和历史 ARM32 探针。文件清单记录各版本的 SHA-256。
- `samples/` 原始手机视频通道、H.264、完整回放和 20 帧短回放。
- `references/` 只读的 NBT-AAD 本机二进制、车机运行库、静态清单和匹配版本的上游参考源码。
- `tools/` ARM32 构建脚本和 USB 首帧采集脚本。
- `MANIFEST.sha256` 原始交接 ZIP 的完整性清单，用于核对交接时的文件。

## 本机复核

用 Node.js 处理样本。Windows 下可在 CMD 执行下面一行，保证重定向保留二进制。结果写到项目根目录。

```cmd
node src\carlife_live_video.cjs < samples\carlife-g9980-capture.bin > reproduced-replay.bin
```

预期为 2,354,867 字节，SHA-256 为 `28b2ed2bdfcb1150762042bb79719b4a43d993c5ff0c49bc200d53a52acd9830`。输入如果是真正的明文 AOA 批量数据，则使用 `--aoa`。这个程序只接收已经取得的字节流，USB 会话仍由接收端负责。

重建探针使用含 `clang.exe` 和 `lld.exe` 的 LLVM 15 工具目录。这个脚本只编译和链接，不启动探针。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-qnx-probes.ps1 -Toolchain 'D:\MATLAB\toolbox\sldrt\clang\win64'
```

收到 `first-idr.h264` 后，用采集日志中 `Phone encoder` 报告的宽、高和帧率生成单帧 mdev 文件。如果手机返回 0 fps，使用车机请求的 30 fps。以下以 1280×720、30 fps 为例，在 Windows CMD 中运行。

```cmd
node src\carlife_live_video.cjs --first-idr 1280 720 30 < first-idr.h264 > first-idr.mdev
```

同一份参考采集的首帧转换结果与既有 mdev 回放的初始化消息和第一帧逐字节一致。原来的裸 CarLife 视频和 `--aoa` 输入仍可生成完整回放。

Windows 上可先执行 `build\decode-first-idr-win.exe first-idr.h264`，通过 Media Foundation 实际解码并检查输出尺寸。交接样本的独立 IDR 已解码为 1280×720 NV12。若需重建，在项目目录下的 Visual Studio x64 开发者命令提示符运行：

```cmd
cl /nologo /W4 /WX /EHsc /Fo:build\obj\decode-first-idr-win.obj /Fe:build\decode-first-idr-win.exe tools\decode-first-idr-win.cpp /link mfplat.lib mfuuid.lib wmcodecdspuuid.lib ole32.lib
```

本机握手回归测试用模拟 USB 读写调用运行实际接收源码。覆盖延迟响应、版本拒绝、加密请求、三个阶段的断线以及等待超时。超时由 Windows 测试替身触发，车机的 QNX watchdog 仍待实测。在同一个开发者命令提示符内重建后可重复运行：

```cmd
set "CARLINK_CLANG=D:\MATLAB\toolbox\sldrt\clang\win64\clang.exe"
"%CARLINK_CLANG%" --target=x86_64-pc-windows-msvc -ffreestanding -std=c11 -Wall -Wextra -Werror -Dopen=test_open -Dclose=test_close -Dread=test_read -Dwrite=test_write -Ddevctl=test_devctl -Dalarm=test_alarm -Dusleep=test_usleep -Dpthread_create=test_pthread_create -Dpthread_join=test_pthread_join -c src\carlife_qnx_usb_capture.c -o build\obj\usb-capture-host-impl.obj
cl /nologo /std:c11 /W4 /WX /c /Fo:build\obj\usb-capture-host-mock.obj tools\tests\usb-capture-host.c
link /nologo /out:build\obj\usb-capture-host.exe build\obj\usb-capture-host-impl.obj build\obj\usb-capture-host-mock.obj libcmt.lib libvcruntime.lib libucrt.lib legacy_stdio_definitions.lib kernel32.lib
node tools\tests\usb-capture-host.cjs
```

## 实测记录和下一步

- 三星手机实采首帧已读回并生成 `build/carlife-first-idr.1716310.mdev`。首帧采集和单帧隐藏回放均已通过实测。
- 车机开机后原厂 `cpvideoctx16838` 占用 mdev，保护脚本拦截了回放。通过 iDrive 启动 Android Auto 并连接手机后，context 切换为 `aadvideoctx4563030`。退出 AA 后确认 context 列表为空，才开始回放。
- 下一步把 USB 连续视频接入私有 renderer，先确认首帧 POST 和断线清理，再验证可见连续画面。现有回放入口是 `sh carlife_qnx_replay_guard.sh PROBE FIRST_FRAME_MDEV`，会在已有 mdev 输入时退出。

### USB 实时隐藏窗口测试

USB 接收端从首个 IDR 开始逐帧发送到私有 renderer，使用这次手机会话协商的尺寸。当前测试限 30 fps，接收端最长运行 45 秒。它沿用隐藏窗口，测试通过要求 mdev 收到 20 帧、目标窗口观察到首次 POST，并清理自己的 context。

第一轮实时实测中 mdev 接收了 1 帧，随后 USB 视频内层头报异常长度 `0x01110000`。程序退出码 65，renderer context 列表恢复为空。为避免 renderer 启动或管道阻塞拖停 USB 读取，已把 USB 读取与管道写入分离，使用 4 MiB 有界缓冲。本机模拟在管道写入阻塞时仍能收完 20 帧，输出逐字节匹配参考。新版仍待上车验证，USB 帧头异常的来源尚未确认。

更新后的实测在一次视频 AOA 数据块缺失后从下一个 IDR 恢复。mdev 接收 20 帧，送帧完成后观察到 1 次隐藏窗口 POST，测试按至少两次的门槛返回 78，context 列表为空。回放端随后改成按 30 fps 间隔送帧，并在每次 mdev 接收后查询 POST，结束时再检查尾部事件。已模拟验证多次 POST、单次 POST 和断流清理。新版实测仍只观察到一次 POST，详情和判定更正见下文。

经授权且 mdev 空闲时，把新构建的 `carlife_qnx_usb_capture.elf`、`carlife_qnx_mmr_replay.elf`、`tools/carlife_usb_first_frame.sh` 和 `src/carlife_qnx_replay_guard.sh` 放到车机的 `/dev/shmem/`。先结束现有投屏并拔下手机，在车机运行：

```sh
cd /dev/shmem
chmod +x carlife_qnx_usb_capture.elf carlife_qnx_mmr_replay.elf &&
sh carlife_usb_first_frame.sh ./carlife_qnx_usb_capture.elf ./carlife_qnx_mmr_replay.elf
echo "live exit=$?"
ls /pps/services/multimedia/renderer/context
```

等提示后插入已解锁的三星手机。实时回放会在插线前、USB 切换后和 renderer 初始化前检查 mdev。USB 中途断开时，renderer 端应收到流结束并走停止、解绑和销毁流程。首轮异常退出已验证 context 清理，可见连续画面和主动拔线仍待实测。

这是一份本地交接包。抓包可能包含手机界面，车机二进制和库仅作为本地参考。包内没有 SSH 凭据、聊天原文、车辆编码备份或临时读回副本。

## 上游版本

本机 NBT-AAD 版本字符串为 `v1.9.36-combo`，与 [提交 08f98e1](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto/commit/08f98e176f93be008d1ae1771917138ae3801818) 的源码相符。版本字符串和关键函数分支已对照，发布二进制尚未做逐字节匹配。包内保留所用上游文件和其许可证。CarLife USB 旧版协议见 [车机端参考](https://github.com/674809/carlife/tree/master/CarLife-Vehicle-Lib/LibSource)。

### 2026-11-10 POST 判定更正

实车 carlifebench9887856 接收了 20 帧，在第 2 帧后观察到唯一一次 POST，USB 缺包后从 IDR 恢复，清理后 context 列表为空。旧版退出 78 来自错误的至少两次 POST 门槛。QNX 6.6 官方文档说明普通子窗口 SCREEN_EVENT_POST 在首次提交画面时发出，不能用事件数统计连续解码帧数。已修正为验证 20 帧 mdev 接收和首次输出，保留隐藏窗口。新构建通过，未再次上车运行。连续解码、物理屏幕出图以及 USB 缺包原因仍未验证，无需为修正退出码重复本次测试。

依据 [QNX Screen event types](https://www.qnx.com/developers/docs/6.6.0.update/com.qnx.doc.screen/topic/group__screen__events_1Screen_Event_Types.html)。
