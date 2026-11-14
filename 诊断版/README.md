### 安眠[息屏管理]
#### Magisk 模块

> ### 原理

- 守护进程常驻运行，用`dumpsys`获取当前前台应用包名（结果缓存`3秒`，并会记住上次成功的取法）。
- 每`10秒`检查一次`/data/adb/modules/anmian_screen_off/配置.prop`是否有改动，改完即生效，不用重启。
- 前台包名命中`配置.prop`里的`package`后开始计时；计时期间用`epoll`监听`/dev/input/event*`，有任何触摸就把计时清零。
- 超过`limit_time`没有触摸，就用`input keyevent 223`（老系统退回 26）熄屏，并再次读取屏幕状态确认是否真的熄灭。
- 触摸监听首选直接读`/dev/input`，读不到时退回`getevent`；屏幕熄灭期间会关闭监听，避免无谓唤醒。

> ### 用法

- 安装后在 **KernelSU / APatch 管理器**里打开本模块，会直接进入 WebUI：添加应用、设置时间、保存即可（10 秒内生效）。
- 升级模块不会丢配置：安装脚本会保留原来的`配置.prop`，WebUI 顶部会显示读到的来源（模块配置 / 待生效配置 / 上次备份）。
- 换机或重装想沿用旧配置：把旧的`配置.prop`放到`/sdcard/Download/`，在 WebUI 点「从 Download 导入」；保存前的旧文件会自动备份，可点「恢复备份」回退。
- Magisk 用户没有自带 WebUI，装一个「WebUI X」模块后同样可以从管理器打开；也可以在 Magisk 里点本模块的「操作」按钮看状态和日志。
- 也可以在文件管理器里改`/data/adb/modules/anmian_screen_off/配置.prop`，或者改压缩包里的`配置.prop`再刷入。
- 一个应用写一组`package` + `limit_time`；`limit_time` 写在对应的 `package` 下面。模块自带四组示例：
```
# 抖音
package=com.ss.android.ugc.aweme
limit_time=30m

# 抖音火山版
package=com.ss.android.ugc.live
limit_time=30m

# 快手
package=com.smile.gifmaker
limit_time=30m

# 快手极速版
package=com.kuaishou.nebula
limit_time=30m
```
- `limit_time` 支持 `数字(s|m|h)`：`1m`=1分钟、`30s`=30秒、`1h`=1小时；不写单位按分钟算（`5` = 5分钟），整项不写默认 30 分钟。

> ### 日志与排查

- 日志在`/data/adb/modules/anmian_screen_off/运行日志.log`，超过`100KB`自动清空重写。
- 不想记录日志：把`service.sh`里的`debug=true`改成`false`。
- 日志里出现`已息屏`说明生效；出现`息屏未生效`多半是系统拦截了按键事件。
- 确认包名可用`dumpsys window | grep mCurrentFocus`，前台应用会出现在`u0`后面。

> ### 诊断

换机型、或者某个机型上"到时间不熄屏"时，有两种方式：

**方式一（推荐）**：WebUI 的「运行状态」里点「诊断」，结果直接显示在下面。本包是「诊断版」，需要收集设备环境、屏幕、输入设备等完整信息反馈问题时，点 WebUI 的「导出报告」或管理器「操作」即可生成报告。

想确认这台机器认不认按键注入，点「测试息屏」：它会马上走一次和自动触发完全相同的流程（`KEYCODE_SLEEP(223)` → 电源键 26 → 回读屏幕），成功会显示「息屏成功」，被拦截会显示失败原因，不用等 30 分钟。

**方式二**：命令行跑一次：

```
su -c '/data/adb/modules/anmian_screen_off/App_screen_off --diag --config=/data/adb/modules/anmian_screen_off/配置.prop' > /sdcard/diag.txt
```

会依次打印：屏幕判定结果、背光节点与实际读数、`dumpsys power/display/deviceidle` 的原始输出、取前台应用的每条命令及其解析结果、`/dev/input` 下每个 event 设备是否被识别为触控、`epoll` 是否可用、以及当前配置。

**最后一段是结论**，不用自己判断：

```
=== 结论 ===
诊断结论: 可用
- 触摸监听: 正常（1 个触控设备）
- 前台识别: 正常（当前 com.ss.android.ugc.aweme）
- 屏幕判定: 正常（背光节点可读）
- 息屏按键: 诊断无法验证，请点「测试息屏」确认
```

结论只有三种：`可用`（三环都通）、`疑似可用`（缺触摸或前台），`需要自行测试`（都没读到）。WebUI 里会用彩色条直接显示这个结论。

重点看两处：
- `背光节点` 有值但和 `屏幕判定` 对不上，说明判定链有问题；
- `触控=N` 如果是 0，说明这台机器没读到触摸设备，模块会暂停息屏而不是乱灭屏。

> ### 加应用

- WebUI 的「已配置应用」下面直接输入包名（例如 `com.ss.android.ugc.aweme`），回车或点「添加」，再点底部「保存配置」。
- 包名可以在系统设置的「应用信息」里看，或者用 `dumpsys window | grep mCurrentFocus` 看前台应用 `u0` 后面那段。


