# 代码分布

← 返回 [`AGENTS.md`](../../AGENTS.md)

---

## 顶层目录职责

```text
device_security_level/
├── AGENTS.md            知识库入口
├── docs/agents/         知识库分层文档
├── bundle.json          部件声明：part/subsystem、features、构建分组、依赖
├── common/
│   ├── dslm.gni         兄弟部件路径变量（dslm_hilog_path 等 6 个）
│   ├── BUILD.gn         共享 config：common_configs（覆盖率）/ common_lite_configs（形态宏）
│   └── include/         IPC 接口码枚举 + IDeviceSecurityLevel 接口（C++）
├── interfaces/inner_api/  对外 native SDK（dslm_sdk），standard / lite(small/mini) 三套实现
├── oem_property/        OEM 适配层：设备安全级别凭据的生成与校验
│   ├── common/          厂商中立逻辑（凭据函数表、凭据对象、工具）
│   └── ohos/            OpenHarmony 实现：common/（HUKS、device_auth 适配）+ standard/ + lite/
├── baselib/
│   ├── msglib/          信使层：封装 DSoftBus 会话/ socket 与设备状态管理
│   └── utils/           工具库 utils_static：base64、hexstring、json、tlv、链表、互斥锁、定时器、状态机、工作队列
├── services/
│   ├── dslm/            核心：每设备 FSM、设备列表、请求处理、消息序列化工具
│   ├── common/          服务公共：随机数（dslm_crypto）、一级消息解析（dslm_msg_serialize）
│   ├── msg/             信使包装：dslm_messenger_wrapper.c（core 与 msglib 之间的桥）
│   ├── sa/              服务入口层：standard（C++ SA + IPC）/ lite small（独立进程）/ lite mini（静态库）
│   ├── dfx/             大数据打点、hidumper、hitrace
│   └── include/         服务内部头文件
├── profile/             SA 3511 的 JSON/CFG 输入及仓内其它 profile 文件
├── test/
│   ├── dslm_unit_test/  GTest 单测（仅 standard），手写 mock + cmocker
│   └── dslm_fuzz_test/  fuzz 测试 dslm_fuzzer + 语料 corpus
└── hisysevent.yaml      DSLM 域的 HiSysEvent 定义
```

---

## 三种系统形态

本仓同一套核心代码编译进三种运行形态，由 `common/BUILD.gn` 的 `common_lite_configs` 定义形态宏区分：

| 形态 | 宏 | 服务形态 | SDK 目标 | 服务入口 |
|---|---|---|---|---|
| standard | `L2_STANDARD` | SA **3511**，`libdslm_service.z.so`，由 `sa_main` 按 `profile/dslm_service.json` 拉起（run-on-create）；启动脚本使用 `profile/dslm_service.cfg` | `libdslm_sdk.z.so`（C++ proxy + IPC） | `services/sa/standard/dslm_service.cpp` |
| small（liteos-a / linux 内核） | `L1_SMALL` | 独立可执行 `dslm_server`，lite IPC | `dslm_sdk_small`（`dslm_level_proxy` 走 lite IPC） | `services/sa/lite/small/dslm_service_main.c` |
| mini（liteos-m 内核） | `L0_MINI` | 静态库 `dslm_server`，链接进系统镜像 | `dslm_sdk_mini`（`device_security_level_inner.c` 直接调服务函数） | `services/sa/lite/mini/dslm_service_feature.c` |

形态差异的代码组织方式是**目录分裂**（`interfaces/inner_api/src/standard`、`interfaces/inner_api/src/lite/small`、`interfaces/inner_api/src/lite/mini`，以及对应的服务目录）加少量 `#ifdef L0_MINI / L1_SMALL / L2_STANDARD`。新增差异代码时应沿用这一模式。

