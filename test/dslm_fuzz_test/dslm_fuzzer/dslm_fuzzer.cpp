/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dslm_fuzzer.h"

#include "parcel.h"
#include "securec.h"
#include "string_ex.h"

#include "device_security_defines.h"
#include "device_security_info.h"
#include "device_security_level_callback_stub.h"
#include "dslm_core_process.h"
#include "dslm_credential.h"
#include "dslm_credential_utils.h"
#include "dslm_device_list.h"
#include "dslm_hidumper.h"
#include "dslm_hievent.h"
#include "dslm_messenger_wrapper.h"
#include "dslm_msg_serialize.h"
#include "dslm_msg_utils.h"
#include "dslm_rpc_process.h"
#include "dslm_service.h"
#include "hks_adapter.h"
#include "messenger_device_socket_manager.h"
#include "messenger_device_status_manager.h"
#include "utils_base64.h"
#include "utils_mem.h"
#include "utils_tlv.h"
#include "hks_param.h"
#include "hks_type.h"
#include "external_interface_adapter.h"
#include "hks_adapter.h"
#include "dslm_test_link.h"

#define CNT 1000
#define ITEMSTATE 4
#define MAX_ENTRY 8
#define MAX_MALLOC_LEN (1 * 1024 * 1024)
#define INIT_MAX 3
#define PTR_LEN 4
#define BLOB_SIZE 5
#define DUMP_NUM 2

extern "C" int32_t OnPeerMsgReceived(const DeviceIdentify *devId, const uint8_t *msg, uint32_t len);
extern "C" int32_t OnSendResultNotifier(const DeviceIdentify *devId, uint64_t transNo, uint32_t result);
extern "C" bool MessengerGetDeviceOnlineStatus(const DeviceIdentify *devId, int32_t *level);

