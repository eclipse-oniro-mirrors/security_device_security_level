/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
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

#ifndef DSLM_SERVICE_H
#define DSLM_SERVICE_H

#include "iremote_stub.h"
#include "nocopyable.h"
#include "system_ability.h"

#include "idevice_security_level.h"

namespace OHOS {
namespace Security {
namespace DeviceSecurityLevel {
class DslmService : public SystemAbility, public IRemoteStub<IDeviceSecurityLevel> {
    DECLARE_SYSTEM_ABILITY(DslmService);

public:
    DISALLOW_COPY_AND_MOVE(DslmService);
    explicit DslmService(int32_t saId, bool runOnCreate);
    virtual ~DslmService() = default;
    void OnStart() override;
    void OnStop() override;
    int32_t Dump(int fd, const std::vector<std::u16string> &args) override;
    int32_t OnRemoteRequest(uint32_t code, MessageParcel &data, MessageParcel &reply, MessageOption &option) override;

private:
    int32_t ProcessGetDeviceSecurityLevel(MessageParcel &data, MessageParcel &reply);
};
} // DeviceSecurityLevel
} // Security
} // OHOS

#endif // DSLM_SERVICE_H