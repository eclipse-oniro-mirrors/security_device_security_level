#/*
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

#include "external_interface.h"
#include "device_security_defines.h"

#include <securec.h>

#include "device_auth.h"
#include "hks_api.h"
#include "hks_param.h"
#include "utils_json.h"
#include "utils_log.h"
#include "utils_mem.h"

char g_keyData[] = "hi_key_data";

#define DSLM_HKS_TAG_ATTESTATION_CHALLENGE (HKS_TAG_TYPE_BYTES | 511)
#define DSLM_HKS_TAG_ATTESTATION_ID_SERIAL (HKS_TAG_TYPE_BYTES | 512)
#define DSLM_HKS_TAG_ATTESTATION_ID_UDID (HKS_TAG_TYPE_BYTES | 513)
#define DSLM_HKS_TAG_ATTESTATION_ID_SEC_LEVEL_INFO (HKS_TAG_TYPE_BYTES | 514)
#define DSLM_HKS_TAG_ATTESTATION_ID_VERSION_INFO (HKS_TAG_TYPE_BYTES | 515)
#define DSLM_HKS_INTERFACE_TRANS_PARAM_NUM 2

#define UDID_STRING_LENGTH 65
#define HICHIAN_INPUT_PARAM_STRING_LENGTH 512
#define DSLM_CERT_CHAIN_BASE_LENGTH 4096

static int32_t HksAttestKey2(const struct HksBlob *keyAlias, const struct HksParamSet *paramSet,
    struct HksCertChain *certChain)
{
    if ((keyAlias == NULL) || (paramSet == NULL) || (certChain == NULL)) {
        return HKS_ERROR_NOT_SUPPORTED;
    }
    if (certChain->certs == NULL || certChain->certs->data == NULL ||
        HksCheckParamSet(paramSet, paramSet->paramSetSize) != HKS_SUCCESS) {
        return HKS_ERROR_NOT_SUPPORTED;
    }

    uint8_t *tmp = certChain->certs->data;
    uint32_t offSet = 0;
    uint32_t dataLen;
    uint32_t totalSize = 0;
    for (uint32_t i = 0; i < paramSet->paramsCnt; i++) {
        switch (paramSet->params[i].tag) {
            case DSLM_HKS_TAG_ATTESTATION_CHALLENGE:
            case DSLM_HKS_TAG_ATTESTATION_ID_SERIAL:
            case DSLM_HKS_TAG_ATTESTATION_ID_UDID:
            case DSLM_HKS_TAG_ATTESTATION_ID_SEC_LEVEL_INFO:
            case DSLM_HKS_TAG_ATTESTATION_ID_VERSION_INFO:
                (void)memcpy_s(tmp + offSet, sizeof(uint32_t), &(paramSet->params[i].tag), sizeof(uint32_t));
                offSet += sizeof(uint32_t);
                dataLen = paramSet->params[i].blob.size;
                (void)memcpy_s(tmp + offSet, sizeof(uint32_t), &dataLen, sizeof(uint32_t));
                offSet += sizeof(uint32_t);
                (void)memcpy_s(tmp + offSet, dataLen, paramSet->params[i].blob.data, dataLen);
                offSet += dataLen;
                totalSize += (sizeof(uint32_t) * paramSet->paramsCnt + dataLen);
                break;
            default:
                break;
        }
    }

    certChain->certs->size = totalSize;
    certChain->certsCount = DSLM_HKS_INTERFACE_TRANS_PARAM_NUM;
    return HKS_SUCCESS;
}

static int32_t HksValidateCertChain2(const struct HksCertChain *certChain, struct HksParamSet *paramSetOut)
{
    if (certChain->certsCount != DSLM_HKS_INTERFACE_TRANS_PARAM_NUM) {
        return HKS_ERROR_INVALID_ARGUMENT;
    }

    uint8_t *tmp = certChain->certs->data;
    uint32_t offSet = 0;
    uint32_t dataLen;
    struct HksParam tmpParams[5] = {0};
    for (uint32_t i = 0; i < DSLM_HKS_INTERFACE_TRANS_PARAM_NUM; i++) {
        tmpParams[i].tag = *((uint32_t *)(&tmp[offSet]));
        offSet += sizeof(uint32_t);
        dataLen = *((uint32_t *)(&tmp[offSet]));
        tmpParams[i].blob.size = dataLen;
        offSet += sizeof(uint32_t);
        tmpParams[i].blob.data = (uint8_t *)MALLOC(dataLen);
        if (tmpParams[i].blob.data == NULL) {
            SECURITY_LOG_INFO("error");
            return HKS_ERROR_MALLOC_FAIL;
        }
        (void)memcpy_s(tmpParams[i].blob.data, dataLen, tmp + offSet, dataLen);
        offSet += dataLen;
    }

    uint32_t tmpTag;
    for (uint32_t i = 0; i < paramSetOut->paramsCnt; i++) {
        tmpTag = paramSetOut->params[i].tag;
        switch (tmpTag) {
            case DSLM_HKS_TAG_ATTESTATION_CHALLENGE:
            case DSLM_HKS_TAG_ATTESTATION_ID_SERIAL:
            case DSLM_HKS_TAG_ATTESTATION_ID_UDID:
            case DSLM_HKS_TAG_ATTESTATION_ID_SEC_LEVEL_INFO:
            case DSLM_HKS_TAG_ATTESTATION_ID_VERSION_INFO:
                for (uint32_t i = 0; i < DSLM_HKS_INTERFACE_TRANS_PARAM_NUM; i++) {
                    if (tmpTag == tmpParams[i].tag) {
                        paramSetOut->params[i].blob.size = tmpParams[i].blob.size;
                        (void)memcpy_s(paramSetOut->params[i].blob.data, tmpParams[i].blob.size, tmpParams[i].blob.data,
                            tmpParams[i].blob.size);
                    } else {
                        continue;
                    }
                }
                break;
            default:
                break;
        }
    }
    return HKS_SUCCESS;
}