> **形态差异会改变行为，不只是打包方式。** 例：mini 形态下 `SendDeviceInfoRequest()` 里的 `SendMsgToDevice` 被 `#ifndef L0_MINI` 整个去掉（`services/dslm/dslm_inner_process.c`，注释「mini devices are not support yet」），即 mini 设备不主动发起跨设备请求；随机数在非 standard 形态是固定值（见 [`high-risk.md`](high-risk.md)「高危入口」）。

---

## 必读的词汇表文件

改任何逻辑前先扫这些头文件，本仓的全部状态语义都在这里：

| 文件 | 内容 |
|---|---|
| `interfaces/inner_api/include/device_security_defines.h` | `DeviceIdentify`（64 字节设备标识）、`RequestOption`（challenge/timeout/extra）、**全部错误码**（`SUCCESS=0` 到 `ERR_DEFAULT=0xFFFF`，约 50 个） |
| `oem_property/include/dslm_cred.h` | `CredType`（`CRED_TYPE_MINI=1000` / `SMALL=2000` / `STANDARD=3000` / `LARGE=4000`）、`DslmCredInfo`（凭据解析结果，含 `credLevel`）、`RequestObject`、`DslmCredBuff`、`ProcessDslmCredFunctions`（凭据函数表） |
| `services/dslm/dslm_core_defines.h` | `DslmDeviceInfo`（每设备核心对象：FSM、nonce、凭据、notify/history 两个链表）、协议版本 `3.0.0`（`GetCurrentVersion()`） |
| `services/dslm/dslm_fsm_process.h` | **4 个状态**（`STATE_INIT` / `STATE_WAITING_CRED_RSP` / `STATE_SUCCESS` / `STATE_FAILED`）与 **9 个事件**（`EVENT_DEVICE_ONLINE` / `EVENT_CRED_RSP` / `EVENT_MSG_SEND_FAILED` / `EVENT_DEVICE_OFFLINE` / `EVENT_TIME_OUT` / `EVENT_SDK_GET` / `EVENT_SDK_TIMEOUT` / `EVENT_CHECK` / `EVENT_TO_SYNC`） |
| `services/include/dslm_msg_serialize.h` | 一级消息格式：`MSG_TYPE_DSLM_CRED_REQUEST=0x1` / `MSG_TYPE_DSLM_CRED_RESPONSE=0x2`，JSON 字段名 `message` / `payload`，`MSG_BUFF_MAX_LENGTH = 81920*4` |
| `common/include/dslm_service_ipc_interface_code.h` | **IPC ABI**：`DeviceSecurityLevelInterfaceCode` 与 `DeviceSecurityLevelCallbackInterfaceCode` 两个枚举，只能末尾追加 |
| `common/include/idevice_security_level.h` | SA 接口定义：SA ID **3511**，`RequestDeviceSecurityLevel` 与回调 `ResponseDeviceSecurityLevel` |

### 凭据类型与安全级别

| | `CredType` | `credLevel`（安全级别 SL） |
|---|---|---|
| 语义 | 对端设备的**系统形态**（mini/small/standard/large） | 对端设备的**安全级别**（1–5，SL1–SL5） |
| 取值 | 1000 / 2000 / 3000 / 4000 | 0 = 未获取/校验失败；1–5 = SL1–SL5 |
| 上限校验 | 决定走哪条校验路径 | small/mini 凭据上限 2（`CRED_MAX_LEVEL_TYPE_SMALL`），standard 上限 5（`CRED_MAX_LEVEL_TYPE_STANDARD`），见 `oem_property/ohos/common/dslm_ohos_verify.c` 的 `CheckCredInfo()` |
| 来源 | 验证入口会先保存响应中的类型；使用时必须结合验证结果判断是否可信 | 在线事件或凭据验证流程都可能写入；使用时必须结合来源和验证结果判断是否可信 |

> **安全约束：字段赋值不等于验证通过。** 当前实现会在验证入口先保存 `credType`，上线事件也可能写入 `credLevel`；任何信任判断都必须以凭据校验返回成功及对应状态为依据，不能仅凭字段非零就把未验证对端当作已验证设备。

---

## 对外接口分层

