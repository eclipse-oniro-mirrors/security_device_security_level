# AGENTS.md — Device Security Level Management 知识库入口

> 本文件是本仓 AI Agent 的**唯一入口**。详细知识位于 `docs/agents/`。
> 使用方式：先读「三十秒定位」判断链路 → 按路由表选择文档 → 修改前核对红线 → 完成前执行最小验证闭环。

> 本知识库描述仓库当前代码状态。文档中的不变量是维护约束；如果当前代码找不到对应落点，先确认是待修缺口还是文档错误，不要凭猜测修改。

---

## 这个仓是什么

本仓是 OpenHarmony **Device Security Level Management（DSLM）** 部件（`@ohos/device_security_level`，`security/device_security_level`），用于在分布式 Super Device 场景中获取并验证设备安全级别（SL1–SL5）。其它部件在传输或处理用户数据前，通过 inner native API 查询目标设备的安全级别。

- standard 形态是 SA **3511**，服务名 `dslm_service`，产物 `libdslm_service.z.so`；
- small 形态是使用 lite IPC 的 `dslm_server`；
- mini 形态是链接进系统的静态 `dslm_server`；
- 对外只提供 inner native C API：`RequestDeviceSecurityInfo`、`RequestDeviceSecurityInfoAsync`、`FreeDeviceSecurityInfo`、`GetDeviceSecurityLevelValue`；
- 安全根基是 OEM credential：本机生成/读取凭据，对端通过 challenge、证书链、HUKS/device_auth 和内容规则验证凭据；
- 通信使用 DSoftBus，经 `baselib/msglib` 和 `services/msg` 封装；
- 核心状态以每设备 `DslmDeviceInfo` 和 FSM 管理。

**一句话心智模型**：本仓是「一条跨设备 challenge-response 管道 + 一份按设备维护的安全级别/FSM 状态表 + 一套 OEM 凭据校验函数」。错误通常出在消息边界、凭据校验边界、状态迁移边界或三种系统形态边界。

详细目录、形态和类型语义见 [`code-map.md`](docs/agents/code-map.md)「三种系统形态」与「每设备 FSM 概览」。

---

## 三十秒定位：两条主链路

### 下行：本地调用方查询目标设备

```text
调用方
  └─ interfaces/inner_api/include/device_security_info.h
       └─ device_security_info.cpp                  C API、同步等待、失败重试
            └─ device_security_level_loader.cpp     Check/Load SA 3511
                 └─ device_security_level_proxy.cpp IPC CMD_GET_DEVICE_SECURITY_LEVEL
                      └─ services/sa/standard/dslm_service.cpp
                           └─ dslm_ipc_process.cpp   parcel 校验、RemoteHolder、cookie
                                └─ dslm_core_process.c::OnRequestDeviceSecLevelInfo
                                     └─ dslm_fsm_process.c::EVENT_SDK_GET
                                          └─ ProcessSdkRequest → notifyList → 结果回调或超时

设备上线 / 检查事件 / 超时重试
  └─ CheckTimesAndSendCredRequest → CheckAndGenerateChallenge
       └─ SendDeviceInfoRequest → services/msg → baselib/msglib → DSoftBus
```

SDK 查询先刷新目标设备的在线状态，再进入对应设备的 `notifyList`。查询入口不会直接调用网络发送函数；在线状态刷新可能触发设备状态事件，之后由 FSM 和设备现有状态决定是否发送 challenge，或立即回调结果。

### 上行：对端应答或设备状态回传

```text
DSoftBus / msglib
  └─ services/sa/common/dslm_rpc_process.c::OnPeerMsgReceived
       └─ ParseMessage                            一级 JSON message/payload
            ├─ request → OnPeerMsgRequestInfoReceived
            │    └─ ParseDeviceSecInfoRequest
            │         └─ DefaultRequestDslmCred → BuildDeviceSecInfoResponse → SendMsgToDevice
            └─ response → OnPeerMsgResponseInfoReceived
                 └─ dslm_fsm_process.c::EVENT_CRED_RSP
                      └─ VerifyDeviceInfoResponse
                           └─ ParseDeviceSecInfoResponse
                                └─ nonce 校验 → DefaultVerifyDslmCred
                                     └─ ProcessSendDeviceInfoCallback
                                          └─ IPC callback proxy → 调用方 callback
```

并行的本机初始化链路是 `InitSelfDeviceSecureLevel` → `DefaultInitDslmCred`；凭据函数由 `oem_property` 注册。具体依赖方向见 [`boundaries.md`](docs/agents/boundaries.md)「分层依赖规则」。

