# 无线：手机在车机热点上没网，以及"车机连手机热点"方案（2026-11-13 晚，进行中）

## 现象与原因
- 手机连车机热点 uap0 后提示"无互联网"，移动数据也不再走（状态栏只有 Wi‑Fi 标）。断开车机热点后恢复。
- 车机 dhcpd 发 `option routers 0.0.0.0`（来自 pss_config `NetworkingService_uap0.defaultgateway = 0.0.0.0`），DNS 指向 172.16.224.1（DnsMasq 回 REFUSED）。
- 试过：删掉网关行重启 dhcpd（car-12 dhcp-fix / car-14 dhcp-start）、手机开"始终开启移动数据"，都不行 → 是三星手机侧把未验证的 Wi‑Fi 留作默认网络。已用 car-13 dhcp-undo 恢复原配置（dhcpd 现不由 scp 管理，重启车机即完全恢复）。
- 百度官方无线 CarLife 拓扑是"手机开热点、车机当客户端"；三星车联里也有"使用手机热点"选项。

## 车机 Wi‑Fi 硬件（只读取证）
- Marvell SD8787，固件 `/opt/conn/etc/sd8787_uapsta.bin`（AP+STA）。
- 驱动 `/opt/conn/lib/devnp-mv8787-omap5.so`，挂载选项 `num_client`（默认 1，名 sta）、`num_ap`、`num_wfd`、`name_client`、`name_ap`、`name_wfd`；由 scp 的 NetworkingService 只以 AP 角色挂载。
- `/opt/conn/bin/wpa_supplicant` v0.7.3（wext）。
- `libnetworkingservice.so` 按角色表拼挂载参数（0→AP，1→client，5/6→wfd），会执行 `wpa_supplicant%s -D wext -i %s -c %s -P %s` 和 `dhcp.client -b -d %s -i %s`，客户端接口名 `wcl0`。
- 取回的二进制：`carlink-logs/fetched/`（scp、WlanDSICommander、libnetworkingservice.so、devnp-mv8787-omap5.so）。

## BSS 角色切换（原厂 get_set_bss_role 复刻：src 中 carlife_qnx_bssrole.c）
- `MsgSendv(socket(AF_INET,SOCK_DGRAM,0), …)`：devctl 头 type 0x106、combine_len 16、dcmd 0x801c697b（SIOCSDRVSPEC）、nbytes 0x4c；ifdrv（28 B）cmd 0x8be5（uap0）；iwreq（32 B）length 1 设/0 读，flags 1（UAP）/0x15（STA）；角色是第 4 段 4 字节。
- 实车 `--get uap0` 返回 **EPERM**；驱动路径没有 EPERM，推测是 io-pkt 对 SIOCSDRVSPEC 的 kauth 检查。`--get-g`（SIOCGDRVSPEC 0xc01c697b）待测：`car-19-bss-get.cmd`（同时收 sloginfo）。

## 后续思路（每一步上车前都要用户同意、车停稳）
1. 跑 car-19 看 `--get-g` 是否能读到角色。
2. 若能切角色或用 `num_client=1,name_client=wcl` 重挂驱动：wpa_supplicant `-Dwext` 连手机热点 + `dhcp.client`。
3. pf：在未用的锚点（如 `filter_USB`）里加放行规则（需单独同意，改防火墙）。
4. `--stream-wifi` 的 IP 校验放宽到手机热点网段。
5. 任何失败：重启车机恢复原厂。
