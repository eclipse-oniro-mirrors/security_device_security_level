# 边界知识：模块依赖规则、协议约束、状态流转约束

← 返回 [`AGENTS.md`](../../AGENTS.md)

> 这是**动手改代码前必读**的文档。本仓最常见的返工原因是越过了这里的某一条边界。

---

## 分层依赖规则

允许的依赖方向是**单向向下**：

```text
interfaces/inner_api (SDK) ──IPC──▶ services/sa ──▶ services/dslm (core) ──▶ services/msg ──▶ baselib/msglib ──▶ DSoftBus
                                          │                │
                                          │                ▼
                                          │         services/common（消息解析/随机数）
                                          ▼
                                    oem_property（凭据，通过函数表被动注册，见下）
```

| 规则 | 说明 |
|---|---|
| `baselib/` 不依赖 `services/` 与 `oem_property/` | 它是纯工具与传输层，被所有层引用。 |
| `baselib/msglib/` 不解析消息内容 | 只做会话/ socket 管理与字节流传输。一级消息解析（JSON、`message`/`payload` 拆包）在 `services/common/dslm_msg_serialize.c`，二级 payload 解析在 `services/dslm/dslm_msg_utils.c`。在 msglib 里加消息格式逻辑是错的。 |
| `services/dslm/`（core）不认识 IPC | core 只面对 C 结构：`DeviceIdentify`、`RequestOption`、`RequestCallback`。所有 parcel 序列化/反序列化都在 `services/sa/`。**把 IPC 类型带进 core 是错的。** |
| `services/sa/` 不做业务 | standard 的 `dslm_service.cpp` 只做 SA 生命周期与 code 分发；`dslm_ipc_process.cpp` 只做 parcel 校验与回调对象暂存；业务入口是 `dslm_core_process.c` 的 `OnRequestDeviceSecLevelInfo`。 |
| `oem_property/` 是**被动注册**，不是被链接调用 | core 通过 `DefaultInitDslmCred` / `DefaultRequestDslmCred` / `DefaultVerifyDslmCred`（`oem_property/common/dslm_credential.c`）查函数表再分发。厂商实现经 `InitDslmCredentialFunctions` 注册（OpenHarmony 实现用 `__attribute__((constructor))`，见 `oem_property/ohos/standard/dslm_ohos_credential.c`）。**函数表为空时三个 `Default*` 返回 -1**，表现为「所有凭据操作失败」，不是崩溃。 |

**违反示例**：在 `dslm_core_process.c` 里直接 include HUKS 头文件做凭据校验。正确做法是走 `DefaultVerifyDslmCred`，让 `oem_property` 的实现去完成。

---

## 线程与队列模型

本仓不使用 ffrt，并发控制主要依赖以下三个机制：

| 机制 | 位置 | 约束 |
|---|---|---|
| **单一工作队列** `messenger_queue`（容量 256） | `baselib/msglib/src/standard/messenger_impl.c` | 对端消息回调、发送结果通知都投递到这个队列串行执行。**任一处理阻塞，所有设备的消息处理停摆。** core 的事件入口（`OnPeerMsg*`）都跑在这个队列上，禁止在里面做长耗时同步操作。 |
| **FSM 递归锁 + 待处理事件队列** | `baselib/utils/src/utils_state_machine.c` | `ScheduleMachine` 持锁执行；处理中重入的事件进 `pendingEventList`，当前事件结束后**递归**调度。**入队事件只保存裸指针 `para`，其指向的内容必须活到出队处理时**——栈上临时对象做 para 且触发重入，是本仓最容易写出的野指针。 |
| **全局互斥锁** | `dslm_device_list.c`（设备表）、`dslm_messenger_wrapper.c`（`g_messenger`） | 静态 `Mutex`，持锁期间不得调用会再次取同一把锁的函数。 |

SDK 回调路径：`ProcessSendDeviceInfoCallback`（core，messenger 队列线程）→ `ProcessCallback`（`dslm_ipc_process.cpp`）→ `DslmCallbackProxy::ResponseDeviceSecurityLevel` → 异步 IPC → 调用方进程的 callback stub。**回调准备和发送仍发生在触发它的线程上；异步 IPC 不会等待调用方业务执行，但触发线程不应在发送前加入长耗时操作。**

---

## 跨设备请求-响应协议约束

### 两级消息格式

```text
一级（dslm_msg_serialize.c）：{ "message": <type>, "payload": { ... } }
  type: 0x1 = MSG_TYPE_DSLM_CRED_REQUEST / 0x2 = MSG_TYPE_DSLM_CRED_RESPONSE
二级 request（dslm_msg_utils.c）：  { version, challenge(hex 字符串), support: [credType...] }
二级 response（dslm_msg_utils.c）： { version, credType, challenge(hex 字符串), credInfo(base64) }
```

