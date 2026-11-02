# 高风险知识：高危入口、高危配置、高危序列、变更风险矩阵

← 返回 [`AGENTS.md`](../../AGENTS.md)

---

## 高危入口

本仓本身就是安全部件：它产出的“设备安全级别”是其它部件放行用户数据的依据。两个方向的错误都可能造成安全事件——**误放宽**会让低安全设备被当作高安全设备，**误收紧**则可能让正常设备互不信任，导致分布式业务不可用。

按事故严重度排序，改动前先确认是否真的必须改：

| 等级 | 入口 | 位置 | 事故形态 |
|---|---|---|---|
| **S** | 凭据校验主流程 | `oem_property/ohos/common/dslm_ohos_verify.c`（`VerifyOhosDslmCred` → 证书链 `ValidateCertChainAdapter` → nonce 校验 → `VerifyDslmCredential` → `CheckCredInfo`） | 校验逻辑改错：伪造凭据被接受（放宽）或全部对端校验失败（收紧） |
| **S** | 凭据函数表注册 | `oem_property/ohos/standard/dslm_ohos_credential.c`（`__attribute__((constructor))`）；或 `PLUGIN_SO_PATH` 插件的 `dlopen`（`dslm_service.cpp::ProcessLoadPlugin`） | 未注册/加载失败时 `DefaultVerifyDslmCred` 等返回 -1，**所有凭据操作失败**；默认凭据函数记录 info 级日志，插件 `dlopen` 失败记录 error 级日志 |
| **S** | 随机数生成 | `services/common/dslm_crypto.c` `GenerateRandom` | standard 用 OpenSSL `RAND_bytes`；**非 standard 形态返回固定值**（`rand->value[0] = 1`）。nonce 是防重放根基，改动这里直接动摇 challenge 机制 |
| **S** | 外部消息入口校验 | `services/common/dslm_msg_serialize.c` `CheckMessage` + `ParseMessage`；`services/dslm/dslm_msg_utils.c` 两个 `Parse*` | 来自网络的不可信输入第一关。去掉长度/字符集/`\0` 结尾校验 = 给 fuzz 和攻击者开门 |
| **A** | FSM 迁移表 | `services/dslm/dslm_fsm_process.c` `stateNodes[]` | 漏边 = 事件静默丢弃；错边 = 状态卡死或凭据状态错乱 |
| **A** | 默认等级赋值点 | `dslm_fsm_process.c` `ProcessDeviceOnline()`（`level==0 && osType==10` → 1） | 默认 SL1 的语义被破坏：未验证设备获得非默认值，或正常设备拿不到默认值 |
| **A** | IPC 反序列化 | `services/sa/standard/dslm_ipc_process.cpp` `DslmGetRequestFromParcel` | parcel 可读长度或缓冲区读取/复制校验缺失可能导致读取异常；timeout 不在 1–60s 时当前会回退到默认值 45s |
| **A** | IPC 接口码枚举 | `common/include/dslm_service_ipc_interface_code.h` | 枚举值即 ABI，修改已有值会让新旧部件静默错分发 |
| **A** | nonce 校验条件编译 | `services/dslm/dslm_inner_process.c` `VerifyDeviceInfoResponse` 中 nonce 过期检查在 `#ifdef L2_STANDARD` 内 | 误以为三形态行为一致；在 small/mini 上「nonce 过期仍被接受」是现状而非 bug |
| **B** | SA 自动卸载 | `dslm_service.cpp` `UNLOAD_TIMEOUT`(10s) + `JudgeListDeviceType()` | 卸载条件改错：SA 被反复卸载重拉（凭据函数表、设备表重建）或永不卸载 |
| **B** | dump 输出内容 | `services/dfx/dslm_hidumper.c` | 现约定：打印 manufacture/brand/model 等，**不打印 udid**。把 udid/凭据原文加进 dump 是信息泄漏 |
| **B** | 凭据长度上限 | `oem_property/common/dslm_credential.c` `MAX_CRED_LEN=81920`；`dslm_msg_serialize.h` `MSG_BUFF_MAX_LENGTH` | 上限调大 = 放大内存攻击面；调小 = 合法凭据被拒 |

---

## 高危配置项

### feature 宏

以下三个与安全或通信相关的 feature 宏，其 `declare_args()` **分散在三个 `BUILD.gn`**（不集中在本仓 `common/dslm.gni`，那里只有兄弟部件路径变量）；`common/BUILD.gn` 另外定义了 coverage feature，但 `bundle.json` 未登记它，不能把以下三项视为全部构建开关：