static int32_t GenerateFuncParamJson(bool isSelfPk, const char *udidStr, char *dest, uint32_t destMax)
{
    JsonHandle json = CreateJson(NULL);
    if (json == NULL) {
        return ERR_INVALID_PARA;
    }

    AddFieldBoolToJson(json, "isSelfPk", isSelfPk);
    AddFieldStringToJson(json, "udid", udidStr);

    char *paramsJsonBuffer = ConvertJsonToString(json);
    if (paramsJsonBuffer == NULL) {
        DestroyJson(json);
        return ERR_MEMORY_ERR;
    }
    DestroyJson(json);
    if (strcpy_s(dest, destMax, paramsJsonBuffer) != EOK) {
        FREE(paramsJsonBuffer);
        paramsJsonBuffer = NULL;
        return ERR_MEMORY_ERR;
    }
    FREE(paramsJsonBuffer);
    paramsJsonBuffer = NULL;
    return SUCCESS;
}

int32_t GetPkInfoListStr(bool isSelf, const uint8_t *udid, uint32_t udidLen, char **pkInfoList)
{
    SECURITY_LOG_INFO("GetPkInfoListStr start");

    char udidStr[UDID_STRING_LENGTH] = {0};
    char paramJson[HICHIAN_INPUT_PARAM_STRING_LENGTH] = {0};
    char resultBuffer[] = "[{\"groupId\" : \"0\",\"publicKey\" : \"0\"}]";

    if (memcpy_s(udidStr, UDID_STRING_LENGTH, udid, udidLen) != EOK) {
        return ERR_MEMORY_ERR;
    }
    int32_t ret = GenerateFuncParamJson(isSelf, udidStr, &paramJson[0], HICHIAN_INPUT_PARAM_STRING_LENGTH);
    if (ret != SUCCESS) {
        SECURITY_LOG_INFO("GenerateFuncParamJson failed");
        return ret;
    }

    *pkInfoList = (char *)MALLOC(strlen(resultBuffer) + 1);
    if (strcpy_s(*pkInfoList, strlen(resultBuffer) + 1, resultBuffer) != EOK) {
        return ERR_MEMORY_ERR;
    }
    SECURITY_LOG_INFO("pkinfo = %{public}s", *pkInfoList);
    return SUCCESS;
}

int DslmCredAttestAdapter(char *nounceStr, char *credStr, uint8_t **certChain, uint32_t *certChainLen)
{
    SECURITY_LOG_INFO("DslmCredAttestAdapter start");

    struct HksParam inputData[] = {
        {.tag = DSLM_HKS_TAG_ATTESTATION_CHALLENGE, .blob = {strlen(nounceStr) + 1, (uint8_t *)nounceStr}},
        {.tag = DSLM_HKS_TAG_ATTESTATION_ID_SEC_LEVEL_INFO, .blob = {strlen(credStr) + 1, (uint8_t *)credStr}},
    };
    struct HksParamSet *inputParam = NULL;
    if (HksInitParamSet(&inputParam) != HKS_SUCCESS) {
        SECURITY_LOG_INFO("DslmCredAttestAdapter error 1");
        return -1;
    }
    if (HksAddParams(inputParam, inputData, sizeof(inputData) / sizeof(inputData[0])) != HKS_SUCCESS) {
        SECURITY_LOG_INFO("DslmCredAttestAdapter error 2");
        return -1;
    }
    if (HksBuildParamSet(&inputParam) != HKS_SUCCESS) {
        SECURITY_LOG_INFO("DslmCredAttestAdapter error 3");
        return -1;
    }

    uint32_t certChainMaxLen = strlen(nounceStr) + strlen(credStr) + DSLM_CERT_CHAIN_BASE_LENGTH;
    *certChain = (uint8_t *)malloc(certChainMaxLen);
    struct HksBlob certChainBlob = {certChainMaxLen, *certChain};
    struct HksCertChain hksCertChain = {&certChainBlob, DSLM_HKS_INTERFACE_TRANS_PARAM_NUM};

    const struct HksBlob keyAlias = {sizeof(g_keyData), (uint8_t *)g_keyData};

    int32_t ret = HksAttestKey2(&keyAlias, inputParam, &hksCertChain);
    if (ret != HKS_SUCCESS) {
        SECURITY_LOG_INFO("HksAttestKey ret = %{public}d ", ret);
        return ret;
    }
    *certChainLen = certChainBlob.size;
    SECURITY_LOG_INFO("DslmCredAttestAdapter success, certChainLen =  %{public}d ", *certChainLen);
    return SUCCESS;
}

