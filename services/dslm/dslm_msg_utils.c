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

#include "dslm_msg_utils.h"

#include <string.h>

#include "securec.h"

#include "device_security_defines.h"
#include "dslm_core_defines.h"
#include "dslm_credential.h"
#include "utils_base64.h"
#include "utils_hexstring.h"
#include "utils_json.h"
#include "utils_log.h"
#include "utils_mem.h"

#define CHALLENGE_STRING_LENGTH 32

static uint8_t *GenerateSecInfoResponseJson(uint64_t challenge, const DslmCredBuff *cred)
{
    uint8_t *credBase64Str = NULL;
    uint8_t *out = NULL;

    DslmJsonHandle head = DslmCreateJson(NULL);
    if (head == NULL) {
        return NULL;
    }

    DslmJsonHandle body = DslmCreateJson(NULL);
    if (body == NULL) {
        DslmDestroyJson(head);
        return NULL;
    }

    DslmAddFieldIntToJson(head, FIELD_MESSAGE, MSG_TYPE_DSLM_CRED_RESPONSE);
    DslmAddFieldJsonToJson(head, FIELD_PAYLOAD, body);

    DslmAddFieldIntToJson(body, FIELD_VERSION, (int32_t)GetCurrentVersion());
    DslmAddFieldIntToJson(body, FIELD_CRED_TYPE, cred->type);

    char challengeStr[CHALLENGE_STRING_LENGTH] = {0};
    char *nonce = &challengeStr[0];
    DslmByteToHexString((uint8_t *)&challenge, sizeof(challenge), (uint8_t *)nonce, CHALLENGE_STRING_LENGTH);
    challengeStr[CHALLENGE_STRING_LENGTH - 1] = 0;
    DslmAddFieldStringToJson(body, FIELD_CHALLENGE, nonce);

    credBase64Str = Base64EncodeApp(cred->credVal, cred->credLen);
    // it is ok when credBase64Str is NULL
    DslmAddFieldStringToJson(body, FIELD_CRED_INFO, (char *)credBase64Str);
    out = (uint8_t *)DslmConvertJsonToString(head);

    if (head != NULL) {
        DslmDestroyJson(head);
        body = NULL; // no need to free body
    }

    if (body != NULL) {
        DslmDestroyJson(body);
    }
    if (credBase64Str != NULL) {
        FREE(credBase64Str);
    }
    return out;
}

static uint8_t *GenerateSecInfoRequestJson(uint64_t challenge)
{
    DslmJsonHandle head = DslmCreateJson(NULL);
    if (head == NULL) {
        return NULL;
    }
    DslmJsonHandle body = DslmCreateJson(NULL);
    if (body == NULL) {
        DslmDestroyJson(head);
        return NULL;
    }

    char challengeStr[CHALLENGE_STRING_LENGTH] = {0};
    char *nonce = &challengeStr[0];
    DslmByteToHexString((uint8_t *)&challenge, sizeof(challenge), (uint8_t *)nonce, CHALLENGE_STRING_LENGTH);
    challengeStr[CHALLENGE_STRING_LENGTH - 1] = 0;

    DslmAddFieldIntToJson(head, FIELD_MESSAGE, MSG_TYPE_DSLM_CRED_REQUEST);
    DslmAddFieldJsonToJson(head, FIELD_PAYLOAD, body);
    DslmAddFieldIntToJson(body, FIELD_VERSION, (int32_t)GetCurrentVersion());

    DslmAddFieldStringToJson(body, FIELD_CHALLENGE, nonce);

    CredType credTypeArray[MAX_CRED_ARRAY_SIZE] = {0};
    int32_t arraySize = GetSupportedCredTypes(credTypeArray, MAX_CRED_ARRAY_SIZE);
    DslmAddFieldIntArrayToJson(body, FIELD_SUPPORT, (const int32_t *)credTypeArray, arraySize);

    uint8_t *out = (uint8_t *)DslmConvertJsonToString(head);

    if (head != NULL) {
        DslmDestroyJson(head);
        body = NULL; // no need to free body
    }

    if (body != NULL) {
        DslmDestroyJson(body);
    }
    return out;
}

int32_t BuildDeviceSecInfoRequest(uint64_t challenge, MessageBuff **msg)
{
    if ((msg == NULL) || (*msg != NULL)) {
        return ERR_INVALID_PARA;
    }

    MessageBuff *out = MALLOC(sizeof(MessageBuff));
    if (out == NULL) {
        return ERR_NO_MEMORY;
    }
    (void)memset_s(out, sizeof(MessageBuff), 0, sizeof(MessageBuff));

    out->buff = GenerateSecInfoRequestJson(challenge);
    if (out->buff == NULL) {
        FREE(out);
        return ERR_JSON_ERR;
    }
    out->length = strlen((char *)out->buff) + 1;
    *msg = out;
    return SUCCESS;
}