对外只暴露 4 个 C API（`interfaces/inner_api/include/device_security_info.h`）：`RequestDeviceSecurityInfo`（同步）、`RequestDeviceSecurityInfoAsync`、`FreeDeviceSecurityInfo`、`GetDeviceSecurityLevelValue`。这是 **inner API，不开放给应用**；消费方以 `external_deps += [ "device_security_level:dslm_sdk" ]` 方式链接。

standard 形态的调用层次（自顶向下）：

| 层 | 文件 | 职责 |
|---|---|---|
| API 实现 | `src/standard/device_security_info.cpp` | 参数校验、默认 option（timeout 45s，上限 300s）、同步接口用 `OncePromise` 等异步结果（最长 50s）、**失败自动重试一次**（间隔 100ms） |
| SA 加载 | `device_security_level_loader.cpp` | `CheckSystemAbility` → `LoadSystemAbility`（SA 3511），加载超时 10s |
| IPC proxy | `device_security_level_proxy.cpp` | 序列化请求，发送 `CMD_GET_DEVICE_SECURITY_LEVEL` |
| 回调管理 | `device_security_level_callback_helper.cpp` + `device_security_level_callback_stub.cpp` | 为每次请求生成 cookie、注册回调 stub，应答回来后按 cookie 分发 |

small / mini 形态没有独立 SA 进程可连：small 走 lite IPC proxy（`src/lite/small/device_security_level_proxy.c`），mini 直接链接服务实现（`src/lite/mini/device_security_level_inner.c`）。

---

## `services/` 模块职责表

### 核心状态机（`services/dslm/`）

| 文件 | 职责 |
|---|---|
| `dslm_core_process.c` | **全部外部事件的入口**：`OnRequestDeviceSecLevelInfo`（本机 SDK 请求）、`OnPeerMsgRequestInfoReceived`（对端查询本机）、`OnPeerMsgResponseInfoReceived`（对端应答）、`OnPeerStatusReceiver`（设备上下线）、`OnMsgSendResultNotifier`（发送结果）、`InitSelfDeviceSecureLevel`（本机自评初始化） |
| `dslm_fsm_process.c` | 每设备 FSM：状态迁移表 `stateNodes[]`（18 条边）、notify 列表回调分发、重发控制 |
| `dslm_inner_process.c` | 请求-应答的协议动作：挑战生成（`CheckAndGenerateChallenge`）、请求发送（`SendDeviceInfoRequest`）、应答校验（`VerifyDeviceInfoResponse`：nonce 相等 + nonce 未过期 + 凭据校验） |
| `dslm_device_list.c` | 全局设备表（静态链表 + 互斥锁）；`MAX_DEVICE_CNT=128` 是创建检查阈值，当前条件为 `size > MAX_DEVICE_CNT`，不是严格的 128 项上限；`CreatOrGetDslmDeviceInfo` 是唯一的设备对象创建点 |
| `dslm_msg_utils.c` | 二级消息（payload）的构造与解析：request 带 challenge + 支持的 credType 列表，response 带 challenge + version + base64 凭据 |
| `dslm_hievent.c` / `dslm_dfx_default.c` | HiSysEvent 上报的 C 层封装与默认实现 |

### 消息与通信

| 文件 | 职责 |
|---|---|
| `services/common/dslm_msg_serialize.c` | **一级消息解析**：`CheckMessage`（检查 ASCII 范围、以 `\0` 结尾且不超过长度上限；不检查是否为可打印字符）→ JSON 解析出 `message`/`payload` |
| `services/common/dslm_crypto.c` | `GenerateRandom`：standard 用 OpenSSL `RAND_bytes`，**其它形态返回固定值** |
| `services/msg/dslm_messenger_wrapper.c` | core 到 msglib 的唯一桥梁，全局 `g_messenger` + 互斥锁；包名 `ohos.dslm`、会话名 `device.security.level`（weak 符号，可被覆盖） |

### 服务入口（`services/sa/`）

