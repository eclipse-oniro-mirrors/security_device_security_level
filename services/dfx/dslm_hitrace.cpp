/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dslm_hitrace.h"

#include "hitrace_meter.h"

#ifdef __cplusplus
extern "C" {
#endif

void DslmStartProcessTrace(const char *value)
{
    StartTraceEx(HITRACE_LEVEL_INFO, HITRACE_TAG_DLSM, value, "");
}

void DslmStartStateMachineTrace(uint32_t machineId, uint32_t event)
{
    std::string traceValue =
        std::string("StartStateMachine_") + std::to_string(machineId) + "_" + std::to_string(event);

    StartTraceEx(HITRACE_LEVEL_INFO, HITRACE_TAG_DLSM, traceValue.c_str(), "");
}

void DslmFinishProcessTrace(void)
{
    FinishTraceEx(HITRACE_LEVEL_INFO, HITRACE_TAG_DLSM);
}

void DslmStartProcessTraceAsync(const char *value, uint32_t owner, uint32_t cookie)
{
    std::string traceValue = std::string(value) + "_" + std::to_string(owner) + "_" + std::to_string(cookie);
    StartAsyncTraceEx(HITRACE_LEVEL_INFO, HITRACE_TAG_DLSM, traceValue.c_str(), cookie, "", "");
}

void DslmFinishProcessTraceAsync(const char *value, uint32_t owner, uint32_t cookie)
{
    std::string traceValue = std::string(value) + "_" + std::to_string(owner) + "_" + std::to_string(cookie);
    FinishAsyncTraceEx(HITRACE_LEVEL_INFO, HITRACE_TAG_DLSM, traceValue.c_str(), cookie);
}

void DslmCountTrace(const char *name, int64_t count)
{
    CountTraceEx(HITRACE_LEVEL_INFO, HITRACE_TAG_DLSM, name, count);
}

#ifdef __cplusplus
}
#endif