线上字节流是以 `\0` 结尾的 JSON 字符串。入口 `CheckMessage()` 检查消息长度、结尾的 `\0` 和 ASCII 范围（0–127），但不检查字符是否可打印；长度上限为 `MSG_BUFF_MAX_LENGTH`（`81920*4`）。

**写读对称约束**：`BuildDeviceSecInfoRequest`/`ParseDeviceSecInfoRequest`、`BuildDeviceSecInfoResponse`/`ParseDeviceSecInfoResponse` 必须成对修改。字段名（`FIELD_VERSION` 等）改一端不改另一端，**不会产生任何报错**，只会静默解析失败返回 `ERR_NO_CHALLENGE` / `ERR_NO_CRED`。改消息格式时同步更新 `test/dslm_fuzz_test/dslm_fuzzer/corpus` 的语料。

### challenge（nonce）语义

- 8 字节随机数 hex 编码为字符串放入 JSON；应答方原样带回，请求方校验**相等且非 0**（`VerifyDeviceInfoResponse`）。
- nonce 有效期 60s（`NONCE_ALIVE_TIME`），**过期检查只在 `L2_STANDARD` 宏内**——small/mini 形态不检查过期，只查相等。改 nonce 逻辑时注意三种形态行为不同。
- 随机数质量见 [`high-risk.md`](high-risk.md)「高危入口」：非 standard 形态 `GenerateRandom` 返回固定值，不能据此假设 nonce 具备随机挑战所需的防重放能力。

### 版本协商

`GetCurrentVersion()` = `3.0.0`（`dslm_core_defines.h`，`major<<16 | minor<<8 | patch`）。应答里的 version 记入 `device->version` 并上报 HiSysEvent（`LOCAL_VERSION` / `TARGET_VERSION`）。**升级版本号意味着协议或凭据格式变化**，属于 [`AGENTS.md`](../../AGENTS.md)「必须停下来找人确认的改动」。

---

## FSM 状态流转约束

迁移表是 `dslm_fsm_process.c` `ScheduleDslmStateMachine()` 里的 `stateNodes[]`（18 条边）。每条边 = `{当前态, 事件, 处理函数, 处理成功去向 nextStateT, 处理失败去向 nextStateF}`。

| 约束 | 说明 |
|---|---|
| 状态迁移只走 `ScheduleDslmStateMachine()` | 直接写 `info->machine.currState` 的唯一现存例外是 `InitSelfDeviceSecureLevel()`（服务初始化、尚无并发事件时）。新代码不要新增直接写。 |
| **没有匹配的边 = 事件被静默丢弃** | `GetScheduleStateNode` 找不到 `{state, event}` 组合时不做任何事（只打一条日志）。新增事件或新增状态后，**必须为每个可能到达的状态补边**，否则表现为「某场景下完全没反应」。 |
| 处理函数返回值决定去留 | 返回 `false` 走 `nextStateF`（多为 `STATE_FAILED`）。处理函数里「部分成功」的语义要自己想清楚返回值。 |
| 事件参数类型约定 | `EVENT_DEVICE_ONLINE` 的 para 是 `uint32`（低 8 位 level、次 8 位 osType）；`EVENT_SDK_GET` 是 `DslmNotifyListNode *`；`EVENT_CRED_RSP` 是 `MessageBuff *`；`EVENT_MSG_SEND_FAILED` 是 `uint32` 原因码。传错类型不报错，读出垃圾。 |
| 定时器事件用设备对象做 context | `TimerProcessSendDeviceInfoRequestTimeOut` 依赖「`DslmDeviceInfo` 永不单独释放」（注释明确写出）。设备对象只在 `DestroyAllDslmDeviceInfo()`（服务退出）统一销毁——**不要给单个设备加销毁逻辑**，会把定时器 context 变成野指针。 |

---

## 凭据生命周期约束

| 约束 | 落点 |
|---|---|
| `credType` 的字段赋值与可信语义必须区分 | `VerifyOhosDslmCred()` 会在进入具体校验分支前保存响应中的 `credType`；只有校验返回成功，并且状态符合预期时，才能把它作为可信结果使用 |
| `credLevel` 的字段赋值与可信语义必须区分 | 设备上线、本机初始化和凭据校验流程都可能写入 `credLevel`；字段被赋值本身不代表凭据校验通过 |
| 等级上限按形态收紧 | `CheckCredInfo()`：small/mini 凭据 `credLevel ≤ 2`，standard `≤ 5`，超限清零并返回 `ERR_CHECK_CRED_INFO` |
| debug 凭据的当前检查规则 | `CheckCredInfo()`：udid 为空时跳过检查；当 `releaseType` 以 `debug` 开头且 udid 非空时，按凭据中的 udid 长度与设备标识比较；release 凭据不执行这项比较 |
| 在线事件中的默认 SL1 回退赋值点 | `ProcessDeviceOnline()`：`level == 0 && osType == DEFAULT_TYPE(10)` 时置 1。这表示「未验证但给 1」，不等于验证结果为 SL1；其它初始化或配置解析路径也可能得到 SL1 |
| `REQUEST_INTERVAL` 的当前实现语义 | `CheckNeedToResend()` 先判断 `credLevel`：为 0 时直接返回 `true`，大于 0 时直接返回 `false`，因此后面的 `REQUEST_INTERVAL` 时间判断当前不可达，24 小时窗口并未实际参与决策 |
| 凭据长度上限 | `CreateDslmCred` 拒绝 `len > MAX_CRED_LEN(81920)` |

