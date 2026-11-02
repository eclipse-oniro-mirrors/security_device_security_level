# 失效模式与根因模式

← 返回 [`AGENTS.md`](../../AGENTS.md)

> 排查 bug 时从这里开始。本仓的问题高度集中在少数几种模式上，先按这里的清单排除，比通读代码快得多。

---

## 外部输入解析失败

本仓位于网络通信边界：一级消息（`dslm_msg_serialize.c`）和二级 payload（`dslm_msg_utils.c`）处理的都是**对端设备发来的不可信字节流**，这也是 `test/dslm_fuzz_test/` 存在的原因。

### 现象特征

- 对端查询本机无应答，或本机查对端一直失败。
- 日志只有一句 `ERR MSG` / `ERR JSON MSG` / `packet is null`（debug/error 级），没有更多上下文。

### 已知校验点（改动时不要削弱）

| 校验 | 位置 | 拒绝条件 |
|---|---|---|
| `CheckMessage` | `dslm_msg_serialize.c` | 空指针；长度 ≤1 或 > `MSG_BUFF_MAX_LENGTH`；包含非 ASCII 字节（允许 ASCII 控制字符）；未以 `\0` 结尾 |
| `ParseMessage` | 同上 | 缺 `message` 字段（类型 < 0）或缺 `payload` |
| `ParseDeviceSecInfoRequest/Response` | `dslm_msg_utils.c` | 缺 `challenge`、challenge hex 解码失败或为 0、response 缺 `credInfo` 或 base64 解码失败 |
| IPC parcel 校验 | `dslm_ipc_process.cpp` `DslmGetRequestFromParcel` | 可读字节数不大于固定预期长度、`ReadBuffer` 失败、cookie 为 0、回调对象为空；timeout 不在 1–60s 时改用默认值 45s |

### 排查方法

将解析得到的字段与 `dslm_msg_utils.c` 的 `Build*` 函数产物逐字段对比，或结合本地安全诊断信息定位问题。字段名不匹配、challenge 编码方式不一致（hex 字符串 vs 原始字节）、base64 编解码不对称，都可能表现为「静默解析失败」，**不会有明确的报错点**。

---

## 凭据相关失效

凭据链路长（证书链 → nonce → 凭据内容 → 等级上限），不同失败分支的结果码和字段状态并不完全相同；不能仅凭当前 `credLevel` 判断失败原因。按层排查：

| 层 | 失效形态 | 根因候选 |
|---|---|---|
| 函数表 | 凭据操作返回 -1 | `InitDslmCredentialFunctions` 未执行：库未链接、`constructor` 未执行，或配置了 `device_security_level_feature_plugin_path` 但插件加载失败。函数表为空时默认凭据函数会记录 info 级日志；插件 `dlopen` 失败另有 error 级日志（见 `dslm_service.cpp::ProcessLoadPlugin`） |
| 本机凭据获取 | `RequestOhosDslmCred` 失败 | `/system/etc/dslm_finger.cfg` 不存在或不可读（`GetCredFromCurrentDevice` 的 `fopen` 失败）；`HksAttestIsReadyAdapter` 未就绪时凭据类型降级（`SelectDslmCredType`：未就绪给 SMALL/MINI 类型） |
| 证书链校验 | `ValidateCertChainAdapter` 失败 | device_auth/HUKS 侧问题；系统时间异常可能导致证书时间有效性校验异常（hidumper 的 `VERIFY_TEST` 输出在系统时间早于 2023-11 时附带 `(please check the system time)` 提示，见 `dslm_hidumper.c`） |
| nonce | `ERR_CHALLENGE_ERR` | 应答的 challenge 与本机 nonce 不等或为 0；`L2_STANDARD` 下还有 60s 过期检查 |
| 等级上限 | 等级检查失败，`credLevel` 被清零并返回 `ERR_CHECK_CRED_INFO` | `CheckCredInfo`：small/mini 类型凭据等级 >2、standard >5 时清零并返回错误；这是凭据校验失败，不是校验成功后丢失等级 |
| debug 凭据 | `ERR_CHECK_CRED_INFO` | `udid` 为空时跳过检查；当 `releaseType` 以 `debug` 开头且 udid 非空时，按凭据中的 udid 长度与设备标识比较；release 凭据不执行这项比较 |

