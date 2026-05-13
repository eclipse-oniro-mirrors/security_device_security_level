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

#include "messenger_device_status_manager.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include "device_manager.h"
#include "securec.h"
#include "singleton.h"

#include "messenger_utils.h"
#include "utils_log.h"
#include "utils_mem.h"
#include "utils_json.h"

#define PKG_NAME_LEN 128
#define TYPE_PLACE 8
#define DEFAULT_TYPE 10
#define LAPTOP_TYPE 12
#define SMART_DISPLAY_TYPE 2562

static void GetDeviceSecurityLevelByNetworkId(const OHOS::DistributedHardware::DmDeviceInfo &info, int32_t &level);

namespace OHOS {
namespace Security {
namespace DeviceSecurityLevel {
using namespace OHOS::DistributedHardware;

class DeviceStatusControlBlock final : public Singleton<DeviceStatusControlBlock> {
public:
    using StateReceiver = std::function<int32_t(const DeviceIdentify *devId, uint32_t status, int32_t level)>;
    void Reset(const std::string &pkgName, WorkQueue *queue, StateReceiver deviceStatusReceiver)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pkgName_ = pkgName;
        queue_ = queue;
        receiver_ = deviceStatusReceiver;
    }
    const std::string &GetPackageName() const
    {
        return pkgName_;
    }

    const StateReceiver &GetStateReceiver() const
    {
        return receiver_;
    }

    WorkQueue *GetQueue() const
    {
        return queue_;
    }

private:
    std::mutex mutex_;
    WorkQueue *queue_ {nullptr};
    std::string pkgName_ {};
    StateReceiver receiver_ {nullptr};
};

class DslmStateReceiver final {
public:
    DslmStateReceiver() = default;
    ~DslmStateReceiver() = default;
};

class DslmDeviceState final : public DeviceStateCallback,
                              public DmInitCallback,
                              public std::enable_shared_from_this<DslmDeviceState> {
public:
    enum State : uint32_t {
        EVENT_NODE_STATE_OFFLINE = 0,
        EVENT_NODE_STATE_ONLINE = 1,
    };

    struct QueueStatusData {
        DeviceIdentify srcIdentity {0, {0}};
        uint32_t status {0};
        uint32_t level {0};
    };

    DslmDeviceState() = default;
    ~DslmDeviceState() override = default;

    void OnDeviceOnline(const DmDeviceInfo &deviceInfo) override
    {
    }

    void OnDeviceOffline(const DmDeviceInfo &deviceInfo) override
    {
        MessengerOnNodeStateChange(deviceInfo, EVENT_NODE_STATE_OFFLINE);
    }

    void OnDeviceChanged(const DmDeviceInfo &deviceInfo) override
    {
    }

    void OnDeviceReady(const DmDeviceInfo &deviceInfo) override
    {
        MessengerOnNodeStateChange(deviceInfo, EVENT_NODE_STATE_ONLINE);
    }

    void OnRemoteDied() override
    {
    }

    static void MessengerOnNodeStateChange(const DmDeviceInfo &info, State state)
    {
        DeviceIdentify identity = {DEVICE_ID_MAX_LEN, {0}};
        int32_t level = 0;
        if (!MessengerGetDeviceIdentifyByNetworkId(info.networkId, &identity)) {
            SECURITY_LOG_ERROR("MessengerOnNodeStateChange copy device error");
            return;
        }
        if (state == EVENT_NODE_STATE_ONLINE) {
            GetDeviceSecurityLevelByNetworkId(info, level);
        }
        ProcessDeviceStatusReceiver(&identity, state, level);
    }

    static void ProcessDeviceStatusReceiver(const DeviceIdentify *devId, uint32_t status, int32_t level)
    {
        if (devId == nullptr || devId->length == 0) {
            SECURITY_LOG_ERROR("ProcessDeviceStatusReceiver, invalid input");
            return;
        }

        auto queue = DeviceStatusControlBlock::GetInstance().GetQueue();
        if (queue == nullptr) {
            SECURITY_LOG_ERROR("ProcessDeviceStatusReceiver, invalid queue");
            return;
        }

        QueueStatusData *data = new (std::nothrow) QueueStatusData;
        if (data == nullptr) {
            SECURITY_LOG_ERROR("ProcessDeviceStatusReceiver, malloc result null");
            return;
        }
        data->srcIdentity = *devId;
        if (level > 0) {
            data->level = static_cast<uint32_t>(level);
        }
        data->status = status;

        uint32_t maskId = MaskDeviceIdentity((const char *)&devId->identity[0], DEVICE_ID_MAX_LEN);
        SECURITY_LOG_INFO("OnlineStateChange device %{public}x*** change to %{public}s, level is %{public}d", maskId,
            (status == EVENT_NODE_STATE_ONLINE) ? " online " : " offline ", level);

        auto process = [](const uint8_t *data, uint32_t len) {
            if (data == nullptr || len == 0) {
                return;
            }
            auto *queueData = static_cast<const QueueStatusData *>(static_cast<const void *>(data));
            if (len != sizeof(QueueStatusData)) {
                SECURITY_LOG_ERROR("ProcessDeviceStatusReceived, invalid input");
                return;
            }
            auto processor = DeviceStatusControlBlock::GetInstance().GetStateReceiver();
            if (processor == nullptr) {
                SECURITY_LOG_ERROR("ProcessDeviceStatusReceiver, invalid queue");
                return;
            }
            processor(&queueData->srcIdentity, queueData->status, queueData->level);
            delete queueData;
        };
        auto input = static_cast<uint8_t *>(static_cast<void *>(data));
        auto ret = QueueWork(queue, process, input, sizeof(QueueStatusData));
        if (ret != WORK_QUEUE_OK) {
            SECURITY_LOG_ERROR("ProcessDeviceStatusReceiver, QueueWork failed, ret is %{public}u", ret);
            delete data;
            return;
        }
    }
};

static bool MessengerConvertNodeToIdentity(const std::string &networkId, DeviceIdentify &devId)
{
    const auto &name = DeviceStatusControlBlock::GetInstance().GetPackageName();

    std::string udid;
    int32_t ret = DeviceManager::GetInstance().GetUdidByNetworkId(name, networkId, udid);
    if (ret != 0) {
        SECURITY_LOG_ERROR("MessengerConvertNodeToIdentity GetUdidByNetworkId failed = %{public}d", ret);
        return false;
    }
    auto size = (udid.size() < DEVICE_ID_MAX_LEN) ? udid.size() : DEVICE_ID_MAX_LEN;

    static_cast<void>(memset_s(&devId, sizeof(DeviceIdentify), 0, sizeof(DeviceIdentify)));
    if (memcpy_s(devId.identity, DEVICE_ID_MAX_LEN, udid.c_str(), size) != EOK) {
        SECURITY_LOG_ERROR("MessengerConvertNodeToIdentity memcpy error");
        return false;
    }
    if (devId.identity[0] == 0) {
        SECURITY_LOG_ERROR("MessengerConvertNodeToIdentity content error");
        return false;
    }
    devId.length = DEVICE_ID_MAX_LEN;
    return true;
}

static bool MessengerGetDeviceNodeBasicInfo(const DeviceIdentify &devId, DmDeviceInfo &info)
{
    const auto &name = DeviceStatusControlBlock::GetInstance().GetPackageName();

    std::vector<DmDeviceInfo> deviceList;
    int32_t ret = DeviceManager::GetInstance().GetTrustedDeviceList(name, "", deviceList);
    if (ret != 0) {
        SECURITY_LOG_ERROR("MessengerGetDeviceOnlineStatus GetTrustedDeviceList failed = %{public}d", ret);
        return false;
    }

    bool find = false;
    for (auto const &device : deviceList) {
        DeviceIdentify curr = {DEVICE_ID_MAX_LEN, {0}};
        bool convert = MessengerConvertNodeToIdentity(device.networkId, curr);
        if (convert != true) {
            continue;
        }

        if (IsSameDevice(&devId, &curr)) {
            find = true;
            info = device;
            break;
        }
    }
    return find;
}
} // namespace DeviceSecurityLevel
} // namespace Security
} // namespace OHOS

