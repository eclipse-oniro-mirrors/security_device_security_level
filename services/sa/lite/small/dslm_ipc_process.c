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

#include "dslm_ipc_process.h"

#include "dslm_callback_info.h"
#include "dslm_core_process.h"
#include "ipc_skeleton.h"
#include "securec.h"
#include "utils_dslm_list.h"
#include "utils_log.h"
#include "utils_mem.h"
#include "utils_mutex.h"

#define DFT_TIMEOUT 45
const uint32_t MAX_TIMEOUT = 60;
const uint32_t MIN_TIMEOUT = 1;
const uint32_t WARNING_GATE = 64;
const uint32_t COOKIE_SHIFT = 32;

static DslmRemoteStubList *GetRemoteStubList(void)
{
    static DslmRemoteStubListNode head = {
        .node = INIT_LIST(head.node),
        .key = 0,
        .reply = NULL
    };
    static Mutex mutex = INITED_MUTEX;
    static DslmRemoteStubList stubList = { .head = &head, .size = 0, .mutex = &mutex};
    return &stubList;
}

static inline void SetRemoteStubStatus(IpcIo *reply, int32_t status)
{
    if (reply == NULL) {
        SECURITY_LOG_ERROR("unexpected input, reply is NULL");
        return;
    }
    WriteInt32(reply, status);
}

static int32_t DslmGetRequestFromParcel(IpcIo *req, DeviceIdentify *identity,
    RequestOption *option, uint32_t *cookie)
{
    ReadUint32(req, &identity->length);
    uint32_t *dataRead = ReadBuffer(req, DEVICE_ID_MAX_LEN);
    if (dataRead == NULL) {
        SECURITY_LOG_ERROR("unexpected input, buffer error");
        return ERR_INVALID_PARA;
    }
    if (memcpy_s(identity->identity, DEVICE_ID_MAX_LEN, dataRead, DEVICE_ID_MAX_LEN) != EOK) {
        SECURITY_LOG_ERROR("unexpected input, buffer copy error");
        return ERR_INVALID_PARA;
    }
    FREE(dataRead);
    dataRead = NULL;

    ReadUint64(req, &option->challenge);
    ReadUint32(req, &option->timeout);
    if (option->timeout < MIN_TIMEOUT || option->timeout > MAX_TIMEOUT) {
        option->timeout = DFT_TIMEOUT;
    }
    ReadUint32(req, &option->extra);
    ReadUint32(req, cookie);
    if (*cookie == 0) {
        SECURITY_LOG_ERROR("unexpected input, cookie error");
        return ERR_INVALID_PARA;
    }

    return SUCCESS;
}

static BOOL DslmPushRemoteStub(uint32_t owner, uint32_t cookie, IpcIo *reply)
{
    if (GetRemoteStubList()->size > WARNING_GATE) {
        SECURITY_LOG_WARN("remote objects max warning");
    }
    uint64_t key = ((uint64_t)owner << COOKIE_SHIFT) | cookie;
    DslmRemoteStubListNode *item = (DslmRemoteStubListNode *)MALLOC(sizeof(DslmRemoteStubListNode));
    if (item == NULL) {
        SECURITY_LOG_ERROR("malloc failed, node is null");
        return false;
    }
    memset_s(item, sizeof(DslmRemoteStubListNode), 0, sizeof(DslmRemoteStubListNode));
    
    item->key = key;
    item->reply = reply;
    LockMutex(GetRemoteStubList()->mutex);
    AddListNode(&item->node, &GetRemoteStubList()->head->node);
    GetRemoteStubList()->size++;
    UnlockMutex(GetRemoteStubList()->mutex);
    return true;
}

static IpcIo *DslmPopRemoteStub(uint32_t owner, uint32_t cookie)
{
    IpcIo *reply = NULL;
    ListNode *node = NULL;
    ListNode *temp = NULL;

    LockMutex(GetRemoteStubList()->mutex);
    uint64_t key = ((uint64_t)owner << COOKIE_SHIFT) | cookie;
    FOREACH_LIST_NODE_SAFE (node, &GetRemoteStubList()->head->node, temp) {
        DslmRemoteStubListNode *item = LIST_ENTRY(node, DslmRemoteStubListNode, node);
        if (item->key == key) {
            SECURITY_LOG_INFO("pop remote stub");
            reply = item->reply;
            RemoveListNode(node);
            if (GetRemoteStubList()->size > 0) {
                GetRemoteStubList()->size--;
            } else {
                SECURITY_LOG_ERROR("list size is abnormal, size = %u", GetRemoteStubList()->size);
            }
            FREE(item);
            break;
        }
    }
    UnlockMutex(GetRemoteStubList()->mutex);
    return reply;
}

static void ProcessCallback(uint32_t owner, uint32_t cookie, uint32_t result, const DslmCallbackInfo *info)
{
    IpcIo *reply = NULL;
    if ((cookie == 0) || (info == NULL)) {
        return;
    }

    reply = DslmPopRemoteStub(owner, cookie);
    if (reply == NULL) {
        SECURITY_LOG_ERROR("no such remote stub");
        return;
    }

    WriteUint32(reply, result);
    WriteUint32(reply, info->level);
    if (info->extraBuff != NULL && info->extraLen != 0) {
        WriteUint32(reply, info->extraLen);
        WriteBuffer(reply, info->extraBuff, info->extraLen);
    }
    SECURITY_LOG_INFO("process callback succ");
}

int32_t DslmProcessGetDeviceSecurityLevel(IUnknown *iUnknown, IpcIo *req, IpcIo *reply)
{
    DeviceIdentify identity;
    RequestOption option;
    uint32_t cookie;

    int32_t ret = DslmGetRequestFromParcel(req, &identity, &option, &cookie);
    if (ret != SUCCESS) {
        SECURITY_LOG_ERROR("DslmGetRequestFromParcel failed, ret = %d", ret);
        return ret;
    }

    uint64_t owner = GetCallingPid();
    DslmPushRemoteStub(owner, cookie, reply);
    ret = OnRequestDeviceSecLevelInfo(&identity, &option, owner, cookie, ProcessCallback);
    if (ret != SUCCESS) {
        SECURITY_LOG_ERROR("OnRequestDeviceSecLevelInfo failed, ret = %d", ret);
        SetRemoteStubStatus(reply, ret);
        DslmPopRemoteStub(owner, cookie);
        return ret;
    }
    return SUCCESS;
}