| 宏 | 声明位置 | 默认值 | 影响面 |
|---|---|---|---|
| `device_security_level_feature_cred_level` | `oem_property/ohos/standard/BUILD.gn` | `1` | **决定设备出厂断言的安全级别**：2–5 分别安装 `dslm_finger_sl2..sl5.cfg`，其它值装 `dslm_finger.cfg`，统一输出为 `dslm_finger.cfg`。改动即改变设备对外声明的 SL |
| `device_security_level_feature_plugin_path` | `services/sa/BUILD.gn` | `""` | 非空时：不编 `dslm_oem_ext`，定义 `PLUGIN_SO_PATH`，服务启动时 `dlopen` 厂商插件提供凭据函数表。**设置后仓内 oem 实现完全不进产物**，插件加载失败 = 全部校验失败 |
| `device_security_level_feature_secondary_session_name` | `services/msg/BUILD.gn` | `""` | 非空时定义 `SECONDARY_SOCKET_NAME`，messenger 启用次会话/socket 名 |

> 仓内自带的 `dslm_finger*.cfg` 当前标记为 `releaseType: "debug"`，凭据字符串采用四段结构 `<header>.<payload>.<signature>.<attestation>`。`CheckCredInfo()` 在 `udid` 为空时跳过标识检查；当 `releaseType` 以 `debug` 开头且 `udid` 非空时，按凭据中的 udid 长度与设备标识比较，并不检查两者完整长度是否相等。当前这些配置的 payload 没有 udid，因此不会触发这项比对。它们代表仓内的默认配置，不能据此推断产品凭据的用途；商用设备的凭据来源需由产品和厂商适配方案确定。替换这些文件会改变使用默认构建的设备所声明的安全级别。

### profile 与加固配置

| 配置 | 位置 | 风险 |
|---|---|---|
| SA 依赖 | `profile/dslm_service.json`：SA 3511 依赖 4700、3510，`depend-time-out = 60000`，并设置 `run-on-create` | 依赖 SA 未注册时本 SA 不启动，表现为「服务没起来」 |
| 运行身份 | `profile/dslm_service.cfg`：由 `profile/BUILD.gn` 的 `ohos_prebuilt_etc("dslm_service.rc")` 安装；包含 service `dslm_service`、uid/gid `3046`、apl `system_basic` 和 seclabel `u:r:dslm_service:s0` | 改 uid/gid/apl/seclabel 影响权限面，属安全评审范围 |
| 符号可见性 | `services/sa/dslm.versionscript`、`interfaces/inner_api/dslm_sdk.versionscript` | version script 没收的符号对外不可见；新增导出符号必须登记（见 [`boundaries.md`](boundaries.md)「新增 IPC 接口的清单（standard 形态）」） |
| sanitizer/CFI | standard 形态各 target 的 `sanitize` 块（`integer_overflow`/`ubsan`/`boundary_sanitize`/`cfi`+`cfi_cross_dso`）+ `branch_protector_ret = "pac_ret"` + `cfi_blocklist.txt` | **不得删减弱化**。新增 target 沿用同一配置；跨 DSO 函数指针调用受 CFI 约束，违规运行期直接终止进程 |
| 超时与次数 | `services/dslm/BUILD.gn` 的 `defines`：`MAX_SEND_TIMES=5`、`SEND_MSG_TIMEOUT_LEN=40000`（测试目标 500） | 改这里同时改变所有产品的重试行为 |

---

## 高危事件序列

以下序列是缺陷密集区，改动相关模块时逐条验证：