| 形态 | 文件 | 职责 |
|---|---|---|
| 公共 | `common/dslm_rpc_process.c` | `InitService`（初始化 messenger + 循环等待 SoftBus 就绪后 `InitDslmProcess`）、`OnPeerMsgReceived` 按消息类型分发 |
| standard | `standard/dslm_service.cpp` | SA 生命周期、`OnRemoteRequest` 分发、**空闲 10s 自动卸载 SA**（`UNLOAD_TIMEOUT`，仅当所有设备 osType 为本类型） |
| standard | `standard/dslm_ipc_process.cpp` | IPC parcel 反序列化与校验（长度、timeout 范围 1–60 否则取 45、cookie 非 0）、`RemoteHolder` 按 `(pid<<32)\|cookie` 暂存回调对象 |
| small | `lite/small/dslm_service_main.c` 等 | 独立进程入口，lite IPC |
| mini | `lite/mini/dslm_service_feature.c` 等 | samgr feature 方式注册 |

### DFX（`services/dfx/`）

`dslm_bigdata.cpp`（HiSysEvent 上报实现）、`dslm_hidumper.c`（dump 设备表与凭据状态）、`dslm_hitrace.cpp`（hitrace 链跟踪）。详见 [`observability.md`](observability.md)。

---

## `oem_property/` 模块职责表

凭据（credential）是本仓的安全根基：设备断言自己安全级别的凭证，由设备厂商签发。

| 目录 | 文件 | 职责 |
|---|---|---|
| `common/` | `dslm_credential.c` | **凭据函数表**：`InitDslmCredentialFunctions` 注册，`DefaultInitDslmCred` / `DefaultRequestDslmCred` / `DefaultVerifyDslmCred` 分发；`CreateDslmCred`（凭据长度上限 `MAX_CRED_LEN=81920`） |
| `common/` | `dslm_credential_utils.c` | 凭据内容解析与校验工具（`VerifyDslmCredential` 等） |
| `ohos/common/` | `dslm_ohos_verify.c` | OpenHarmony 校验实现 `VerifyOhosDslmCred`：standard 走证书链校验（`ValidateCertChainAdapter` → nonce 校验 → 凭据校验 → `CheckCredInfo`）；small/mini 走 `VerifyLiteDslmCred` |
| `ohos/common/` | `dslm_ohos_request.c` | 本机凭据获取 `RequestOhosDslmCred` |
| `ohos/common/` | `external_interface_adapter.c`、`hks_adapter.c` | device_auth / HUKS 适配 |
| `ohos/standard/impl/`、`ohos/lite/impl/` | `dslm_ohos_init.c` | 本机凭据初始化 `InitOhosDslmCred` |
| `ohos/standard/`、`ohos/lite/` | `dslm_finger.cfg`、`dslm_finger_sl2..sl5.cfg` | **指纹配置文件**：按 `device_security_level_feature_cred_level`（默认 1）选一个安装为 `dslm_finger.cfg` |

**函数表注册方式**：`ohos/standard/dslm_ohos_credential.c` 用 `__attribute__((constructor))` 在库加载时注册；standard 形态若配置了 `device_security_level_feature_plugin_path`，则不编 `dslm_oem_ext`，改为 `dslm_service` 启动时 `dlopen(PLUGIN_SO_PATH)` 加载厂商插件（`services/sa/standard/dslm_service.cpp` 的 `ProcessLoadPlugin`）。**校验函数表为空时 `DefaultVerifyDslmCred` 返回 -1，所有对端校验都会失败**——排查「全部设备校验失败」先看插件是否加载成功。

---

## 每设备 FSM 概览

每个对端设备一个 `DslmDeviceInfo`，内嵌一台状态机。迁移表在 `dslm_fsm_process.c` 的 `ScheduleDslmStateMachine()`：

