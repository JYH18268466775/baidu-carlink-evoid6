# carlife 离线工作状态（2026-11-12 03:30）

## 已完成并通过离线测试
- 连续投屏（CARLINK_SESSION_SECONDS=0，环形缓冲、时间戳修正、停止/隐藏文件、看门狗）。
- carlife_qnx_ctl.c（UDP 52790 控制端，运行时加载 libsocket）。旋钮监听 --watch 0 / 倒车泊车不发键。
- 启动脚本 CARLINK_PLUGGED_OK；显示脚本 SHOWN 标记。
- 菜单监督脚本 carlife-menu/package/nbt-aad：sh/bash 共 54 项主机测试全过（等手机、隐藏/显示、倒车、泊车、quit/SIGTERM/kill-9、重试3次放弃、端口被占、初始倒车）。
- 菜单文字改为 CarLife（10 语言，保留上游署名行）；clmenu_install/uninstall、upload-carlife-menu.cmd（已测）。
- 无线 CarLife 调研与设计：wireless-carlife-design.md。

## 待做
- 用户在 Windows 用 LLVM15 重建（build-qnx-probes.ps1 新增 ctl ELF 与 getppid/poll 符号），核对未改 ELF 哈希；然后 upload-carlife-menu.cmd、clmenu_install.sh --yes、appreset（均需用户授权、车停稳）。
- USB 读线程分离（专用读线程+处理线程）。
- 无线：按设计文档第4节实现，先上车只读确认第5节。
- 上车验证：监督脚本在真实 sh 下的行为（QNX sh 非 dash/bash）、ctl 在车机上能否 bind、倒车/泊车让屏、卸载路径；测完重启车机恢复原厂 CarPlay。

## 2026-11-12 12:10 重建结果（用户 LLVM15）
- usb_status 512D23E3… 不变；cp_purge BAB65890… 不变；capture C8EF174C…；mmr_replay 9911941E…（源码改了，与 wide 基线不同属预期）；most_watch 96275478…；ctl 0EF676D6…。
- check-build.cjs 已改为：status 须与验证版一致，capture/replay 须不同，watcher/ctl/purge 只验证是完整 ARM ELF；通过。用户 PC 没有 node，检查由我在沙箱对已暂存的 ELF 运行。
- 下一步：用户授权后 upload-carlife-menu.cmd → clmenu_install.sh --yes → appreset（车停稳）。