#ifdef __cplusplus
extern "C" {
#endif
using namespace OHOS::Security::DeviceSecurityLevel;

static void GetDeviceSecurityLevelByNetworkId(const OHOS::DistributedHardware::DmDeviceInfo &info, int32_t &level)
{
    const char pkgName[PKG_NAME_LEN + 1] = "ohos.dslm";
    int32_t ret = DeviceManager::GetInstance().GetDeviceSecurityLevel(pkgName, info.networkId, level);
    SECURITY_LOG_INFO("GetDeviceSecurityLevelByNetworkId ret = %{public}d, level = %{public}d", ret, level);

    const char *jsonString = static_cast<const char *>(info.extraData.c_str());
    DslmJsonHandle handle = DslmCreateJson(jsonString);
    if (handle == nullptr) {
        return;
    }
    uint32_t osType = static_cast<uint32_t>(DslmGetJsonFieldInt(handle, "OS_TYPE"));
    SECURITY_LOG_INFO("device type is %{public}d", osType);
    if (info.deviceTypeId == LAPTOP_TYPE || info.deviceTypeId == SMART_DISPLAY_TYPE) {
        osType = DEFAULT_TYPE;
        SECURITY_LOG_INFO("deviceTypeId is %{public}d", info.deviceTypeId);
    }
    level = static_cast<int32_t>(static_cast<uint32_t>(level) | (osType << TYPE_PLACE));
    DslmDestroyJson(handle);
    return;
}

bool InitDeviceStatusManager(WorkQueue *queue, const char *pkgName, DeviceStatusReceiver deviceStatusReceiver)
{
    if (queue == nullptr || pkgName == nullptr || deviceStatusReceiver == nullptr) {
        return false;
    }
    const std::string name(pkgName);
    DeviceStatusControlBlock::GetInstance().Reset(name, queue, deviceStatusReceiver);

    auto callback = std::make_shared<DslmDeviceState>();
    if (callback == nullptr) {
        SECURITY_LOG_ERROR("DslmDeviceState alloc failed");
        return false;
    }

    int tryTimes = 0;
    int32_t ret = 0;
    do {
        tryTimes++;
        ret = DeviceManager::GetInstance().InitDeviceManager(name, callback);
        if (ret != 0) {
            SECURITY_LOG_ERROR("InitDeviceManager failed = %{public}d", ret);
            MessengerSleep(1); // sleep 1 second and try again
            continue;
        }
    } while (ret != 0 && tryTimes < MAX_TRY_TIMES);

    if (ret != 0) {
        SECURITY_LOG_ERROR("InitDeviceManager RegNodeDeviceStateCb failed = %{public}d", ret);
        return false;
    }

    auto process = [](const DeviceIdentify *devId, int32_t level, void *para) -> int32_t {
        static_cast<void>(para);
        DslmDeviceState::ProcessDeviceStatusReceiver(devId, DslmDeviceState::State::EVENT_NODE_STATE_ONLINE, level);
        return 0;
    };

    MessengerForEachDeviceProcess(process, nullptr);
    SECURITY_LOG_INFO("InitDeviceManager RegNodeDeviceStateCb success");
    return true;
}

bool DeInitDeviceStatusManager(void)
{
    const auto &name = DeviceStatusControlBlock::GetInstance().GetPackageName();
    static_cast<void>(DeviceManager::GetInstance().UnInitDeviceManager(name));

    SECURITY_LOG_INFO("DeInitDeviceManager UnregNodeDeviceStateCb success");
    return true;
}

bool MessengerGetDeviceOnlineStatus(const DeviceIdentify *devId, int32_t *level)
{
    if (devId == nullptr) {
        return false;
    }

    DmDeviceInfo info;
    bool result = MessengerGetDeviceNodeBasicInfo(*devId, info);
    if (result == true && level != nullptr) {
        GetDeviceSecurityLevelByNetworkId(info, *level);
    }
    return result;
}

bool MessengerGetSelfDeviceIdentify(DeviceIdentify *devId, int32_t *level)
{
    if (devId == nullptr || level == nullptr) {
        return false;
    }

    const auto &name = DeviceStatusControlBlock::GetInstance().GetPackageName();

    DmDeviceInfo info;
    int32_t ret = DeviceManager::GetInstance().GetLocalDeviceInfo(name, info);
    if (ret != 0) {
        SECURITY_LOG_ERROR("MessengerGetSelfDeviceIdentify GetLocalNodeDeviceInfo failed = %{public}d", ret);
        return false;
    }

    bool convert = MessengerConvertNodeToIdentity(info.networkId, *devId);
    if (convert == false) {
        return false;
    }

    GetDeviceSecurityLevelByNetworkId(info, *level);
    *level = static_cast<int32_t>(static_cast<uint32_t>(*level) & 0xFF);

    uint32_t maskId = MaskDeviceIdentity((const char *)&devId->identity[0], DEVICE_ID_MAX_LEN);
    SECURITY_LOG_DEBUG("MessengerGetSelfDeviceIdentify device %{public}x***, level is %{public}d", maskId, *level);
    return true;
}

void MessengerForEachDeviceProcess(const DeviceProcessor processor, void *para)
{
    if (processor == nullptr) {
        return;
    }

    const auto &name = DeviceStatusControlBlock::GetInstance().GetPackageName();

    std::vector<DmDeviceInfo> deviceList;
    int32_t ret = DeviceManager::GetInstance().GetTrustedDeviceList(name, "", deviceList);
    if (ret != 0) {
        SECURITY_LOG_ERROR("MessengerForEachDeviceProcess GetTrustedDeviceList failed = %{public}d", ret);
        return;
    }

    for (auto const &device : deviceList) {
        DeviceIdentify curr = {DEVICE_ID_MAX_LEN, {0}};
        bool convert = MessengerConvertNodeToIdentity(device.networkId, curr);
        int32_t level = 0;
        GetDeviceSecurityLevelByNetworkId(device, level);

        if (convert == true) {
            processor(&curr, level, para);
        }
    }
}

bool MessengerGetNetworkIdByDeviceIdentify(const DeviceIdentify *devId, char *networkId, uint32_t len)
{
    if (devId == nullptr || networkId == nullptr || len == 0) {
        return false;
    }
    DmDeviceInfo info;
    bool result = MessengerGetDeviceNodeBasicInfo(*devId, info);
    if (result != true) {
        return false;
    }

    int32_t ret = strcpy_s(networkId, len, info.networkId);
    if (ret != EOK) {
        SECURITY_LOG_ERROR("MessengerGetNetworkIdByDeviceIdentify strcpy_s error");
        return false;
    }
    return true;
}

bool MessengerGetDeviceIdentifyByNetworkId(const char *networkId, DeviceIdentify *devId)
{
    if (networkId == nullptr || devId == nullptr) {
        SECURITY_LOG_ERROR("MessengerGetDeviceIdentifyByNetworkId input error");
        return false;
    }
    return MessengerConvertNodeToIdentity(networkId, *devId);
}
#ifdef __cplusplus
}
#endif
