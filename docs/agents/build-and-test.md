# 编译与测试

← 返回 [`AGENTS.md`](../../AGENTS.md)

---

## 构建定位

本仓是 OpenHarmony 整仓部件，不能脱离完整源码树独立构建。源码在整仓中的标准落位是：

```text
<OHOS_ROOT>/base/security/device_security_level
```

| 项 | 值 |
|---|---|
| `part_name` | `device_security_level` |
| `subsystem_name` | `security` |
| `bundle.json` name | `@ohos/device_security_level` |
| 主 standard 产物 | `libdslm_service.z.so` |
| standard SDK 产物 | `libdslm_sdk.z.so` |
| standard SA | 3511 / `dslm_service` |
| 适配系统 | `standard`、`small`、`mini` |

`bundle.json` 的构建分组：

| 分组 | 目标 |
|---|---|
| `fwk_group` | `interfaces/inner_api:fwk_group`，按 `os_level` 选择 `dslm_sdk` / `dslm_sdk_small` / `dslm_sdk_mini` |
| `service_group` | `oem_property/ohos:dslm_service`、`oem_property/ohos:dslm_ohos_cred_file`、`profile:dslm_service.rc`（GN target；实际安装源文件为 `profile/dslm_service.cfg`） |
| `inner_kits` | `interfaces/inner_api:dslm_sdk`、`services/sa:dslm_service` |
| `test` | `test/dslm_unit_test:dslm_test`、`test/dslm_fuzz_test:dslm_fuzz` |

---

## 构建命令

以下命令应在**完整 OpenHarmony 源码根目录**执行。先用实际产品名替换 `<product>`；这些示例不是可直接复制的完整命令：

```bash
# 构建整个部件
./build.sh --product-name <product> --build-target device_security_level

# 使用 hb 构建整个部件
hb build -T device_security_level

# 仅构建单元测试组（standard 产品）
./build.sh --product-name <product> --build-target dslm_test

# 仅构建 fuzz 组（standard 产品）
./build.sh --product-name <product> --build-target dslm_fuzz
```

也可以使用 GN label 定位单个目标：

```bash
./build.sh --product-name <product> \
  --build-target "//base/security/device_security_level/interfaces/inner_api:dslm_sdk"

./build.sh --product-name <product> \
  --build-target "//base/security/device_security_level/services/sa:dslm_service"

./build.sh --product-name <product> \
  --build-target "//base/security/device_security_level/test/dslm_unit_test:dslm_test"
```

> 本工作目录不是完整 OpenHarmony 源码树，因此本地只完成命令与 `bundle.json`/`BUILD.gn` 的静态核对，未在此目录实际编译或运行设备测试。命令的执行前提是完整 OpenHarmony 源码树；在具备该环境时可按本文端到端执行。

---

## `common/dslm.gni` 与共享配置

`common/dslm.gni` 是兄弟部件路径参数的集中入口：

| 参数 | 默认路径用途 |
|---|---|
| `dslm_hilog_path` | hilog lite |
| `dslm_dsoftbus_path` | DSoftBus |
| `dslm_common_lib_path` | commonlibrary utils lite |
| `dslm_ipc_path` | lite IPC |
| `dslm_samgr_path` | systemability manager |
| `dslm_lite_component_path` | lite 组件配置 |

不要在 BUILD.gn 中硬编码这些兄弟部件路径；新增外部依赖时同时检查 `bundle.json` 的 `component.deps.components`。

`common/BUILD.gn` 提供两组公共配置：

- `common_configs`：`device_security_level_feature_coverage = true` 时添加 `--coverage` 编译和链接参数；
- `common_lite_configs`：standard 定义 `L2_STANDARD`，linux/liteos-a 定义 `L1_SMALL`，liteos-m 定义 `L0_MINI`。

形态差异应优先通过 `standard/`、`lite/small/`、`lite/mini/` 目录组织；只有共享文件中的必要差异才使用这些宏。