---

## 新增 IPC 接口的清单（standard 形态）

现有 IPC 面只有一进一出两个 code。新增接口按顺序检查以下各点；漏项可能在编译或链接阶段失败，也可能直到运行期才暴露：

| # | 文件 | 动作 | 漏掉的后果 |
|---|---|---|---|
| 1 | `common/include/dslm_service_ipc_interface_code.h` | 在**枚举末尾追加**新 code | — |
| 2 | `common/include/idevice_security_level.h` | `IDeviceSecurityLevel` 加纯虚函数 | 具体实现未同步更新时编译失败 |
| 3 | `interfaces/inner_api/src/standard/device_security_level_proxy.{h,cpp}` | 实现 proxy：写 token → 写参数 → `SendRequest` → 读 reply | 客户端调不通 |
| 4 | `services/sa/standard/dslm_service.cpp` | `OnRemoteRequest` 的 `switch` 加 case 并分发 | **运行期落到 default 分支，静默返回错误** |
| 5 | `services/sa/standard/dslm_ipc_process.{h,cpp}` | parcel 反序列化；参照 `DslmGetRequestFromParcel` 检查可读字节数、缓冲区读取/复制、回调对象和 cookie，并按当前实现归一化非法 timeout | 越界读、脏数据 |
| 6 | `interfaces/inner_api/include/device_security_info.h` | 新增对外 C API 声明 | 调用方无法使用 |
| 7 | `interfaces/inner_api/dslm_sdk.versionscript` | `global:` 段加新符号 | 调用方可能在链接阶段或运行期找不到导出符号 |

proxy 写入与 stub 读取的顺序、类型、个数必须严格一致；`MessageParcel` 不做类型校验。

回调方向（服务 → 客户端）涉及另外一套：`IDeviceSecurityLevelCallback` + `device_security_level_callback_stub.cpp` + `dslm_callback_proxy.cpp` + `device_security_level_callback_helper.cpp` 的 cookie 注册/分发，改动时四处同步。

small / mini 形态有独立的 IPC/直连路径（`services/sa/lite/`），接口语义变更时三态都要评估。

---

## 隐式约束速查

| 约束 | 说明 |
|---|---|
| 错误码即对外契约 | `device_security_defines.h` 的错误码经回调返回调用方、并打进 HiSysEvent（`RET_CODE`）。**只能追加，不得复用已有值表达新含义**——调用方与运营看板都按值解读。 |
| `cookie == 0` 非法 | SDK 侧（callback helper）与服务侧（`DslmGetRequestFromParcel`）都拒绝 0；回调分发以 `(pid << 32) \| cookie` 为键（`RemoteHolder`）。 |
| 单设备挂起请求上限 64 | `MAX_NOTIFY_SIZE`，超过返回 `ERR_SA_BUSY`。新增「批量查询」类需求必须先评估这个上限。 |
| `GetSelfDevice()` 结果静态缓存 | `dslm_messenger_wrapper.c` 里 `static DeviceIdentify`，只在首次有效获取后缓存。设备标识运行期变化的场景不支持。 |
| SA 空闲 10s 自动卸载 | `dslm_service.cpp` `UNLOAD_TIMEOUT`；前提是 `JudgeListDeviceType()` 为真（设备表里所有设备 `osType == 10`）。**设备表里存在其它类型设备时 SA 不会卸载**——改 osType 赋值逻辑会改变 SA 生命周期行为。 |
| 会话名是 weak 符号 | `GetMessengerPackageName` / `GetMessengerPrimarySessionName` / `GetMessengerSecondarySessionName` 是 `__attribute__((weak))`，产品可覆盖；次会话名由 `device_security_level_feature_secondary_session_name` 宏注入。排查「连错会话」先确认产品侧是否覆盖了这些符号。 |
| C 与 C++ 交界 | core 是 C，standard SA 层是 C++；跨边界头文件必须带 `extern "C"`（现有头文件均已带，新增头文件保持一致）。 |
| `MALLOC`/`FREE` 配对 | 全仓走 `utils_mem.h` 的宏（便于单测 mock 内存失败）。新增堆分配必须配 `FREE`，且异常分支也要走到，见 [`failure-modes.md`](failure-modes.md)「资源与内存配对」。 |
