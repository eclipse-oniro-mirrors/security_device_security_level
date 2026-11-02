# 运维观测：日志、打点、dump

← 返回 [`AGENTS.md`](../../AGENTS.md)

---

## 日志标识

| 项 | 值 | 定义位置 |
|---|---|---|
| log tag | `DSLM_SERVICE` | `baselib/utils/include/utils_log.h` |
| log domain | `0xD002F05` | 同上（通过 HiLog 宏使用 `LOG_CORE`） |
| 服务进程 | `dslm_service`（standard） | `profile/dslm_service.json`；启动脚本源文件为 `profile/dslm_service.cfg` |
| SA ID | `3511` | `common/include/idevice_security_level.h` |
| hitrace tag | `HITRACE_TAG_DLSM` | `services/dfx/dslm_hitrace.cpp` |

抓日志的起点（命令需在已连接设备、完整 OpenHarmony 调试环境中执行）：

```shell
hdc shell hilog | grep -i DSLM
hdc shell hilog -D 0xD002F05
hdc shell "ps -ef | grep dslm_service"
```

日志宏统一封装为 `SECURITY_LOG_DEBUG/INFO/WARN/ERROR/FATAL`，自动加当前函数名。不要打印凭据原文、私钥、完整 udid 或原始消息；定位设备使用 `machineId`、cookie、transNum、结果码与长度。

---

## 关键日志观测点

### 服务启动与初始化

| 观测点 | 文件/函数 | 判断 |
|---|---|---|
| `InitService` / `InitMessenger` | `services/sa/common/dslm_rpc_process.c` | messenger 是否创建 |
| `InitMessenger ret` | 同上 | DSoftBus/messenger 初始化失败原因 |
| `wait SoftBus timeout` | 同上 | 初始化持续失败，达到 `times > TRY_TIMES` 的退出条件；实际等待次数和耗时见 [`failure-modes.md`](failure-modes.md)「通信层失效」 |
| `InitSelfDeviceSecureLevel` | `services/dslm/dslm_core_process.c` | 本机安全级别初始化是否成功 |
| `ProcessLoadPlugin` | `services/sa/standard/dslm_service.cpp` | `PLUGIN_SO_PATH` 是否加载成功 |

### 请求与应答链路

| 观测点 | 文件/函数 | 判断 |
|---|---|---|
| `OnRequestDeviceSecLevelInfo` | `dslm_core_process.c` | SDK 请求是否进入 core |
| `ProcessSdkRequest` | `dslm_fsm_process.c` | 请求是否进入 notifyList |
| `SendCredRequest` / `challenge` / `transNum` | `dslm_fsm_process.c` / `dslm_inner_process.c` | 是否实际发起 challenge 请求；日志只输出截断 challenge |
| `OnPeerMsgReceived` / `packet->type` | `dslm_rpc_process.c` | 消息是否通过一级解析并分发 |
| `ParseDeviceSecInfoRequest/Response failed` | `dslm_msg_utils.c` | 二级消息字段或编码错误 |
| `ProcessVerifyCredMessage` | `dslm_fsm_process.c` | nonce、凭据、等级校验结果 |
| `RequestDone` / `SdkTimeout` | `dslm_fsm_process.c` | SDK 回调完成或超时 |

### 状态与通信问题

`ScheduleMachine` 会记录 `machineId`、旧状态、事件和新状态。排查「无反应」时先核对当前状态与事件是否存在于 `stateNodes[]`；事件无匹配边时只会留下调度日志，不会自动报错。

`OnMsgSendResultNotifier` 会记录 `transNo` 与 result；只有 transNum 相等且当前尚未拿到凭据时才会触发重发。若发送失败日志出现但没有重发，先检查这两个条件。

---

## dump 命令

standard SA 实现 `DslmService::Dump`，支持 `-h` 和 `-l`：

```shell
hdc shell hidumper -s 3511 -a "-h"
hdc shell hidumper -s 3511 -a "-l"
```