> **关键陷阱：`credLevel == 0` 可能表示「尚未取得等级」「凭据校验失败」，也可能表示校验流程发现等级超限后清零。** 解析或 nonce 校验失败时，已有凭据字段不一定会清理；其它失败分支也可能留下旧值或本轮部分解析值。因此不能仅凭当前等级判断失败原因，应结合错误码、验证状态和日志区分，见 [`observability.md`](observability.md)「dump 命令」。

---

## 重复请求判断

`REQUEST_INTERVAL` 定义为 24h，但当前 `CheckNeedToResend()` 先按 `credLevel` 返回：等级大于 0 时返回 `false`，等级为 0 时返回 `true`，因此后面的时间比较不可达。排查重复请求时，应以当前等级分支和实际事件序列为准，不要把 24 小时当作已经生效的去重窗口。

---

## 通信层失效

| 失效 | 表现 | 根因候选 |
|---|---|---|
| DSLM 初始化未完成 | 服务启动期间 `wait SoftBus timeout` 日志；请求阶段若 messenger 未就绪，可能返回 `ERR_MSG_NOT_INIT` | `InitService` 按 `TRY_TIMES` 重试 `InitDslmProcess()`；它还可能因本机设备标识获取或设备对象创建失败而返回 false，不只是 messenger 未就绪。当前退出条件为 `times > TRY_TIMES`，连续失败时约执行 22 次 `sleep(0.5s)`；总耗时还受每次初始化调用影响。循环退出本身不能证明 messenger 不可用或后续请求必然返回 `ERR_MSG_NOT_INIT`（`dslm_rpc_process.c`、`dslm_core_process.c`） |
| 会话打开失败 | 挂起的 SDK 请求立即收到 `ERR_MSG_OPEN_SESSION` 失败回调 | `ProcessSendRequestFailed` 对 `ERR_SESSION_OPEN_FAILED` 的特判：不再重发，直接通知所有等待者 |
| 迟到的发送失败通知被丢弃 | 预期中的重发没有发生 | `OnMsgSendResultNotifier` 校验 `transNum` 相等且 `credLevel == 0` 才转成 `EVENT_MSG_SEND_FAILED`；nonce 已刷新或已有凭据时通知被有意丢弃——**这是设计，不是 bug** |
| 设备上下线状态错 | 请求返回 `ERR_NOT_ONLINE` 或上线后无反应 | 状态来源是 device_manager 经 msglib 的 `statusReceiver` 上报；先 dump 看 `DEVICE_ONLINE_STATUS` |

---

## FSM 状态卡死与静默丢事件

### 事件被静默丢弃

`ScheduleMachine` 找不到与 `{state, event}` 匹配的边时**不会继续处理**（`utils_state_machine.c` 只记录一条调度日志）。表现为「某个场景下完全无反应、无报错」。

排查：先确认设备当前状态（dump 的 `DEVICE_MACHINE_STATUS`），再查 `stateNodes[]` 里有没有这条边。**新增事件/状态后忘了补边是本仓最容易引入的缺陷。**

### `STATE_FAILED` 的出路

进入 `STATE_FAILED` 后，`EVENT_DEVICE_ONLINE` 和 `EVENT_CHECK` 可以重新进入请求流程，`EVENT_CRED_RSP` 则负责处理迟到或重入的应答；`EVENT_DEVICE_OFFLINE` 会回到 `STATE_INIT`，`EVENT_SDK_GET` 保持当前状态并按已有结果处理。如果某条路径让必要事件都不再到来，设备可能长期停在 FAILED——改 FSM 时要逐条确认进入失败态后的恢复路径。

### 定时器 context 的生命周期依赖

