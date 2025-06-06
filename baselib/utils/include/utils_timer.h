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

#ifndef SEC_UTILS_TIMER_H
#define SEC_UTILS_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uintptr_t TimerHandle;

typedef void (*TimerProc)(const void *context);

TimerHandle DslmUtilsStartPeriodicTimerTask(uint32_t interval, TimerProc callback, const void *context);

TimerHandle DslmUtilsStartOnceTimerTask(uint32_t interval, TimerProc callback, const void *context);

void DslmUtilsStopTimerTask(TimerHandle handle);

#ifdef __cplusplus
}
#endif

#endif // SEC_UTILS_TIMER_H