---

## 目标与系统形态

| 形态 | 主要目标 | 入口 |
|---|---|---|
| standard | `interfaces/inner_api:dslm_sdk`、`services/sa:dslm_service` | C++ SA 3511，`services/sa/standard/dslm_service.cpp` |
| small | `interfaces/inner_api:dslm_sdk_small`、`services/sa:dslm_server` | lite IPC 可执行文件，`services/sa/lite/small/` |
| mini | `interfaces/inner_api:dslm_sdk_mini`、`services/sa:dslm_server` | liteos-m 静态库，`services/sa/lite/mini/` |

核心 source set：

| 目标 | 文件表来源 | 用途 |
|---|---|---|
| `baselib/utils:utils_static` | `baselib/utils/BUILD.gn` | 工具库 |
| `baselib/msglib:messenger_static` | `baselib/msglib/BUILD.gn` | 信使传输层 |
| `services/common:service_common_obj` | `services/common/BUILD.gn` | crypto、一级消息序列化 |
| `services/dslm:service_dslm_obj` | `services/dslm/BUILD.gn` | 核心业务与 FSM |
| `services/msg:service_msg_obj` | `services/msg/BUILD.gn` | messenger 包装 |
| `services/dfx:dslm_extension_dfx` | `services/dfx/BUILD.gn` | HiSysEvent、dump、hitrace |
| `oem_property/ohos/standard:dslm_ohos_cred_obj` | `oem_property/ohos/standard/BUILD.gn` | standard HUKS/device_auth 凭据实现 |

---

## 编译与加固约束

standard target 普遍启用：

```text
integer_overflow = true
ubsan = true
boundary_sanitize = true
cfi = true
cfi_cross_dso = true
branch_protector_ret = "pac_ret"
blocklist = "../../cfi_blocklist.txt"
```

改动时还要遵守目标已有的编译选项与接口导出约束：

- 不要引入 `try/catch` 或依赖 RTTI；
- 不要删除或放宽 integer overflow、UBSan、boundary sanitize、CFI、PAC 配置；
- 跨 DSO 的新增符号同步加入对应 version script（`services/sa/dslm.versionscript` 或 `interfaces/inner_api/dslm_sdk.versionscript`）；
- `MAX_SEND_TIMES` 与 `SEND_MSG_TIMEOUT_LEN` 在生产 source set 中定义为 5/40000，测试 source set 定义为 5/500，不能误把测试值当成生产值；
- 新增依赖同时修改目标的 `deps`/`external_deps` 与 `bundle.json` 组件依赖，并检查 license。

---

## 测试套的选择

### `dslm_test`

`test/dslm_unit_test/BUILD.gn` 的 `group("dslm_test")` 仅在 `os_level == "standard"` 时依赖：

- `DslmUnitTest`：核心 RPC、消息、OEM 凭据、DFX 和 messenger 路径，使用 `device_manager_mock.cpp`、内存 mock、消息接口 mock、回调 mock；
- `DslmUnitTest2`：msglib/socket 辅助路径与第二组 mock。

两个目标都启用 integer overflow、UBSan、boundary sanitize、CFI 和 PAC；安装目录为：

```text
/data/test/device_security_level/device_security_level/<DslmUnitTest 或 DslmUnitTest2>
```

将尖括号中的占位符替换为实际二进制名后再执行。例如：

```bash
hdc shell /data/test/device_security_level/device_security_level/DslmUnitTest
hdc shell /data/test/device_security_level/device_security_level/DslmUnitTest \
  '--gtest_filter=DslmRpcProcessTest.OnPeerMsgReceived_InvalidInputPara_001'
hdc shell /data/test/device_security_level/device_security_level/DslmUnitTest \
  --gtest_list_tests
```

### `dslm_fuzz`