```text
STATE_INIT ──EVENT_DEVICE_ONLINE──▶ STATE_WAITING_CRED_RSP
STATE_WAITING_CRED_RSP ──EVENT_CRED_RSP 校验通过──▶ STATE_SUCCESS
STATE_WAITING_CRED_RSP ──EVENT_TIME_OUT / EVENT_MSG_SEND_FAILED──▶ 重试或失败
STATE_WAITING_CRED_RSP ──EVENT_TO_SYNC──▶ STATE_SUCCESS
STATE_FAILED ──EVENT_DEVICE_ONLINE / EVENT_CHECK──▶ 按条件重新发起请求
STATE_WAITING_CRED_RSP / STATE_SUCCESS / STATE_FAILED ──EVENT_DEVICE_OFFLINE──▶ STATE_INIT
```

以上为主要路径，完整的成功/失败分支以 `stateNodes[]` 为准。

关键数值（定义在 `services/dslm/BUILD.gn` 的 `defines` 与源码中）：

| 参数 | 值 | 含义 |
|---|---|---|
| `MAX_SEND_TIMES` | 5 | 非强制发送的计数阈值；判断条件为 `queryTimes > MAX_SEND_TIMES`，从 0 开始时最多发送 6 次（首次加 5 次重试） |
| `SEND_MSG_TIMEOUT_LEN` | 40000ms | 单次请求-应答超时（**测试目标为 500ms**，行为差异见 [`build-and-test.md`](build-and-test.md)「测试套的选择」） |
| `NONCE_ALIVE_TIME` | 60000ms | nonce 有效期（`dslm_inner_process.c`） |
| `REQUEST_INTERVAL` | 24h | 代码中定义的重复请求间隔；但当前 `CheckNeedToResend()` 在 `credLevel == 0` 时直接返回 `true`、大于 0 时直接返回 `false`，后面的时间判断不可达，因此 24 小时目前未参与决策 |
| `MAX_NOTIFY_SIZE` | 64 | 单设备挂起的 SDK 请求上限，超过返回 `ERR_SA_BUSY` |
| `MAX_HISTORY_CNT` | 30 | 单设备历史回调记录上限 |
| `MAX_DEVICE_CNT` | 128 | 全局设备表创建检查阈值；当前代码使用 `size > MAX_DEVICE_CNT`，因此不是严格的 128 项上限 |

> SDK 查询先刷新目标设备在线状态，再把请求交给对应设备的 FSM（`EVENT_SDK_GET`），不会在查询入口直接调用网络发送函数。在线状态刷新可能触发状态事件；后续是否发送由 FSM、设备现有状态及超时重试路径决定。若设备已有结果（`credLevel != 0`，或状态为 `STATE_SUCCESS`/`STATE_FAILED`），请求会立即回调；否则等待应答或独立的 SDK 超时。内部 `keep` 以毫秒计，由以秒计的 `option.timeout` 换算得到。

---

## 关键文件体量（定位时的取舍参考）

> 下表数字为**快照值，仅用于判断相对量级**，不是当前精确行数。
> 统计口径：`wc -l`，含版权头与空行。

| 文件 | 量级 | 说明 |
|---|---|---|
| `baselib/msglib/src/standard/messenger_device_socket_manager.c` | ~770 | DSoftBus socket 管理，msglib 最大文件 |
| `oem_property/common/dslm_credential_utils.c` | ~580 | 凭据解析校验工具 |
| `baselib/msglib/src/lite/messenger_device_session_manager.c` | ~490 | lite 会话管理 |
| `services/dslm/dslm_fsm_process.c` | ~470 | **FSM，改动前通读迁移表** |
| `baselib/msglib/src/standard/messenger_device_status_manager.cpp` | ~430 | 设备状态管理（device_manager 交互） |
| `services/dslm/dslm_core_process.c` | ~300 | 事件入口 |
| `oem_property/ohos/common/dslm_ohos_verify.c` | ~300 | 凭据校验主流程 |
| `oem_property/ohos/common/external_interface_adapter.c` | ~300 | device_auth 适配 |

本仓文件都不大，可以整读；FSM 与凭据校验两个文件建议完整读完再动手。
