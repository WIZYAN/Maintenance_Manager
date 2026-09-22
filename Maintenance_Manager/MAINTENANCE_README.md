# 保养倒计时模块

## 已实现的规则

- 时间标准仅来自串口屏 RTC，按日历日期计算，每跨过午夜减少一天。
- 断电期间继续计入保养周期：上电读取 EEPROM 记录和屏幕当前日期重新计算。
- 初始周期为 180 天。首次使用且两个 EEPROM 保留页全部为 `0xFF` 时，等到有效 RTC 回包后开始计时并保存。
- 修改周期保留起始日期：经过 30 天后从 180 改为 150，剩余 120 天。
- 到期显示 0 天，状态为 `MAINTENANCE_DUE`，不会自动开始下一轮。
- 确认完成保养后，从当天重新计时。
- RTC 无效、超时或日期回退时，`countdown_valid=false`，不将旧的剩余天数当作有效值。
- 已损坏/非本模块格式的 EEPROM 数据不会被当作首次使用自动覆盖。

## 调用方式

`src/hal_entry.c` 已接入：

```c
while (1)
{
    Maintenance_Task();
    /* 其他非阻塞任务 */
}
```

修改周期或完成保养，在**主循环上下文**调用：

```c
Maintenance_result_t result;
result = Maintenance_SetPeriodDays(150);
/* 只有 result == MAINTENANCE_OK 时，才提示用户“设置已保存”。 */

result = Maintenance_Reset();
/* 只有 result == MAINTENANCE_OK 时，才提示用户“保养记录已更新”。 */
```

不要每次循环都调用 `Maintenance_Reset()`，只在用户确认完成保养的事件发生时调用。
不要在串口中断或 SysTick 中调用以上业务函数。两个操作函数需要最近 3 秒内取得有效 RTC 时间。
设置相同周期直接成功返回，不重复写 EEPROM。周期范围为 1～36500 天。

两个 EEPROM 页都损坏时，`Maintenance_Reset()` 是用户明确确认后的恢复入口，会使用默认 180 天周期重新建立记录；
不会自动恢复原来已无法读出的周期。仅通信故障时先恢复 I²C，模块每 5 秒重试加载记录。

## 两组结构体

`src/Maintenance.h` 定义并导出两个变量：

| 变量 | 字段 | 含义 |
|---|---|---|
| `Maintenance_save` | `period_days` | 保存的保养周期 |
| | `start_day` | 本轮起始日期，以 2000-01-01 为第 0 天 |
| | `saved_day` | 最近一次设置时的日期 |
| | `sequence` | 保存记录的序号 |
| `Maintenance_para` | `rtc` | 最近一次有效的屏幕年月日、时分秒 |
| | `remaining_days` | 剩余天数，读取前检查 `countdown_valid` |
| | `current_day / due_day / elapsed_days` | 当前日期、到期日期和已过天数 |
| | `status` | 等待、运行、到期、RTC 故障、存储故障、硬件故障 |
| | `last_result` | 最近一次设置/存储操作结果 |
| | `rtc_valid / record_valid / countdown_valid` | 时间、存储记录和倒计时是否有效 |
| | 其他字段 | 状态机、串口收发、时基等过程变量 |

这两个变量由模块管理，应用程序可以读取；**设置请调用函数，不要直接修改字段**，否则不会完成掉电保存和一致性校验。

## 硬件与资源

- 串口：FSP `DIS_UART`，SCI9，115200 / 8N1，回调 `DIS_Callback()` 已实现。
- MCU P109/TXD9、P110/RXD9 经 SP3223 与屏幕通信；屏幕需使用匹配的 RS-232 接口。
- P113 `DIS_POWER`、P114 `DIS_EN` 在初始化时拉高。等待 2 秒后开始轮询 RTC；屏幕启动更慢时会继续重试。
- EEPROM：24C256，7 位地址 `0x50`（A0/A1/A2 接地）。P608/SCL、P609/SDA 采用开漏软件 I²C，P115 控制 WP。
- 默认使用两个 64 字节页：**0x7F00～0x7F3F、0x7F40～0x7F7F，共 128 字节**。其他模块不得占用，可在配置头中调整。
- 模块占用 **SysTick，1 ms 中断**；以后若接入 RTOS 或已有系统时基，需要改造 `Maintenance_PortNow()` 和时基初始化，不能重复定义 `SysTick_Handler()`。
- 通常任务轮询不等待串口回复。EEPROM 加载/保存为有超时的同步操作，期间中断保持开启；保存会占用数十毫秒，具体时间需在实板测量。不要把它放入要求严格周期的控制中断。

## 断电保存方式

EEPROM 使用独立序列化格式，不直接保存 C 结构体内存，因此不依赖编译器填充字节。
每份记录包含标识、版本、长度、序号、参数及 CRC32，页末带提交标记。