| 序列 | 为什么危险 |
|---|---|
| 设备上线 → 请求 → 超时重试 → 失败 | 非强制路径按 `queryTimes > MAX_SEND_TIMES` 判断是否继续发送，单次请求超时为 40s；SDK 请求还有独立的 `keep` 超时。`REQUEST_INTERVAL` 虽定义为 24h，但当前 `CheckNeedToResend()` 的后续时间判断不可达，不能把它当作实际去重窗口，也不能简单按重试次数推导统一的最长等待时间 |
| 请求在途 → 对端下线 → 对端重上线 | 下线回到 `STATE_INIT` 并回调所有挂起请求；重上线重新挑战。时序交错时 nonce 已更新但旧应答到达 |
| nonce 60s 有效期 vs 40s 发送超时交错 | 若重发前触发了 nonce 刷新，迟到的旧应答会因 nonce 不匹配而失败；单纯 40s 超时不必然刷新 nonce |
| 单设备并发 SDK 请求打满 64 | 第 65 个请求得到 `ERR_SA_BUSY`；调用方常把任何非 0 都当失败重试，加剧打满 |
| 多设备同时上线 | `CreatOrGetDslmDeviceInfo` 使用 `GetDeviceListSize() > MAX_DEVICE_CNT` 判断；`MAX_DEVICE_CNT` 为 128 的创建检查阈值，边界数量需结合当前比较符核对，失败时事件可能被吞 |
| SA 卸载 → 立即有新请求 | 函数表、设备表、`g_messenger` 全部重建；loader 10s 加载超时窗口 |
| `PLUGIN_SO_PATH` 插件加载失败 | 服务可继续启动，但插件加载会记录 error 级日志；函数表未注册时默认凭据操作返回 -1，并记录 info 级日志 |
| 服务启动时 DSLM 初始化持续失败 | `InitService` 重试 `InitDslmProcess()`；messenger 未就绪、本机设备标识获取失败或设备对象创建失败都可能导致重试。退出条件为 `times > TRY_TIMES`，连续失败时约执行 22 次 `sleep(0.5s)`，总耗时还包括初始化调用耗时。循环退出后需结合具体失败日志排查，不能据此断定 messenger 不可用或请求必然返回 `ERR_MSG_NOT_INIT` |
| 凭据前置校验通过但等级超上限 | `CheckCredInfo` 把 `credLevel` 清零并返回 `ERR_CHECK_CRED_INFO`——**清零后字段不能作为可信等级使用**，下游按无有效等级处理 |

---

## 变更风险矩阵

| 改动类型 | 风险 | 灰度手段 | 必须的验证 |
|---|---|---|---|
| 新增 feature 宏包裹的新能力 | 视功能而定 | 宏本身是编译期开关，默认空/关 | 宏开/关各编一次，并按功能覆盖相关系统形态 |
| 修改 `device_security_level_feature_cred_level` 默认值或 finger cfg 内容 | **极高** | 无 | 改变设备对外声明的安全级别，必须人工评审 |
| 凭据校验逻辑任何改动 | **极高** | 无 | `DslmUnitTest`/`DslmUnitTest2` 中相关用例通过，并补充过期、伪造、越级凭据的负向用例 |
| 消息格式（两级 JSON 字段）变更 | 高 | 无（跨设备协议） | 两端同步修改 + fuzz 语料更新 + 版本号评估 |
| 新增 IPC 接口（末尾追加） | 低 | 旧客户端不调即不受影响 | [`boundaries.md`](boundaries.md)「新增 IPC 接口的清单（standard 形态）」七步 |
| 修改 IPC 已有接口语义或参数 | **极高** | 无 | 只能新增接口，不得修改 |
| FSM 新增一条边 | 中 | 无 | 该边 + 相邻态的所有事件组合 |
| FSM 修改既有边 | 高 | 无 | 本文「高危事件序列」相关序列全部验证 |
| 修改超时/次数等数值参数 | 中 | 无 | 评估与 nonce 60s、SA 卸载 10s、SDK 默认 45s 的相对关系 |
| 日志/打点调整 | 低 | 无 | 确认无 udid/凭据原文进日志（见 [`observability.md`](observability.md)） |
| 纯重构 | 中 | 无 | 重构前后运行 `DslmUnitTest`/`DslmUnitTest2`，并确认相关用例通过 |

### 本仓可用的灰度手段

1. **编译期 feature 宏** —— 唯一成熟路径，新能力默认关闭合入。
2. **凭据/插件替换** —— 厂商维度差异化，但不能用于代码逻辑灰度。

> **没有运行期配置项，没有 A/B 能力。** 编译期宏可以控制新能力是否进入产物，但不能替代安全评审、兼容性检查和高危事件序列回归。

---

## 改动前自检

动手改本文「高危入口」列出的入口之前，先回答：

1. 这个改动会让**未验证的凭据**有机会被当成已验证吗？（校验放宽方向）
2. 它会让**合法凭据**在某些形态/场景下验证失败吗？（校验收紧方向；三种形态分支都过了吗）
3. 它触发本文「高危事件序列」中的哪几条？逐条列出。
4. 它改了跨边界的东西吗（消息字段、错误码、IPC 码、版本号、导出符号）？是新增兼容内容，还是改变既有含义？已有契约原则上只能追加，不能修改或重排。
5. 凭据原文、私钥、udid 会不会因此进入日志或 dump？