---

## 知识路由表

### 任务触发路由

| 任务 | 先读 | 主要修改位置 |
|---|---|---|
| 修改 SDK/API 或同步/异步行为 | [`code-map.md`](docs/agents/code-map.md)「对外接口分层」 + [`boundaries.md`](docs/agents/boundaries.md)「新增 IPC 接口的清单（standard 形态）」 | `interfaces/inner_api/`、standard IPC 层、version script |
| 修改跨设备消息格式 | [`boundaries.md`](docs/agents/boundaries.md)「跨设备请求-响应协议约束」 + [`failure-modes.md`](docs/agents/failure-modes.md)「外部输入解析失败」 | `services/common/dslm_msg_serialize.c`、`services/dslm/dslm_msg_utils.c`，同步 fuzz 语料 |
| 修改凭据生成/校验/安全级别 | [`high-risk.md`](docs/agents/high-risk.md)「高危入口」 + [`boundaries.md`](docs/agents/boundaries.md)「凭据生命周期约束」 | `oem_property/`、`services/common/dslm_crypto.c` |
| 修改 FSM、超时、重试、回调 | [`boundaries.md`](docs/agents/boundaries.md)「FSM 状态流转约束」 + [`failure-modes.md`](docs/agents/failure-modes.md)「FSM 状态卡死与静默丢事件」 | `services/dslm/dslm_fsm_process.c`、`dslm_core_process.c` |
| 修改 DSoftBus、会话、设备上下线 | [`code-map.md`](docs/agents/code-map.md)「消息与通信」 + [`high-risk.md`](docs/agents/high-risk.md)「高危事件序列」 | `baselib/msglib/`、`services/msg/` |
| 修改 IPC parcel 或接口码 | [`boundaries.md`](docs/agents/boundaries.md)「新增 IPC 接口的清单（standard 形态）」 | `common/include/`、`services/sa/standard/`、proxy/stub |
| 排查线上状态/等级问题 | [`observability.md`](docs/agents/observability.md)「推荐排查顺序」 | dump、hilog、HiSysEvent、hitrace |
| 排查 bug 并补回归 | [`failure-modes.md`](docs/agents/failure-modes.md) + [`fix-and-verify.md`](docs/agents/fix-and-verify.md) | 单测 mock、fuzz 入口和 corpus |
| 修改构建目标/feature/系统形态 | [`build-and-test.md`](docs/agents/build-and-test.md)「新增文件与 feature 的构建清单」 | `BUILD.gn`、`common/dslm.gni`、`bundle.json` |

### 路径触发路由

| 路径 | 必读知识 |
|---|---|
| `oem_property/**` | [`high-risk.md`](docs/agents/high-risk.md)「高危入口」、[`failure-modes.md`](docs/agents/failure-modes.md)「凭据相关失效」 |
| `services/common/dslm_crypto.c` | [`high-risk.md`](docs/agents/high-risk.md)「高危入口」 |
| `services/common/dslm_msg_serialize.c`、`services/dslm/dslm_msg_utils.c` | [`boundaries.md`](docs/agents/boundaries.md)「跨设备请求-响应协议约束」、[`failure-modes.md`](docs/agents/failure-modes.md)「外部输入解析失败」 |
| `services/dslm/dslm_fsm_process.c` | [`boundaries.md`](docs/agents/boundaries.md)「FSM 状态流转约束」、[`failure-modes.md`](docs/agents/failure-modes.md)「FSM 状态卡死与静默丢事件」 |
| `services/sa/standard/**`、`interfaces/inner_api/src/standard/**` | [`boundaries.md`](docs/agents/boundaries.md)「新增 IPC 接口的清单（standard 形态）」 |
| `baselib/msglib/**`、`services/msg/**` | [`boundaries.md`](docs/agents/boundaries.md)「线程与队列模型」、[`high-risk.md`](docs/agents/high-risk.md)「高危事件序列」 |
| `profile/**`、`*.versionscript`、`cfi_blocklist.txt` | [`high-risk.md`](docs/agents/high-risk.md)「profile 与加固配置」 |
| `test/**` | [`fix-and-verify.md`](docs/agents/fix-and-verify.md)「单元测试约定」、[`build-and-test.md`](docs/agents/build-and-test.md)「测试套的选择」 |
| `BUILD.gn`、`common/dslm.gni`、`bundle.json` | [`build-and-test.md`](docs/agents/build-and-test.md) |

### 词汇触发路由