int32_t ParseDeviceSecInfoRequest(const MessageBuff *msg, RequestObject *obj)
{
    if (msg == NULL || obj == NULL || msg->buff == NULL) {
        return ERR_INVALID_PARA;
    }
    SECURITY_LOG_DEBUG("ParseDeviceSecInfoRequest msg is %s", (char *)msg->buff);

    DslmJsonHandle handle = DslmCreateJson((const char *)msg->buff);
    if (handle == NULL) {
        return ERR_INVALID_PARA;
    }

    const char *nonceStr = DslmGetJsonFieldString(handle, FIELD_CHALLENGE);
    if (nonceStr == NULL) {
        DslmDestroyJson(handle);
        return ERR_NO_CHALLENGE;
    }

    int32_t ret = DslmHexStringToByte(nonceStr, strlen(nonceStr), (uint8_t *)&obj->challenge, sizeof(obj->challenge));
    if (ret != 0) {
        DslmDestroyJson(handle);
        return ERR_NO_CHALLENGE;
    }

    obj->version = (uint32_t)DslmGetJsonFieldInt(handle, FIELD_VERSION);
    obj->arraySize = DslmGetJsonFieldIntArray(handle, FIELD_SUPPORT, (int32_t *)obj->credArray, MAX_CRED_ARRAY_SIZE);

    DslmDestroyJson(handle);

    return SUCCESS;
}

int32_t BuildDeviceSecInfoResponse(uint64_t challenge, const DslmCredBuff *cred, MessageBuff **msg)
{
    if ((cred == NULL) || (msg == NULL) || (*msg != NULL)) {
        return ERR_INVALID_PARA;
    }
    MessageBuff *out = MALLOC(sizeof(MessageBuff));
    if (out == NULL) {
        return ERR_NO_MEMORY;
    }

    out->buff = GenerateSecInfoResponseJson(challenge, cred);
    if (out->buff == NULL) {
        FREE(out);
        return ERR_JSON_ERR;
    }
    out->length = strlen((char *)out->buff) + 1;

    *msg = out;
    return SUCCESS;
}

int32_t ParseDeviceSecInfoResponse(const MessageBuff *msg, uint64_t *challenge, uint32_t *version, DslmCredBuff **cred)
{
    if (msg == NULL || challenge == NULL || version == NULL || cred == NULL) {
        return ERR_INVALID_PARA;
    }

    if (msg->buff == NULL || *cred != NULL) {
        return ERR_INVALID_PARA;
    }

    DslmJsonHandle handle = DslmCreateJson((const char *)msg->buff);
    if (handle == NULL) {
        return ERR_INVALID_PARA;
    }

    const char *nonceStr = DslmGetJsonFieldString(handle, FIELD_CHALLENGE);
    if (nonceStr == NULL) {
        DslmDestroyJson(handle);
        return ERR_NO_CHALLENGE;
    }
    uint64_t nonceNum = 0;
    int32_t ret = DslmHexStringToByte(nonceStr, strlen(nonceStr), (uint8_t *)&nonceNum, sizeof(uint64_t));
    if (ret != 0) {
        DslmDestroyJson(handle);
        return ERR_NO_CHALLENGE;
    }

    uint32_t type = (uint32_t)DslmGetJsonFieldInt(handle, FIELD_CRED_TYPE);
    uint32_t verNum = (uint32_t)DslmGetJsonFieldInt(handle, FIELD_VERSION);

    const char *credStr = DslmGetJsonFieldString(handle, FIELD_CRED_INFO);
    if (credStr == NULL) {
        DslmDestroyJson(handle);
        return ERR_NO_CRED;
    }

    uint8_t *credBuf = NULL;
    uint32_t credLen = (uint32_t)Base64DecodeApp((uint8_t *)credStr, &credBuf);
    if (credBuf == NULL) {
        DslmDestroyJson(handle);
        return ERR_NO_CRED;
    }

    DslmCredBuff *out = CreateDslmCred((CredType)type, credLen, credBuf);
    if (out == NULL) {
        DslmDestroyJson(handle);
        FREE(credBuf);
        return ERR_NO_MEMORY;
    }

    DslmDestroyJson(handle);
    FREE(credBuf);
    *version = verNum;
    *challenge = nonceNum;
    *cred = out;
    return SUCCESS;
}