`-l` 输出每台设备的：

- online/offline 状态与时间；
- request/response/verify 时间和耗时；
- pending notify 数量；
- FSM 状态（`STATE_INIT`、`STATE_WAITING_CRED_RSP`、`STATE_SUCCESS`、`STATE_FAILED`）；
- 当前记录的等级与结果、凭据类型、版本、厂商、型号、软件版本；
- SDK 调用历史（pid、cookie、结果和耗时）。

输出 `REQUEST_TEST` / `VERIFY_TEST` / `SELF_CRED_LEVEL` 可快速区分「本机凭据获取失败」与「对端查询失败」。设备详情中的 `DEVICE_VERIFIED_LEVEL` 和 `DEVICE_VERIFIED_RESULT` 来自设备对象当前字段；这些字段也可能由设备上线或本机初始化路径写入，不能只凭非零等级或 `success` 文本认定凭据校验已经通过。注意 dump 是诊断接口，新增字段不得放入凭据原文、私钥或完整设备标识。

---

## HiSysEvent 打点

事件域为 `DSLM`，定义在 `hisysevent.yaml`，实现集中在 `services/dfx/dslm_bigdata.cpp`：

| 事件 | 类型 | 触发 |
|---|---|---|
| `SERVICE_START_FAILED` | FAULT / CRITICAL | messenger 或服务启动失败，字段 `ERROR_TYPE` |
| `INIT_SELF_LEVEL_FAULT` | FAULT / CRITICAL | 本机安全级别初始化失败，字段 `ERROR_STR` |
| `CALL_INTERFACE` | STATISTIC / MINOR | 外部模块调用 DSLM 接口，含耗时、结果码、等级、返回模式和模型字段 |
| `QUERY_INFO` | STATISTIC / MINOR | 查询对端信息，含本地/目标版本、凭据类型、结果码、耗时和等级 |

C 层入口：`services/dslm/dslm_hievent.c` 的 `ReportHiEventServiceStartFailed`、`ReportHiEventInitSelfFailed`、`ReportHiEventInfoSync`、`ReportHiEventAppInvoke`。

查看事件：

```shell
hdc shell hisysevent -r -d DSLM
```

新增失败分支时应补故障类打点；字段不能包含凭据材料、私钥或不必要的设备标识。模型字段属于已有事件契约，修改其含义需要评审。

---

## Hitrace 与其它观测手段

`services/dfx/dslm_hitrace.cpp` 提供：

- `DslmStartProcessTrace` / `DslmFinishProcessTrace`：服务初始化；
- `DslmStartStateMachineTrace`：状态机事件；
- `DslmStartProcessTraceAsync` / `DslmFinishProcessTraceAsync`：SDK_GET 和 SendCredRequest；
- `DslmCountTrace`：初始化重试次数。

相关命令：

```shell
hdc shell hitrace --trace_begin dslm
# 复现后按设备工具链方式结束并导出 trace
```

若服务反复重启，结合 `ps` 与 faultlog；若状态长期不变，优先看 dump 的 FSM 状态、pending 数量和 request/response 时间差，再看 `DSLM` 故障事件。

---

## 推荐排查顺序

1. `hidumper -s 3511 -a "-l"`：确认服务自身看到的设备、状态和等级。
2. `hilog -D 0xD002F05`：先看启动/错误日志，再按「请求与应答链路」定位断点。
3. 对照 `machineId`、`transNum`、cookie 和状态，判断是设备状态、消息解析、凭据校验还是回调超时。
4. `hisysevent -r -d DSLM`：确认是否有启动故障或接口/查询统计。
5. 状态机不动时查 [`failure-modes.md`](failure-modes.md)「FSM 状态卡死与静默丢事件」；级别为 0 时查「凭据相关失效」。
6. 需要时使用 Hitrace 复核 SoftBus、状态机和 SDK 回调的时序。