保存顺序：使目标备用页失效并确认 → 写入新数据并回读 → 写入提交标记 → 回读并校验完整记录。
当前有效页始终保留。上电选择序号最新的有效记录，损坏时回退到另一份完整记录。
日常 RTC 轮询和剩余天数变化不写 EEPROM，仅首次建立、修改周期、确认保养完成时写入。

保存中断电时，新设置可能没有提交，应以重启读出的完整记录为准。
如果提交完成但最后回读失败，接口仍返回错误；模块后续重新加载 EEPROM，恢复到实际已提交的记录。

## 串口屏画面尚未制作

核心计时和存储已启用，**显示控件 ID 默认 0xFFFF，暂不发送控件更新指令**。
页面做好后，在 `src/maintenance_config.h` 填写：

```c
#define MAINTENANCE_SCREEN_ID          /* 画面 ID */
#define MAINTENANCE_REMAIN_CONTROL_ID  /* 剩余天数文本控件 ID */
#define MAINTENANCE_PERIOD_CONTROL_ID  /* 保养周期文本控件 ID */
#define MAINTENANCE_STATUS_CONTROL_ID  /* 状态文本控件 ID */
```

三个输出均使用 **文本控件**，采用 `EE B1 10 Screen_id Control_id Strings FF FC FF FF` 更新，ID 为大端两字节。
剩余天数无效时显示 `--`；状态文本为 `WAIT RTC / RUNNING / DUE / RTC ERROR / EEPROM ERROR / HW ERROR`。
目前未绑定屏幕按钮和输入框回包；制作页面后在主循环解析其事件，再调用两个操作函数。
现有接收器会安全跳过其他控件报文，不会将按钮报文误当 RTC。

屏幕使用**大彩组态协议、关闭串口 CRC、115200 / 8N1**：

```text
读取 RTC：EE 82 FF FC FF FF
RTC 回包：EE F7 年 月 星期 日 时 分 秒 FF FC FF FF
```

时间字段均为 BCD，年份按 2000～2099 解释，星期 0～6（星期日为 0）。
代码检查报文长度、帧尾、BCD、月份天数和闰年，不使用屏幕普通计时控件累计天数。
协议依据：[大彩官方 RTC 说明](https://doc.gz-dc.com/Control/06_RTC.html)、
[大彩指令集 V5.1，2.39 读取 RTC](https://www.gz-dc.com/UPLOADS/FILE/20200702/%E5%A4%A7%E5%BD%A9%E4%B8%B2%E5%8F%A3%E5%B1%8F%E6%8C%87%E4%BB%A4%E9%9B%86V5.1%20.PDF)。
尚未取得 DC10600PM101 的专用手册，需实机确认它使用上述协议及 RTC 格式。

## RTC 校时规则与边界

首次使用前，先校准屏幕 RTC，并确认其备用电池能维持整机断电后的走时。
有效日历不等于实际时间正确；模块无法判断一个格式正确的出厂日期是否已校准。
检测到日期早于本次开机已见到的日期，或早于 EEPROM 保存日期时，暂停有效倒计时，待屏幕日期纠正后自动恢复。
同一天内调整时分秒不影响日历天数。日期向前调整会提前到期。
为避免每天写 EEPROM，模块不会持久化每天的时间水位；无法检测所有跨断电的人工回拨，例如回拨后仍晚于最近保存日期。
屏幕时间在 2099 年后需要升级日期协议/版本。主控的毫秒时基仅用于通信调度，支持 uint32 回绕。

## 编译和测试

工程已将 `src` 配置为源码目录，新增 `.c` 文件会参与 e2 studio 构建，不需要手改 `ra_gen` 文件。
命令行检查脚本位于仓库根目录 `tools/check_maintenance.py`：

```text
python tools/check_maintenance.py
```

脚本先用主机 GCC 运行核心/存储/协议测试（显示禁用与显示启用两种配置），再用本机 ARM GCC 10.3 编译全部源码、链接，输出 ELF、HEX 和 map 到 `tmp/maintenance-build`。
链接使用工程当前的 `rdimon.specs` 与 `nano.specs`，未执行烧录。
可用 `--arm-gcc`、`--host-gcc` 指定其他工具链；`--host-only` 只运行主机测试。

已覆盖：2000～2099 所有有效日期、闰年、跨年、修改周期、到期、保养复位、断电日期推进、
RTC 拆包/坏包/超时/超长帧、串口丢字节标记、CRC 损坏、每一个写入字节位置的掉电模拟、
存储读取失败恢复、毫秒回绕、可选文本显示。

主机模拟不验证真实 I²C 电气时序、屏幕协议兼容性及 EEPROM 实际掉电行为；这些要在实板验证。