namespace OHOS {
namespace Security {
namespace DeviceSecurityLevel {
namespace {
const uint8_t mockBuffer[DEVICE_ID_MAX_LEN] = {0};

DslmService g_dslmService(DEVICE_SECURITY_LEVEL_MANAGER_SA_ID, true);
static int32_t g_init = 0;

const char *g_cred = "eyJ0eXAiOiAiRFNMIn0=.eyJ0eXBlIjogImRlYnVnIiwgIm1h"
                     "bnVmYWN0dXJlIjogIk9IT1MiLCAiYnJhbmQiOiAicmszNTY4IiwgIm1vZGVsIjog"
                     "InJrMzU2OCIsICJzb2Z0d2FyZVZlcnNpb24iOiAiMy4wLjAiLCAic2VjdXJpdHlM"
                     "ZXZlbCI6ICJTTDMiLCAic2lnblRpbWUiOiAiMjAyMjExMjYxNzMzNDMiLCAidmVy"
                     "c2lvbiI6ICIxLjAuMSJ9.MGUCMEPpiP8hOZlve/H81B7AvL4Fuwe8YYAdKckLEOc"
                     "EQKKTiNRM6irjXSwboMppAFNMSgIxAILC1S6KMp6Zp2ACppXF3j3fV0PBdLZOSO1"
                     "Lm9sqtdiJ5FidaAaMYlwdLMy3vfBeSg==.W3sidXNlclB1YmxpY0tleSI6ICJNSG"
                     "93RkFZSEtvWkl6ajBDQVFZSkt5UURBd0lJQVFFTEEySUFCQiszTHJWUU13cWlwc2"
                     "VnOUFBT0twMDJFeDNKOTJlUzdrK0k5cFJPWnVvOFZFQmVvbzF6Ris2MWhtVU5TMm"
                     "tjN0c3NTBVOExOT2pUamhUVGp2NW1CQjdBdnhnUDMwc3d3SDJ1dFVoczhGRzAwQU"
                     "xsOUZuWFZsSmNpaGo5SGJ0WjNnPT0iLCAic2lnbmF0dXJlIjogIk1HVUNNUUNIUV"
                     "dzYXNYc1NpL3dJUThmWW5PRlhsaWhTem5ETG1RSjBEOGp4U3RVM2Z2bk4xZkgzUV"
                     "JJUnRzM1lIK293bE9zQ01EY2pJU0pOK2J6M2g0VUU2UTl1NW92K0RHcFRHL2Vqd0"
                     "xTU2FyMHJzZ09ZSVovODdRb0p2QllaM2hFamlDcWQ1dz09In0sIHsidXNlclB1Ym"
                     "xpY0tleSI6ICJNSG93RkFZSEtvWkl6ajBDQVFZSkt5UURBd0lJQVFFTEEySUFCRk"
                     "RMR2M4YlhQT2RBYVpLN25OQUZrYkRoVHBwcTNaQW92T3FKZDJKMy9vdW14eG84Qn"
                     "Q4ZGhiQjBtR3FHQjE4V0hpTkUwNFRCS1RvYU9lQ3NtZEZ0dUtXcEtwZEtIRDdGL3"
                     "YvaXhxbHd6MnMzSk9scFQ3dUQzbjNieHFaVHJzMnFnPT0iLCAic2lnbmF0dXJlIj"
                     "ogIk1HUUNNSGthczBkZDgwUVpiQVB6eElhMXhBYmd1WlhwNjU0T29rL2VGR2M0ek"
                     "tLczlqYjVKK24waHJDcytoa0JrR0N0b3dJd1pYcGlYUjRiS1h3RUlTZmdpSDI4dk"
                     "ZaZVQxcFJCcnFkSHd2d3ErOXcrdWQzMkhkeC90YWhHZ1kySHVZZFNHZDUifSwgey"
                     "J1c2VyUHVibGljS2V5IjogIk1Ib3dGQVlIS29aSXpqMENBUVlKS3lRREF3SUlBUU"
                     "VMQTJJQUJEVTVaYkhESGl2TGgzRFN4UDEwbGluL2FIMXJabG1XMnBMZ3JwZ3BiL0"
                     "lnWkkrMzJyWC9QdFhURGZWYmVyRG93VkhURTJ0MFZMNzlnQ2wrbUVCL1dBeDVEZW"
                     "1lamlMNTJ6S0l6M2RTNWJxVHdYVExvRHZTSml3Z3dxYmZPMEZtK3c9PSIsICJzaW"
                     "duYXR1cmUiOiAiTUdRQ01HWlI0MUdsd1RnL0xUMGtFT3lTZnRHTDBlV04zb2dXdF"
                     "o0NTZ2VkdqMm56WnhsamFlN2pveWw4cWZHNjZSTUdTQUl3S2M3V2VpQ1c1UlFGSj"
                     "ROWitSRUErNVNpMHhRVFpOdzlhb1FTUG5LVTA0L2ZIWUhkVERNWitncUY3U3RJMD"
                     "ZTbSJ9XQ==";

typedef struct {
    uint32_t code;
    void (*process)(DeviceIdentify *deviceIdentify, Parcel &parcel);
} DslmFuzzerTable;

void OnPeerMsgReceivedTest1(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint8_t jsonString[] = R"(
            {"message":0, "payload":111}
            )";
    (void)OnPeerMsgReceived(deviceIdentify, jsonString, sizeof(jsonString));
}

void OnPeerMsgReceivedTest2(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint8_t jsonString[] = R"(
            {"message":1, "payload":{"challenge":"0102030405060708"}}
            )";
    (void)OnPeerMsgReceived(deviceIdentify, jsonString, sizeof(jsonString));
}

void OnPeerMsgReceivedTest3(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint8_t jsonString[] = R"(
            {"message":2, "payload":222}
            )";
    (void)OnPeerMsgReceived(deviceIdentify, jsonString, sizeof(jsonString));

    uint32_t len = 0;
    (void)OnPeerMsgReceived(deviceIdentify, jsonString, len);
}

void ServiceTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    if (g_init < INIT_MAX) {
        uint8_t jsonString[] = R"(
            {"message":0, "payload":111}
            )";
        uint64_t transNo = 1;
        static DeviceIdentify self = {0, {0}};
        int32_t level;
        (void)InitService();
        (void)MessengerGetDeviceOnlineStatus(deviceIdentify, &level);
        (void)MessengerGetDeviceOnlineStatus(nullptr, &level);
        (void)MessengerGetSelfDeviceIdentify(&self, &level);
        (void)MessengerGetSelfDeviceIdentify(nullptr, &level);
        MessengerSendMsgTo(transNo, deviceIdentify, jsonString, sizeof(jsonString));
        g_init++;
    }
}

void OnSendResultNotifierTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    const DeviceIdentify identify = {DEVICE_ID_MAX_LEN, {0}};
    (void)OnSendResultNotifier(&identify, 0, SUCCESS);
}

void VerifyDslmCredentialTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    DslmCredInfo info;
    AttestationList list;
    memset_s(&info, sizeof(DslmCredInfo), 0, sizeof(DslmCredInfo));
    memset_s(&list, sizeof(AttestationList), 0, sizeof(AttestationList));

    (void)VerifyDslmCredential(g_cred, &info, &list);
    (void)VerifyDslmCredential(nullptr, &info, &list);
    FreeAttestationList(&list);
}

void MessengerSendMsgToTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint8_t jsonString[] = R"(
            {"message":0, "payload":111}
            )";
    uint64_t transNo = 1;
    static DeviceIdentify self = {0, {0}};
    int32_t level;
    (void)MessengerGetSelfDeviceIdentify(&self, &level);
    MessengerSendMsgTo(transNo, deviceIdentify, jsonString, sizeof(jsonString));
    MessengerSendMsgTo(transNo, nullptr, jsonString, sizeof(jsonString));
}

void OnPeerStatusReceiverTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint32_t status = parcel.ReadUint32() % 2;
    int32_t level = -1;
    (void)MessengerGetDeviceOnlineStatus(deviceIdentify, &level);
    (void)MessengerGetDeviceOnlineStatus(nullptr, &level);
    (void)OnPeerStatusReceiver(deviceIdentify, status, level);
}

void DslmDumperTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    int fd = 3511;
    uint32_t status = parcel.ReadUint32() % PTR_LEN;
    std::vector<std::u16string> args;
    if (status == 0) {
        g_dslmService.Dump(0, args);
    } else if (status == 1) {
        args.emplace_back(Str8ToStr16("-h"));
    } else if (status == DUMP_NUM) {
        args.emplace_back(Str8ToStr16("-l"));
    } else {
        args.emplace_back(Str8ToStr16("-t"));
    }
    g_dslmService.Dump(fd, args);
    DslmDumper(-1);
}

void VerifyOhosDslmCredTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    const DeviceIdentify device = {DEVICE_ID_MAX_LEN, {'a', 'b', 'c', 'd', 'e', 'f', 'a', 'b'}};
    uint64_t challenge = 0x1234;
    uint8_t info[] = {'a', 'b', 'c', 'd', 1, 3, 5, 7, 9};
    DslmCredBuff cred = {CRED_TYPE_STANDARD, 9, info};
    DslmCredInfo credInfo;
    (void)memset_s(&credInfo, sizeof(DslmCredInfo), 0, sizeof(DslmCredInfo));
    (void)VerifyOhosDslmCred(&device, challenge, &cred, &credInfo);

    cred.type = CRED_TYPE_LARGE;
    (void)VerifyOhosDslmCred(&device, challenge, &cred, &credInfo);
    (void)VerifyOhosDslmCred(nullptr, challenge, &cred, &credInfo);
}

void RequestDeviceSecurityInfoTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    DeviceIdentify device = {DEVICE_ID_MAX_LEN, {0}};
    int32_t level = 0;
    DeviceSecurityInfo *info = nullptr;
    (void)RequestDeviceSecurityInfo(&device, nullptr, &info);
    (void)GetDeviceSecurityLevelValue(info, &level);
    FreeDeviceSecurityInfo(info);
}

void GetPeerDeviceOnlineStatusTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    int32_t level;
    (void)GetPeerDeviceOnlineStatus(deviceIdentify, &level);
    (void)GetPeerDeviceOnlineStatus(nullptr, nullptr);
}

void Base64EncodeAppTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint8_t src[] = {'a', 'b', 'c', 'd', '\0'};
    uint32_t maxStrLen = 4;

    (void)Base64EncodeApp(nullptr, sizeof(src));
    (void)Base64EncodeApp(src, maxStrLen);
}

void SerializeTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint32_t size = 0;
    int i = 0;
    TlvCommon tlvs[MAX_ENTRY];

    uint8_t buff[MAX_ENTRY * sizeof(TlvCommon)] = {0};
    size = 0;
    (void)memset_s(&buff[0], sizeof(buff), 0, sizeof(buff));
    (void)memset_s(&tlvs[0], sizeof(tlvs), 0, sizeof(tlvs));

    for (i = 0; i < MAX_ENTRY; i++) {
        TlvCommon *ptr = (TlvCommon *)tlvs + i;
        ptr->tag = 0x105;
        ptr->len = PTR_LEN;
        ptr->value = nullptr;
    }

    (void)Serialize(tlvs, MAX_ENTRY, buff, sizeof(buff), &size);
    (void)Serialize(nullptr, MAX_ENTRY, buff, sizeof(buff), &size);
}

void BufferToHksCertChainTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint8_t buff[8];
    uint32_t len = 8;
    uint8_t *data;
    memset_s(buff, sizeof(buff), 'c', sizeof(buff));
    TlvCommon *ptr = (TlvCommon *)buff;
    ptr->tag = 0x110;
    ptr->len = PTR_LEN;
    struct HksCertChain chain;
    memset_s(&chain, sizeof(struct HksCertChain), 0, sizeof(struct HksCertChain));
    CredType credType = CRED_TYPE_STANDARD;

    (void)BufferToHksCertChain(buff, len, &chain);
    (void)BufferToHksCertChain(nullptr, len, &chain);
    (void)CreateDslmCred(credType, len, buff);
    (void)CreateDslmCred(credType, len, nullptr);
    (void)HksCertChainToBuffer(&chain, &data, &len);
    (void)HksCertChainToBuffer(nullptr, &data, &len);
}

void DestroyHksCertChainTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    struct HksCertChain *chain = (struct HksCertChain *)MALLOC(sizeof(struct HksCertChain));
    struct HksBlob *blob = (struct HksBlob *)MALLOC(sizeof(struct HksBlob));
    blob->size = BLOB_SIZE;
    blob->data = nullptr;
    chain->certs = blob;
    chain->certsCount = 1;

    DestroyHksCertChain(chain);
    DestroyHksCertChain(nullptr);
}

void DefaultInitDslmCredTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    DslmCredInfo credInfo;
    DefaultInitDslmCred(&credInfo);
}

void BuildDeviceSecInfoResponseTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint64_t challenge = 0x1234;
    uint8_t info[] = {'a', 'b', 'c', 'd', 1, 3, 5, 7, 9};
    DslmCredBuff cred = {CRED_TYPE_STANDARD, 9, info};
    MessageBuff *msg = NULL;
    (void)BuildDeviceSecInfoResponse(challenge, &cred, &msg);
    (void)BuildDeviceSecInfoResponse(challenge, nullptr, &msg);
}

void ReportHiEventTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    uint32_t errorType = ERR_MSG_NOT_INIT;
    ReportHiEventServiceStartFailed(errorType);

    DslmDeviceInfo *info = (DslmDeviceInfo *)MALLOC(sizeof(DslmDeviceInfo));
    (void)memset_s(info, sizeof(DslmDeviceInfo), 0, sizeof(DslmDeviceInfo));
    info->lastRequestTime = 10U;
    ReportHiEventInfoSync(nullptr);
    ReportHiEventInfoSync(info);
    ReportHiEventAppInvoke(nullptr);
    FREE(info);
    info = nullptr;
}

void ConstructHksCertChainTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    struct HksCertChain *chain = nullptr;

    {
        (void)ConstructHksCertChain(&chain, nullptr);
    }

    {
        struct HksCertChainInitParams param;
        param.certChainExist = false;
        param.certCountValid = true;
        param.certDataExist = true;

        (void)ConstructHksCertChain(&chain, &param);
    }

    {
        struct HksCertChainInitParams param;
        param.certChainExist = true;
        param.certCountValid = false;
        param.certDataExist = true;

        (void)ConstructHksCertChain(&chain, &param);
    }

    {
        struct HksCertChainInitParams param;
        param.certChainExist = true;
        param.certCountValid = true;
        param.certDataExist = false;

        (void)ConstructHksCertChain(&chain, &param);
    }
}

void FillHksParamSetTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    struct HksParamSet *paramSet = nullptr;
    struct HksParam *param = nullptr;
    int32_t paramNums = 0;
    (void)FillHksParamSet(&paramSet, param, paramNums);
}

void HksGenerateKeyAdapterTest(DeviceIdentify *deviceIdentify, Parcel &parcel)
{
    const HksBlob *keyAlias1 = nullptr;

    (void)HksGenerateKeyAdapter(keyAlias1);
}

DslmFuzzerTable g_fuzzerTable[] = {{0, OnPeerMsgReceivedTest1}, {1, OnPeerMsgReceivedTest2},
    {2, OnPeerMsgReceivedTest3}, {3, ServiceTest}, {4, OnSendResultNotifierTest}, {5, VerifyDslmCredentialTest},
    {6, MessengerSendMsgToTest}, {7, OnPeerStatusReceiverTest}, {8, DslmDumperTest}, {9, VerifyOhosDslmCredTest},
    {10, RequestDeviceSecurityInfoTest}, {11, GetPeerDeviceOnlineStatusTest}, {12, Base64EncodeAppTest},
    {13, SerializeTest}, {14, BufferToHksCertChainTest}, {15, DestroyHksCertChainTest}, {16, DefaultInitDslmCredTest},
    {17, BuildDeviceSecInfoResponseTest}, {18, ReportHiEventTest}, {19, FillHksParamSetTest},
    {20, HksGenerateKeyAdapterTest}, {21, ConstructHksCertChainTest}};

