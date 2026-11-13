/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#include <new>

#include "device_manager.h"

#include "device_manager_mock.h"

namespace OHOS {
namespace DistributedHardware {
DeviceManagerMock::DeviceManagerMock()
{
    ON_CALL(*this, InitDeviceManager).WillByDefault([this](const std::string &, std::shared_ptr<DmInitCallback>) {
        return 0;
    });

    ON_CALL(*this, GetUdidByNetworkId)
        .WillByDefault([](const std::string &, const std::string &netWorkId, std::string &udid) {
            udid = netWorkId;
            return 0;
        });

    ON_CALL(*this, GetTrustedDeviceList)
        .WillByDefault([](const std::string &, const std::string &, std::vector<DmDeviceInfo> &deviceList) {
            DmDeviceInfo info1 {.networkId = {'a', 0}, .extraData = "{}"};
            DmDeviceInfo info2 {.networkId = {'b', 0}, .extraData = "{}"};
            DmDeviceInfo info3 {.networkId = {'c', 0}};
            DmDeviceInfo info4 {.networkId = {'d', 0}};
            deviceList.push_back(info1);
            deviceList.push_back(info2);
            deviceList.push_back(info3);
            deviceList.push_back(info4);
            return 0;
        });

    ON_CALL(*this, GetLocalDeviceInfo).WillByDefault([](const std::string &, DmDeviceInfo &deviceInfo) {
        DmDeviceInfo info1 {.networkId = {'a', 0}};
        deviceInfo = info1;
        return 0;
    });
}

DeviceManagerMock::~DeviceManagerMock()
{
}

DeviceManagerMock &DeviceManagerMock::Instance()
{
    static testing::NiceMock<DeviceManagerMock> *inst = new (std::nothrow) testing::NiceMock<DeviceManagerMock>();
    testing::Mock::AllowLeak(inst);
    return *inst;
}
} // namespace DistributedHardware
} // namespace OHOS
