# 修复策略与验证回归

← 返回 [`AGENTS.md`](../../AGENTS.md)

> 定位到根因之后、动手之前读这一篇。正确修复点通常比补一个条件更重要。

---

## 修复点选择原则

### 在决策点修，不在执行点修

| 关注点 | 决策点（正确修复位置） | 执行点（错误修复位置） |
|---|---|---|
| 是否接受一条对端消息 | `CheckMessage` / `ParseDeviceSecInfo*` | 在回调函数里吞掉错误 |
| 是否信任凭据及其等级 | `VerifyOhosDslmCred` → `CheckCredInfo`，并结合返回值和设备状态判断 | 在 `ProcessDeviceOnline` 或 SDK 返回前强行改 `credLevel`，或仅凭字段非零就当作已验证 |
| 何时发送 challenge | `CheckTimesAndSendCredRequest` / FSM | 在 msglib socket 回调里自行重发 |
| 事件进入哪个状态 | `stateNodes[]` + 对应 process 函数 | 直接写 `machine.currState` |
| IPC 参数是否合法 | `DslmGetRequestFromParcel` | 在 core 业务函数里补救非法参数 |

判据：如果修复方式是在某个具体功能函数里加一个 `if`，去补救上游传下来的错误输入，通常说明应回头找决策点。

### 在状态拥有者修，不在消费者修

`credLevel` 由凭据校验、本机初始化和设备在线状态路径共同维护；设备在线 level 是状态机入口，设备对象表由 `dslm_device_list.c` 管理。消费者发现状态不对时，应修对应路径的写入/清理逻辑，并以校验返回值和状态判断可信性，不要在 dump、IPC 回调或 SDK wrapper 中加「看起来不对就改掉」的补丁。

### 补齐对称性

| 新增内容 | 必须同时检查 |
|---|---|
| 一个 `MALLOC` / `Create*` | 所有成功、失败、提前返回路径的 `FREE` / `Destroy*` |
| 一条 FSM 状态边 | 同一状态的其它事件、进入失败态后的恢复边 |
| 一个消息字段 | request/response 的 Build 与 Parse 两端、旧版本兼容策略、fuzz 语料 |
| 一个 IPC code | 枚举、接口、proxy、SA switch、parcel 读写、回调/SDK API、version script |
| 一个 feature 宏 | `declare_args`、`defines`、源文件/include 条件、`bundle.json` feature、宏关闭编译 |
| 一个超时/重试值 | SDK timeout、nonce 60s、消息 40s、SA unload 10s 的相对关系；`REQUEST_INTERVAL` 虽定义为 24h，但当前判断不可达 |

---

## 错误修法反例

| 反例 | 为什么错 | 正确做法 |
|---|---|---|
| 收到消息后不做 `CheckMessage` 就解析 JSON | 网络输入可越界/畸形 | 保留一级校验，再进入 `ParseMessage` |
| 只在 response 端放宽 nonce 校验 | 破坏 challenge 绑定，可能接受迟到/错配应答 | 修正 challenge 生成、传递或两端协议版本 |
| 直接给 `credLevel` 赋高等级 | 绕过凭据签名、证书链、等级上限校验 | 让 `VerifyOhosDslmCred` 完整写入已验证字段 |
| 在 `ScheduleDslmStateMachine` 外直接写 `currState` | 不执行边上的副作用，事件/回调/定时器状态不一致 | 增加 `stateNodes[]` 边和处理函数 |
| 只改 Build 函数不改 Parse 函数 | 对端静默解析失败 | 两端按字段顺序成对修改并更新版本评估 |
| 在 `msglib` 中加入 DSLM 消息业务判断 | 传输层被业务耦合，三种形态难以复用 | 在 `services/common`/`services/dslm` 解析 |
| 只把异常 return 改成 `SUCCESS` | 让调用方把未验证或未完成状态当成有效结果 | 保留具体错误码并补状态/打点 |
| 复用已有错误码表达新含义 | 调用方和 HiSysEvent 看板语义断裂 | 追加错误码并同步文档 |
| 修改已有 IPC 枚举值 | ABI 静默错分发 | 只在枚举末尾追加 |
| 日志打印 credential 原文、udid 或私钥材料 | 安全信息泄漏 | 只记录 machineId、cookie、结果码、长度与状态 |

---

## 最小复现场景

修复说明或 commit message 写可执行的序列，而不是只写单点现象：