消息请求 timer 和 SDK 请求 timeout timer 都把 `DslmDeviceInfo *` 当作 context，直接调度状态机，依赖「设备对象服务退出前不释放」这一约定（`dslm_fsm_process.c` 注释明确写出）。**任何给单个设备对象增加释放逻辑的改动，都会把定时器 context 变成野指针。**

消息请求 timer 使用 `DslmDeviceInfo::timeHandle` 保存句柄，由 `StartSendDeviceInfoRequestTimer` 创建；成功、会话打开失败、下线、重发前等路径会调用 `StopSendDeviceInfoRequestTimer`，超时回调则自然结束。SDK 请求 timeout timer 在 `ProcessSdkRequest` 中按 `notify->keep` 创建，但没有为每个 notify 节点保存独立句柄；其超时回调通过设备对象重新投递 `EVENT_SDK_TIMEOUT`。SA unload timer 在 `OnStart` 中调度，`SetSystemAbilityUnloadSchedule` 会停止并替换已有句柄；`OnStop` 当前只调用 `UnInitService` 和 `ProcessUnloadPlugin`，没有显式停止 `unloadTimerHandle_`。新增或修改相关逻辑时，必须分别核对这三类 timer 的启动、停止和自然到期路径。

### 重入事件的 para 生命周期

`ScheduleMachine` 重入时事件进 `pendingEventList`，**只存裸指针**。如果 para 指向栈上对象且发生重入，出队时该对象可能已经失效，导致读取悬空数据。事件 `para` 必须指向在事件处理完成前始终有效的对象；不能使用可能在事件出队前就失效的栈上变量。

---

## 资源与内存配对

C 代码手工管理内存，统一走 `utils_mem.h` 的 `MALLOC`/`FREE`（便于单测 mock 内存失败注入）。新增分配时立刻搜索对应释放点，并确认**异常分支也走到**：

| 资源 | 分配 | 释放 | 易漏点 |
|---|---|---|---|
| `MessageBuff` / `MessagePacket` | `BuildDeviceSecInfo*` / `ParseMessage` | `FreeMessageBuff` / `FreeMessagePacket` | 解析失败的中途分支 |
| `DslmCredBuff` | `CreateDslmCred` | `DestroyDslmCred`（内含 `credVal`） | `VerifyDeviceInfoResponse` 的每个 `break` 前都要先销毁 |
| `DslmNotifyListNode` | `ProcessSdkRequest` 的 `MALLOC` | 回调后从 notifyList 移入 historyList，超 30 条或设备销毁时 `FREE` | cookie 为 0 / callback 为空的提前返回分支（已释放，保持这个顺序） |
| 定时器 | `DslmUtilsStartOnceTimerTask` | 显式停止或一次性回调自然结束 | 分别核对消息请求、SDK 请求和 SA 卸载定时器的启动、停止及到期路径，不能假定三者共享同一套停止逻辑（见本文「定时器 context 的生命周期依赖」） |
| 插件句柄 | `dlopen(PLUGIN_SO_PATH)` | `ProcessUnloadPlugin` 的 `dlclose` | 服务 `OnStop` 路径 |

---

## 兼容性断裂

| 断裂类型 | 触发 | 后果 |
|---|---|---|
| 错误码 | 修改 `device_security_defines.h` 已有错误码的值或含义 | 调用方分支判断与 HiSysEvent 看板失效 |
| IPC ABI | 修改 `dslm_service_ipc_interface_code.h` 已有枚举值 | 新旧部件静默错分发 |
| 消息字段 | 改 `FIELD_*` 字段名或编码方式只改一端 | 跨设备静默解析失败 |
| 导出符号 | 新增 API 但未登记 `dslm_sdk.versionscript` | 编译正常，运行期符号不可见 |
| 协议版本 | 改 `VERSION_MAJOR/MINOR/PATCH` | 跨版本设备间行为分叉，打点里的 `LOCAL_VERSION`/`TARGET_VERSION` 失去可比性 |