void OnPeerMsgReceivedFuzzer(Parcel &parcel)
{
    SECURITY_LOG_INFO("begin");
    DeviceIdentify deviceIdentify = {};
    deviceIdentify.length = DEVICE_ID_MAX_LEN;
    const uint8_t *buffer = parcel.ReadBuffer(DEVICE_ID_MAX_LEN);
    if (buffer != nullptr) {
        (void)memcpy_s(deviceIdentify.identity, DEVICE_ID_MAX_LEN, buffer, DEVICE_ID_MAX_LEN);
    }

    static int cnt = 0;
    cnt++;
    if (cnt <= CNT) {
        DslmDeviceInfo *info = CreatOrGetDslmDeviceInfo(&deviceIdentify);
        if (info != nullptr) {
            info->machine.currState = parcel.ReadUint32() % ITEMSTATE;
        }
    }

    uint32_t a = parcel.ReadUint32() % 22;
    for (uint32_t i = 0; i < sizeof(g_fuzzerTable) / sizeof(DslmFuzzerTable); ++i) {
        if (g_fuzzerTable[i].code == a) {
            g_fuzzerTable[i].process(&deviceIdentify, parcel);
        }
    }

    SECURITY_LOG_INFO("end");
}

void OnRemoteRequestFuzzer(Parcel &parcel)
{
    SECURITY_LOG_INFO("begin");
    MessageParcel data;
    MessageParcel reply;
    MessageOption option;

    data.WriteInterfaceToken(IDeviceSecurityLevel::GetDescriptor());

    /* DeviceIdentify */
    data.WriteUint32(parcel.ReadUint32());
    const uint8_t *buffer = parcel.ReadBuffer(DEVICE_ID_MAX_LEN);
    if (buffer == nullptr) {
        data.WriteBuffer(mockBuffer, DEVICE_ID_MAX_LEN);
    } else {
        data.WriteBuffer(buffer, DEVICE_ID_MAX_LEN);
    }

    /* option */
    data.WriteUint64(parcel.ReadUint64());
    data.WriteUint32(parcel.ReadUint32());
    data.WriteUint32(parcel.ReadUint32());

    sptr<IRemoteObject> callback = new (std::nothrow) DeviceSecurityLevelCallbackStub(
        [](uint32_t code, MessageParcel &data, uint32_t &cookie, uint32_t &result, uint32_t &level) {
            SECURITY_LOG_INFO("DeviceSecurityLevelCallbackStub called");
            return 0;
        },
        [](uint32_t cookie, uint32_t result, uint32_t level) {
            SECURITY_LOG_INFO("DeviceSecurityLevelCallbackStub called");
            return 0;
        });
    /* callback */
    data.WriteRemoteObject(callback);
    /* cookie */
    data.WriteUint32(parcel.ReadUint32());

    g_dslmService.OnRemoteRequest(parcel.ReadUint32(), data, reply, option);
    SECURITY_LOG_INFO("end");
}

void UtServerOnShutdownFuzz()
{
    DeviceIdentify idt {.length = 1, .identity = {'a'}};

    char dummy[] = {'a', 0};
    PeerSocketInfo peer = {.name = dummy, .networkId = dummy};
    UtServerOnBind(1, peer);
    UtClientOnBind(1, &idt);

    UtCreateOrRestartSocketCloseTimerWithLock(0);
    UtCreateOrRestartSocketCloseTimerWithLock(1);

    UtServerOnBytes(1, dummy, 1);
    UtClientOnBytes(1, dummy, 1);
}

void DslmFuzzTest(const uint8_t *data, size_t size)
{
    Parcel parcel;
    parcel.WriteBuffer(data, size);
    parcel.RewindRead(0);
    if (parcel.ReadBool()) {
        OnPeerMsgReceivedFuzzer(parcel);
    } else {
        OnRemoteRequestFuzzer(parcel);
    }
    UtServerOnShutdownFuzz();
}
} // namespace
} // namespace DeviceSecurityLevel
} // namespace Security
} // namespace OHOS

/* Fuzzer entry point */
extern "C" int32_t LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int initCount = 0;
    if (initCount == 0) {
        const ProcessDslmCredFunctions func = {
            .initFunc = InitOhosDslmCred,
            .requestFunc = RequestOhosDslmCred,
            .verifyFunc = VerifyOhosDslmCred,
            .credTypeCnt = 2,
            .credTypeArray = {CRED_TYPE_STANDARD, CRED_TYPE_SMALL},
        };
        InitDslmCredentialFunctions(&func);
        atexit(UnInitService);
        initCount = 1;
    }
    OHOS::Security::DeviceSecurityLevel::DslmFuzzTest(data, size);
    return 0;
}