int ValidateCertChainAdapter(uint8_t *data, uint32_t dataLen, struct CertChainValidateResult *resultInfo)
{
    SECURITY_LOG_INFO("ValidateCertChainAdapter start");
    struct HksParam outputData[] = {
        {.tag = DSLM_HKS_TAG_ATTESTATION_CHALLENGE, .blob = {resultInfo->nounceLen, resultInfo->nounce}},
        {.tag = DSLM_HKS_TAG_ATTESTATION_ID_SEC_LEVEL_INFO, .blob = {resultInfo->credLen, resultInfo->cred}},
    };
    struct HksParamSet *outputParam = NULL;
    if (HksInitParamSet(&outputParam) != HKS_SUCCESS) {
        return -1;
    }
    if (HksAddParams(outputParam, outputData, sizeof(outputData) / sizeof(outputData[0])) != HKS_SUCCESS) {
        return -1;
    }
    if (HksBuildParamSet(&outputParam) != HKS_SUCCESS) {
        return -1;
    }

    struct HksBlob certChainBlob = {dataLen, data};
    struct HksCertChain hksCertChain = {&certChainBlob, DSLM_HKS_INTERFACE_TRANS_PARAM_NUM};
    int32_t ret = HksValidateCertChain2(&hksCertChain, outputParam);
    if (ret != HKS_SUCCESS) {
        SECURITY_LOG_INFO("HksValidateCertChain error, ret = %{public}d", ret);
        return ERR_CALL_EXTERNAL_FUNC;
    }
    // fix
    resultInfo->nounceLen = strlen((char *)outputParam->params[0].blob.data);
    memcpy_s(resultInfo->nounce, resultInfo->nounceLen + 1, outputParam->params[0].blob.data,
        resultInfo->nounceLen + 1);
    resultInfo->credLen = strlen((char *)outputParam->params[1].blob.data);
    memcpy_s(resultInfo->cred, resultInfo->credLen + 1, outputParam->params[1].blob.data, resultInfo->credLen + 1);

    SECURITY_LOG_INFO("resultInfo nounce len = %{public}d", resultInfo->nounceLen);
    SECURITY_LOG_INFO("resultInfo cred len = %{public}d", resultInfo->credLen);

    SECURITY_LOG_INFO("resultInfo nounce = %{public}s", (char *)resultInfo->nounce);
    SECURITY_LOG_INFO("resultInfo cred = %{public}s", (char *)resultInfo->cred);

    return SUCCESS;
}

void InitCertChainValidateResult(struct CertChainValidateResult *resultInfo, uint32_t maxLen)
{
    (void)memset_s(resultInfo, sizeof(struct CertChainValidateResult), 0, sizeof(struct CertChainValidateResult));
    resultInfo->nounce = (uint8_t *)MALLOC(maxLen);
    resultInfo->nounceLen = maxLen;
    resultInfo->cred = (uint8_t *)MALLOC(maxLen);
    resultInfo->credLen = maxLen;
}

void DestroyCertChainValidateResult(struct CertChainValidateResult *resultInfo)
{
    if (resultInfo == NULL) {
        return;
    }
    if (resultInfo->udid != NULL) {
        FREE(resultInfo->udid);
        resultInfo->udid = NULL;
    }
    if (resultInfo->nounce != NULL) {
        FREE(resultInfo->nounce);
        resultInfo->nounce = NULL;
    }
    if (resultInfo->cred != NULL) {
        FREE(resultInfo->cred);
        resultInfo->cred = NULL;
    }
    if (resultInfo->serialNum != NULL) {
        FREE(resultInfo->serialNum);
        resultInfo->serialNum = NULL;
    }
    (void)memset_s(resultInfo, sizeof(struct CertChainValidateResult), 0, sizeof(struct CertChainValidateResult));
}