| 词 | 含义 | 落点 |
|---|---|---|
| **SL1–SL5 / credLevel** | 设备安全级别；`0` 表示未取得有效等级，1–5 对应安全级别 | `oem_property/ohos/common/dslm_ohos_verify.c`、[`code-map.md`](docs/agents/code-map.md)「凭据类型与安全级别」 |
| **CredType** | 设备系统形态类型，不等于安全级别：mini 1000、small 2000、standard 3000、large 4000 | `oem_property/include/dslm_cred.h` |
| **challenge / nonce** | 请求方生成、应答方原样返回的 8 字节随机数；校验相等且非 0，standard 还检查 60s 有效期 | `services/dslm/dslm_inner_process.c` |
| **一级消息 / 二级 payload** | 一级是 `message`/`payload` JSON 包装；二级是 credential request/response 字段 | `services/common/dslm_msg_serialize.c`、`services/dslm/dslm_msg_utils.c` |
| **DslmDeviceInfo** | 每个设备一份的核心对象，含 FSM、凭据、时间戳、notify/history 列表 | `services/dslm/dslm_core_defines.h` |
| **notifyList / historyList** | 当前等待回调的请求列表 / 已完成请求历史列表 | `services/dslm/dslm_fsm_process.c` |
| **L0_MINI / L1_SMALL / L2_STANDARD** | mini、small、standard 编译形态宏 | `common/BUILD.gn` |
| **RemoteHolder** | standard IPC 服务端按 `(pid << 32) | cookie` 暂存 callback remote object 的表 | `services/sa/standard/dslm_ipc_process.cpp` |
| **fingerprint cfg** | `dslm_finger.cfg` 及 `dslm_finger_sl2..sl5.cfg`，设备凭据配置；不是普通日志或测试数据 | `oem_property/ohos/standard/` |
| **PLUGIN_SO_PATH** | 非空 `device_security_level_feature_plugin_path` 生成的宏，服务启动时 `dlopen` OEM 凭据插件 | `services/sa/standard/dslm_service.cpp` |

---

## 动手前必须先自陈

写出第一行代码改动前，先用三行说明：

```text
任务类别：<任务触发路由中的哪一行>
已读文档：<docs/agents/ 下实际读过的文件与小节>
命中约束：<本文红线编号 + 分层文档中的不变量；没有则写“无”>
```

无法写出这三行，说明路由尚未完成，不要开始修改。

---

## 红线（禁止事项）

违反以下任一条的改动不可直接合入：

1. **禁止把未验证字段当作验证结果使用。** 当前实现会在凭据验证入口先保存 `credType`，设备上线、本机初始化和凭据验证流程也可能写入 `credLevel`；这些字段赋值本身都不是凭据验证通过的证据。后续信任判断只能基于相应的验证返回值和状态，默认 SL1 的赋值点也不能与验证等级混为一谈。
2. **禁止削弱外部输入校验。** 网络消息、JSON、base64、IPC parcel、设备标识都必须先做长度、空指针、范围和编码校验。
3. **禁止只改消息/IPC 的一端。** `Build`/`Parse`、proxy/stub 的写入/读取必须严格对称。
4. **禁止在 `ScheduleDslmStateMachine()` 外新增直接写 `currState`。** 状态迁移必须经过 `stateNodes[]` 和处理函数，保留副作用、回调和定时器语义。
5. **禁止在 messenger 工作队列中加入长耗时同步操作。** `messenger_queue` 串行处理所有设备消息；阻塞会让全局状态停摆。
6. **禁止修改已有 IPC code、错误码、协议字段或导出符号的既有含义。** 只能末尾追加，并保留旧接口兼容。
7. **禁止记录凭据原文、私钥、完整 udid 或原始网络消息。** 日志/dump 不得记录凭据原文、私钥、完整 udid 或原始网络消息；必要的诊断字段可使用 machineId、cookie、transNum、结果码、长度、状态以及脱敏后的厂商/型号信息。
8. **禁止删减 sanitizer、CFI、PAC、version script 或 `profile/dslm_service.cfg` 的 SELinux seclabel。** 新 target 沿用现有加固配置。
9. **禁止把 standard/small/mini 的差异散落成无边界的 `#ifdef`。** 优先按 `standard/`、`lite/small/`、`lite/mini/` 目录分裂。
10. **禁止新增堆分配而不配对释放。** 所有 `MALLOC`/`FREE`、credential/message 对象、timer、plugin handle 都要覆盖异常路径。
11. **禁止不登记构建输入。** 新源文件、include、外部依赖、feature、fuzzer target 必须同步进入对应 `BUILD.gn`/`bundle.json`。
12. **禁止把测试构建参数当生产参数。** 生产 `SEND_MSG_TIMEOUT_LEN=40000`，测试 source set 是 500；两者影响不同。