**规则：已有错误码、IPC code 和既有字段/符号含义不能修改或重排；新增兼容内容应采用追加方式。协议版本与消息格式变更还必须评估兼容策略，不能一概套用“只能追加”。**

---

## 根因模式分类

定位到问题后先归类，再选修复策略（见 [`fix-and-verify.md`](fix-and-verify.md)）。

### 设计类

状态在错误的层被表达。典型：某模块自己缓存了一份本应由设备表/FSM 提供的凭据状态，缓存与主状态不同步。
**识别**：同一事实在两处存储。**修复方向**：删副本，每次查主状态。

### 实现类

单点逻辑写错：条件写反、长度算错、hex/base64 编码方向搞反、忘判空。
**识别**：能指着一行说「这里应该是 X 不是 Y」。**修复方向**：就地改，补一条覆盖该分支的用例。

### 并发类

messenger 队列、IPC 线程、SA 卸载定时器线程会碰同一批状态。表现为偶现、加日志就不复现。
**修复方向**：优先不共享（参数传递），其次入队串行化到 messenger 队列，最后才加锁——加锁前确认不是递归路径上的同一把锁。

### 状态流转类

FSM 漏边、某条边漏副作用（漏停定时器、漏回调挂起请求、漏刷新时间戳）。
**识别**：特定事件序列才复现。**修复方向**：补齐该边的全部副作用，并检查同一状态下其它事件的边是否也存在同类遗漏。

### 依赖类

HUKS/device_auth 未就绪、凭据文件缺失、插件加载失败、宏未配置、SoftBus 未拉起。
**识别**：同代码换产品/换形态就不对；或服务重启后时好时坏。**修复方向**：先确认配置与依赖就绪顺序，再考虑改代码。

### 门控类：判据依赖的字段从未被填上

实现「级别达到 X 才允许 Y」类需求时，只在消费点加 `if (credLevel >= X)` 是不够的，必须确认：

1. 信任判断不会仅依赖 `credLevel` 的非零值，而是结合校验返回值和状态（见 [`code-map.md`](code-map.md)「凭据类型与安全级别」的安全约束）；
2. 当前实现并非所有失败分支都会清理 `credLevel`；若新需求要求失败后统一保持 0，必须逐条补充并验证；
3. 默认 SL1 的赋值点（`ProcessDeviceOnline`）是否符合新需求的预期——它是「未验证但给 1」，不是「验证为 1」。

---

## 快速分诊表

| 现象 | 常见根因 | 先看 |
|---|---|---|
| 对端级别查出来是 0 | 凭据链路任一环失败 | 本文「凭据相关失效」+ dump 的 `DEVICE_VERIFIED_RESULT` |
| 某场景下完全无反应无报错 | FSM 无匹配边，事件被丢弃 | 本文「FSM 状态卡死与静默丢事件」 |
| 请求立即返回 `ERR_MSG_NOT_INIT` | SoftBus 未就绪 | 本文「通信层失效」 |
| 挂起请求集中失败回调 | 会话打开失败特判 | 本文「通信层失效」 |
| 等级检查失败且级别变为 0 | 等级超上限时返回 `ERR_CHECK_CRED_INFO` 并将 `credLevel` 清零 | 本文「凭据相关失效」的 `CheckCredInfo` 行 |
| 所有设备都校验失败 | 凭据函数表未注册/插件加载失败 | 本文「凭据相关失效」的函数表行 |
| 第二台设备才开始异常 | 设备表/notify 列表上限或泄漏 | [`code-map.md`](code-map.md)「每设备 FSM 概览」的上限表、本文「资源与内存配对」 |
| 换个形态（small/mini）就不对 | 形态宏分支差异 | [`code-map.md`](code-map.md)「三种系统形态」、[`high-risk.md`](high-risk.md)「高危入口」的 nonce 行 |
| 调用方编译正常但运行期找不到符号 | version script 未登记 | 本文「兼容性断裂」 |
