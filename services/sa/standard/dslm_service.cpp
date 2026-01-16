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

#include "dslm_service.h"

#include <dlfcn.h>
#include <mutex>
#include <thread>

#include "iremote_object.h"
#include "singleton.h"
#include "utils_log.h"
#include "string_ex.h"

#include "device_security_defines.h"
#include "dslm_device_list.h"
#include "dslm_hidumper.h"
#include "dslm_ipc_process.h"
#include "dslm_rpc_process.h"

constexpr uint32_t UNLOAD_TIMEOUT = 10000;

namespace OHOS {
namespace Security {
namespace DeviceSecurityLevel {
REGISTER_SYSTEM_ABILITY_BY_ID(DslmService, DEVICE_SECURITY_LEVEL_MANAGER_SA_ID, true);

DslmService::DslmService(int32_t saId, bool runOnCreate) : SystemAbility(saId, runOnCreate), IRemoteStub(true)
{
    SECURITY_LOG_INFO("object initialization");
    ProcessLoadPlugin();
}

static void TimerProcessUnloadSystemAbility(const void *context)
{
    (void)context;

    if (!JudgeListDeviceType()) {
        return;
    }

    auto samgrProxy = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgrProxy == nullptr) {
        SECURITY_LOG_ERROR("get samgr failed");
        return;
    }

    int32_t ret = samgrProxy->UnloadSystemAbility(DEVICE_SECURITY_LEVEL_MANAGER_SA_ID);
    if (ret != ERR_OK) {
        SECURITY_LOG_ERROR("unload system ability failed");
        return;
    }
    SECURITY_LOG_INFO("unload system ability succeed");
}

static void SetSystemAbilityUnloadSchedule(TimerHandle &handle)
{
    if (!JudgeListDeviceType()) {
        return;
    }
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    if (handle != 0) {
        DslmUtilsStopTimerTask(handle);
    }

    handle = DslmUtilsStartOnceTimerTask(UNLOAD_TIMEOUT, TimerProcessUnloadSystemAbility, nullptr);
}

void DslmService::OnStart()
{
    SECURITY_LOG_INFO("start");
    std::thread thread([this]() {
        if (InitService() == SUCCESS) {
            SECURITY_LOG_INFO("init service success");
        }
        if (!Publish(this)) {
            SECURITY_LOG_ERROR("publish service failed");
        }
        SetSystemAbilityUnloadSchedule(unloadTimerHandle_);
    });
    thread.detach();
}

void DslmService::OnStop()
{
    UnInitService();
    ProcessUnloadPlugin();
    SECURITY_LOG_INFO("stop service");
}

int32_t DslmService::Dump(int fd, const std::vector<std::u16string> &args)
{
    dprintf(fd, "Dslm Dump:\n");
    if (args.size() == 0) {
        dprintf(fd, "please use hidumper -s said -a -h command help\n");
        return 0;
    }
    std::string arg0 = Str16ToStr8(args.at(0));
    if (arg0.compare("-h") == 0) {
        dprintf(fd, "Usage:\n");
        dprintf(fd, "       -h: command help\n");
        dprintf(fd, "       -l: dump all device\n");
    } else if (arg0.compare("-l") == 0) {
        DslmDumper(fd);
    } else {
        dprintf(fd, "please use hidumper -s said -a -h command help\n");
    }

    return 0;
}

int32_t DslmService::OnRemoteRequest(uint32_t code, MessageParcel &data, MessageParcel &reply, MessageOption &option)
{
    SetSystemAbilityUnloadSchedule(unloadTimerHandle_);

    do {
        if (IDeviceSecurityLevel::GetDescriptor() != data.ReadInterfaceToken()) {
            SECURITY_LOG_ERROR("local descriptor is not equal remote");
            break;
        }
        switch (code) {
            case CMD_GET_DEVICE_SECURITY_LEVEL:
                return ProcessGetDeviceSecurityLevel(data, reply);
            default:
                return IPCObjectStub::OnRemoteRequest(code, data, reply, option);
        }
    } while (false);

    return ERR_REQUEST_CODE_ERR;
}

int32_t DslmService::RequestDeviceSecurityLevel(const DeviceIdentify &identify, const RequestOption &option,
    const sptr<IRemoteObject> &callback, uint64_t cookie)
    {
        return Singleton<DslmIpcProcess>::GetInstance().DslmProcessGetDeviceSecurityLevel(&identify, &option,
            cookie, callback);
    }

int32_t DslmService::ProcessGetDeviceSecurityLevel(MessageParcel &data, MessageParcel &reply)
{
    DeviceIdentify identify;
    RequestOption option;
    sptr<IRemoteObject> callback;
    uint32_t cookie;
    int32_t ret = Singleton<DslmIpcProcess>::GetInstance().DslmGetRequestFromParcel(data, identify, option,
        callback, cookie);
    if (ret != SUCCESS) {
        SECURITY_LOG_ERROR("DslmGetRequestFromParcel failed, ret is %{public}d", ret);
        return ret;
    }

    ret = RequestDeviceSecurityLevel(identify, option, callback, cookie);
    if (ret != SUCCESS) {
        SECURITY_LOG_ERROR("RequestDeviceSecurityLevel failed, ret is %{public}d", ret);
        return ret;
    }

    ret = Singleton<DslmIpcProcess>::GetInstance().DslmSetResponseToParcel(reply, cookie);
    if (ret != SUCCESS) {
        SECURITY_LOG_ERROR("DslmSetResponseToParcel failed, ret is %{public}d", ret);
        return ret;
    }

    return SUCCESS;
}

void DslmService::ProcessLoadPlugin(void)
{
#ifdef PLUGIN_SO_PATH
    handle_ = dlopen(PLUGIN_SO_PATH, RTLD_NOW);
    if (!handle_) {
        SECURITY_LOG_ERROR("load %{public}s failed for %{public}s", PLUGIN_SO_PATH, dlerror());
    }
#endif
}

void DslmService::ProcessUnloadPlugin(void)
{
#ifdef PLUGIN_SO_PATH
    if (handle_) {
        dlclose(handle_);
        SECURITY_LOG_INFO("unload %{public}s", PLUGIN_SO_PATH);
    }
#endif
}
} // namespace DeviceSecurityLevel
} // namespace Security
} // namespace OHOS
