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

#include "device_security_level_callback_helper.h"

#include <cstdint>
#include <functional>
#include <map>
#include <new>
#include <utility>

#include "hilog/log_cpp.h"
#include "idevice_security_level.h"
#include "timer.h"

#include "device_security_info.h"
#include "device_security_level_callback_stub.h"
#include "device_security_level_defines.h"

namespace OHOS {
namespace Security {
namespace DeviceSecurityLevel {
using namespace OHOS::HiviewDFX;

constexpr uint32_t MAX_CALLBACKS_NUM = 128;

DeviceSecurityLevelCallbackHelper::DeviceSecurityLevelCallbackHelper()
{
    auto request = [this](uint32_t code, MessageParcel &data, uint32_t &cookie, uint32_t &result, uint32_t &level) {
        return this->OnRequest(code, data, cookie, result, level);
    };
    auto response = [this](uint32_t cookie, uint32_t result, uint32_t level) {
        return this->ResponseDeviceSecurityLevel(cookie, result, level);
    };
    stub_ = new (std::nothrow) DeviceSecurityLevelCallbackStub(request, response);
}

DeviceSecurityLevelCallbackHelper::~DeviceSecurityLevelCallbackHelper()
{
    stub_ = nullptr;
}

bool DeviceSecurityLevelCallbackHelper::Publish(const DeviceIdentify &identity, const ResultCallback &callback,
    uint32_t keep, sptr<DeviceSecurityLevelCallbackStub> &stub, uint32_t &cookie)
{
    if (stub_ == nullptr) {
        return false;
    }

    auto result = holder_.PushCallback(identity, callback, keep, cookie);
    if (!result) {
        HILOG_ERROR(LOG_CORE, "DeviceSecurityLevelCallbackHelper::PushCallback failed");
        return false;
    }

    stub = stub_;
    return true;
}

bool DeviceSecurityLevelCallbackHelper::Withdraw(uint32_t cookie)
{
    if (cookie == 0) {
        return false;
    }

    auto result = holder_.PopCallback(cookie);
    if (!result) {
        HILOG_ERROR(LOG_CORE, "DeviceSecurityLevelCallbackHelper::withdraw failed");
        return false;
    }
    return true;
}

int32_t DeviceSecurityLevelCallbackHelper::OnRequest(uint32_t code, MessageParcel &data, uint32_t &cookie,
    uint32_t &result, uint32_t &level)
{
    if (code == DeviceSecurityLevelCallbackStub::CMD_SET_DEVICE_SECURITY_LEVEL) {
        cookie = data.ReadUint32();
        result = data.ReadUint32();
        level = data.ReadUint32();
        HILOG_INFO(LOG_CORE, "OnRemoteRequest cookie %{public}u, result %{public}u, level %{public}u", cookie,
            result, level);
    }

    return SUCCESS;
}

int32_t DeviceSecurityLevelCallbackHelper::ResponseDeviceSecurityLevel(uint32_t cookie, uint32_t result,
    uint32_t level)
{
    HILOG_INFO(LOG_CORE, "ResponseDeviceSecurityLevel cookie %{public}u, result %{public}u, level %{public}u",
        cookie, result, level);
    holder_.PopCallback(cookie, result, level);

    return SUCCESS;
}

DeviceSecurityLevelCallbackHelper::CallbackInfoHolder::CallbackInfoHolder()
{
}

DeviceSecurityLevelCallbackHelper::CallbackInfoHolder::~CallbackInfoHolder()
{
}

bool DeviceSecurityLevelCallbackHelper::CallbackInfoHolder::PushCallback(const DeviceIdentify &identity,
    const ResultCallback &callback, uint32_t keep, uint32_t &cookie)
{
    (void)keep;
    std::lock_guard<std::mutex> lock(mutex_);
    if (map_.size() > MAX_CALLBACKS_NUM) {
        HILOG_ERROR(LOG_CORE, "DeviceSecurityLevelCallbackHelper::PushCallback reached max");
        return false;
    }

    cookie = ++generate_;
    CallbackInfo info = {.identity = identity, .callback = callback, .cookie = cookie};
    auto result = map_.emplace(generate_, info);
    return result.second;
}

bool DeviceSecurityLevelCallbackHelper::CallbackInfoHolder::PopCallback(uint32_t cookie, uint32_t result,
    uint32_t level)
{
    DeviceIdentify identity;
    ResultCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto iter = map_.find(cookie);
        if (iter == map_.end()) {
            return false;
        }
        identity = iter->second.identity;
        callback = iter->second.callback;
        map_.erase(iter);
    }

    if (callback != nullptr) {
        DeviceSecurityInfo *info = new (std::nothrow) DeviceSecurityInfo();
        if (info != nullptr) {
            info->magicNum = SECURITY_MAGIC;
            info->result = result;
            info->level = level;
        }
        callback(&identity, info);
    }

    return true;
}

bool DeviceSecurityLevelCallbackHelper::CallbackInfoHolder::PopCallback(uint32_t cookie)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = map_.find(cookie);
    if (iter == map_.end()) {
        return false;
    }
    map_.erase(iter);
    return true;
}
} // namespace DeviceSecurityLevel
} // namespace Security
} // namespace OHOS