`test/dslm_fuzz_test/BUILD.gn` 的 `group("dslm_fuzz")` 依赖 `dslm_fuzzer:DslmFuzzTest`，仅 standard 构建。它把消息解析、RPC、凭据、dump、standard IPC 等大量生产源直接编入 fuzz 二进制，并固定 `L2_STANDARD`、`MAX_SEND_TIMES=5`、`SEND_MSG_TIMEOUT_LEN=40000`。

修改外部输入、JSON/base64、parcel、凭据格式时，优先补 fuzz seed，并确认错误输入不会崩溃、越界或泄漏。

---

## 新增文件与 feature 的构建清单

### 新增源文件

1. 按所属 target 加入对应 `BUILD.gn` 的 `sources`；
2. 若服务核心源文件同时需要被单测编译，检查 `service_dslm_test_obj` 是否也同步；
3. 新 include 目录加入 target 的 `include_dirs`；
4. 若只适用于某个形态，放进对应目录或条件块，而不是让其它形态看到无效符号；
5. 若新增接口符号，更新 version script。

### 新增 feature

1. 在实际拥有该 feature 的 `BUILD.gn` 的 `declare_args()` 加默认值；
2. 在条件块中加入 define、源文件或依赖；
3. 如果该 feature 需要作为部件配置对外声明，在 `bundle.json` 的 `component.features` 登记；仅用于构建配置的开关（例如 coverage）按现有 `BUILD.gn` 方式维护，不要强行加入 `bundle.json`；
4. 宏关闭时至少构建一次，确认宏内成员没有被宏外引用；
5. 若改变安全级别、凭据插件、会话名或默认行为，按 [`high-risk.md`](high-risk.md)「变更风险矩阵」人工复核。

---

## 无完整构建环境时的静态自检

没有完整源码树或设备时，仍必须完成：

| # | 检查 | 方法 |
|---|---|---|
| 1 | 新增源文件加入正确 target | 搜对应 `BUILD.gn` 的 `sources` |
| 2 | 新增 include 目录齐全 | 对照 include 语句与 `include_dirs` |
| 3 | 新增依赖两处登记 | 对照 `BUILD.gn` 与 `bundle.json` |
| 4 | feature 配置链路一致 | `declare_args` / define 条件 / 源文件条件；需要对外声明的 feature 再核对 `bundle.json` |
| 5 | 三种形态可见性正确 | 检查 `L0_MINI` / `L1_SMALL` / `L2_STANDARD` 与目录分支 |
| 6 | IPC code 只追加且多处同步 | 见 [`boundaries.md`](boundaries.md)「新增 IPC 接口的清单（standard 形态）」 |
| 7 | proxy/stub 或 Build/Parse 顺序对称 | 并排比对写入和读取 |
| 8 | 外部输入都有长度、空指针、范围校验 | 消息、parcel、凭据、设备标识逐项检查 |
| 9 | `MALLOC`/`FREE`、timer start/stop 成对 | 检查所有成功与失败分支 |
| 10 | 日志/dump 无凭据和私钥材料 | 检查新增日志字段与 dump 输出 |
| 11 | HiSysEvent 新事件与 yaml 同步 | 事件名、类型、字段逐项核对 |
| 12 | test/fuzz 目标已挂进 group | 检查 `test/*/BUILD.gn` 的 `deps` |
| 13 | 静态断言与真实路径一致 | grep 函数名、宏名、目标名；Markdown 相对链接无断链 |

---

## DoD

- [ ] 静态自检 13 项完成；
- [ ] 有完整 OpenHarmony 构建环境时，`device_security_level` 主目标编译通过；
- [ ] standard 环境下相关 `DslmUnitTest`/`DslmUnitTest2` 编译并运行通过；
- [ ] 消息/parcel/凭据改动时，`DslmFuzzTest` 编译并执行相关输入；
- [ ] [`fix-and-verify.md`](fix-and-verify.md)「回归面判定」中的场景逐项确认；
- [ ] 未实际执行的构建/设备测试在最终回复中明确说明，不用「应该可以」代替证据。