---

## 必须停下来找人确认的改动

| 触发条件 | 原因 |
|---|---|
| 修改 `dslm_finger*.cfg` 或 `device_security_level_feature_cred_level` 默认值 | 直接改变设备对外声明的安全级别 |
| 放宽/收紧凭据校验、证书链、nonce 或 debug/release 规则 | 可能造成越权信任或全量设备互不信任 |
| 引入或修改 `PLUGIN_SO_PATH` OEM 插件机制 | 改变安全根基的加载来源 |
| 修改既有 IPC code、错误码、消息字段、版本号或跨部件结构体 | ABI/协议兼容性超出本仓单独验证能力 |
| 修改 `profile` 的 seclabel、SA 依赖、run-on-create 或卸载时序 | 影响启动、安全权限和服务生命周期 |
| 删除或放宽 sanitizer/CFI/PAC/version script | 安全加固和符号契约变化 |
| 新增第三方依赖或改变 device_auth/HUKS/DSoftBus 依赖 | license、供应链和外部部件兼容性审查 |
| 改变三种系统形态的安全语义 | standard/small/mini 行为可能分叉，单测只覆盖 standard |

判据：后果不能在本仓和现有测试中独立验证，就属于本节。

---

## 最小验证闭环（DoD）

### 第 1 步：静态自检

按 [`build-and-test.md`](docs/agents/build-and-test.md)「无完整构建环境时的静态自检」完成 13 项；尤其检查源文件/依赖/feature/fuzzer 是否登记、消息与 IPC 是否对称、凭据与资源是否成对释放、文档链接是否断链。

### 第 2 步：构建

在完整 OpenHarmony 源码根目录执行：

```shell
./build.sh --product-name <product> --build-target device_security_level
./build.sh --product-name <product> --build-target dslm_test
./build.sh --product-name <product> --build-target dslm_fuzz
```

命令面向完整 OHOS 源码树；当前工作目录没有该完整环境，因此本次知识库构建只做静态核对，不声称已实际编译。

### 第 3 步：测试

standard 产品运行相关 `DslmUnitTest`/`DslmUnitTest2`，消息/parcel/凭据改动运行 `DslmFuzzTest`。测试套、产物路径和筛选命令见 [`build-and-test.md`](docs/agents/build-and-test.md)「测试套的选择」。

### 第 4 步：回归判定

按 [`fix-and-verify.md`](docs/agents/fix-and-verify.md)「回归面判定」确认影响场景；按 [`high-risk.md`](docs/agents/high-risk.md)「高危事件序列」确认时序；涉及标志位/资源/状态机时确认置位与清位、申请与释放、进入与退出边成对。

### 第 5 步：最终回复必须说明

1. 改了什么：文件清单和每份文档的职责；
2. 任务类别、已读文档、命中约束三行自陈；
3. 静态自检、构建、测试、回归各项已验证/未验证结果；
4. 构建边界：是否在完整 OpenHarmony 源码树实际执行；
5. 遗留风险、发现但未修改的仓内不一致、需人工复核事项。

---

## 分层文档索引

| 文档 | 内容 | 什么时候读 |
|---|---|---|
| [`docs/agents/code-map.md`](docs/agents/code-map.md) | 目录分层、三种系统形态、接口层次、模块职责、FSM 与文件分布 | 第一次进入某个模块 |
| [`docs/agents/boundaries.md`](docs/agents/boundaries.md) | 分层依赖、线程队列、消息协议、FSM、凭据生命周期、IPC 清单 | **动手改代码前必读** |
| [`docs/agents/high-risk.md`](docs/agents/high-risk.md) | 高危入口、feature/profile/加固配置、高危事件序列、风险矩阵 | 改凭据、消息、IPC、FSM 或构建配置时 |
| [`docs/agents/failure-modes.md`](docs/agents/failure-modes.md) | 消息/凭据/通信/FSM/内存失效模式与快速分诊 | 排查 bug 时 |
| [`docs/agents/fix-and-verify.md`](docs/agents/fix-and-verify.md) | 修复点原则、错误修法、GTest/fuzz 约定与回归面 | 确定根因后、动手前 |
| [`docs/agents/observability.md`](docs/agents/observability.md) | hilog、dump、HiSysEvent、hitrace 与排查顺序 | 定位现场问题时 |
| [`docs/agents/build-and-test.md`](docs/agents/build-and-test.md) | 构建目标、命令、测试套、静态自检和 DoD | 验证阶段 |