```text
前置条件：<系统形态 / SoftBus 状态 / 凭据文件或插件 / 对端在线状态>
操作序列：1. … 2. … 3. …
预期：…
实际（修复前）：…
```

本仓优先覆盖：设备上下线交错、消息超时重发、nonce 过期、凭据校验失败、SDK 回调超时、SA 卸载后重新加载，以及同一设备 64 个挂起请求边界。

---

## 单元测试约定

单测只在 standard 形态构建，目标是 `DslmUnitTest` 与 `DslmUnitTest2`。沿用现有风格：

```cpp
#include <gtest/gtest.h>

class DslmRpcProcessTest : public testing::Test {
public:
    void SetUp();
    void TearDown();
};

/**
 * @tc.name: OnPeerMsgReceived_InvalidInputPara_001
 * @tc.desc: function OnPeerMsgReceived with null input
 */
HWTEST_F(DslmRpcProcessTest, OnPeerMsgReceived_InvalidInputPara_001, TestSize.Level0)
{
    ASSERT_EQ(OnPeerMsgReceived(nullptr, nullptr, 0), ERR_INVALID_PARA);
}
```

约定：

- 测试套由 `ohos_unittest` 定义，使用 `HWTEST_F(..., TestSize.Level0)`；已有文件通常使用 `@tc.name` 与 `@tc.desc` 注释。
- C 依赖使用 `*_mock.cpp` 与 `cmocker/CMocker<T>`，不要把真实 DSoftBus/device_manager/HUKS 依赖引进白盒用例。
- 单例、静态设备表、全局 messenger 会跨用例保留状态；`TearDown()` 必须清理设备表、回调、mock 和标志位。
- 新用例必须断言具体结果码、状态、level、列表数量或资源释放结果，不要只验证「不崩溃」。
- 消息解析、RPC parcel、凭据校验的边界输入优先放 `DslmUnitTest`；socket 状态管理与 msglib 辅助函数优先放 `DslmUnitTest2`。

---

## Fuzz 验证

入口为 `test/dslm_fuzz_test/dslm_fuzzer/dslm_fuzzer.cpp`，目标为 `DslmFuzzTest`，`test/dslm_fuzz_test/BUILD.gn` 将它挂到 `dslm_fuzz` 组。fuzzer 覆盖 `OnPeerMsgReceived`、RPC、消息解析、凭据与 dump 等路径，并以 `L2_STANDARD`、`MAX_SEND_TIMES=5`、`SEND_MSG_TIMEOUT_LEN=40000` 编译。

新增或修改消息格式时：

1. 保留旧格式的 seed；
2. 增加合法最小消息、缺字段、超长、非 ASCII、错误 base64、错误 challenge、截断 parcel 等 seed；
3. 确认入口对任意输入只返回错误或安全结果，不崩溃、不泄漏；
4. 修改 fuzzer 目录后同步检查 `test/dslm_fuzz_test/BUILD.gn` 的 group 依赖。

---

## 回归面判定

| 改动模块 | 必须回归的场景 |
|---|---|
| `services/common/`、`services/dslm/dslm_msg_utils.c` | request/response 正常、缺字段、超长、错误 challenge、错误 base64 |
| `oem_property/` | standard/small/mini 凭据、debug/release、等级上限、HUKS/device_auth 不可用、凭据文件缺失 |
| `services/dslm/dslm_fsm_process.c` | 在线、离线、应答成功、应答失败、发送失败、请求超时、SDK 超时、重复请求 |
| `services/msg/`、`baselib/msglib/` | 建立/关闭会话、设备上下线、发送成功/失败、迟到通知、多设备并发 |
| standard IPC/SDK | 参数边界、timeout 1/60/非法值、cookie 0、SA 首次加载/已加载/卸载重拉、同步/异步 API |
| `common/dslm.gni`、各 `BUILD.gn` | standard、small、mini 目标分别编译；feature 宏开关组合 |
| `services/dfx/`、`hisysevent.yaml` | dump 无参数/`-h`/`-l`、服务启动失败、self init 失败、接口调用与查询打点 |

---

## DoD

1. 最小复现场景已验证，并写进提交说明。
2. 至少一条包含实际断言的 GTest 用例覆盖修改分支；消息/输入边界改动同时更新 fuzz 语料或说明无需新增语料。
3. 按本文「回归面判定」确认受影响场景。
4. 若涉及凭据、消息、IPC、feature、FSM 或导出符号，已分别完成对应对称性清单。
