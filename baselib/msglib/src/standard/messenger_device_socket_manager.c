/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
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
#include "messenger_device_socket_manager.h"

#include <pthread.h>
#include <stdlib.h>

#include "securec.h"
#include "socket.h"

#include "messenger_device_status_manager.h"
#include "messenger_utils.h"
#include "utils_list.h"
#include "utils_log.h"
#include "utils_mem.h"
#include "utils_mutex.h"
#include "utils_timer.h"

#define MSG_BUFF_MAX_LENGTH (81920 * 4)
#define PKG_NAME_LEN 128
#define SOCKET_NAME_LEN 128
#define WAITING_TIMEOUT_LEN 20000

typedef struct DeviceSocketManager {
    ListHead pendingSendList;
    ListHead serverSocketList;
    ListHead clientSocketList;
    DeviceMessageReceiver messageReceiver;
    MessageSendResultNotifier sendResultNotifier;
    const char *pkgName;
    const char *primarySockName;
    const char *secondarySockName;
    int32_t primarySocket;
    int32_t secondarySocket;
    WorkQueue *queue;
    Mutex mutex;
} DeviceSocketManager;

typedef struct QueueMsgData {
    DeviceIdentify srcIdentity;
    uint32_t msgLen;
    uint8_t msgData[1];
} QueueMsgData;

typedef struct PendingMsgData {
    ListNode link;
    uint32_t transNo;
    DeviceIdentify destIdentity;
    uint32_t msgLen;
    uint8_t msgData[1];
} PendingMsgData;

typedef struct SocketNodeInfo {
    ListNode link;
    int32_t socket;
    uint32_t maskId;
    DeviceIdentify identity;
    TimerHandle timeHandle;
} SocketNodeInfo;

static DeviceSocketManager *GetDeviceSocketManagerInstance(void)
{
    static DeviceSocketManager manager = {
        .pendingSendList = INIT_LIST(manager.pendingSendList),
        .serverSocketList = INIT_LIST(manager.serverSocketList),
        .clientSocketList = INIT_LIST(manager.clientSocketList),
        .messageReceiver = NULL,
        .sendResultNotifier = NULL,
        .queue = NULL,
        .mutex = INITED_MUTEX,
        .pkgName = NULL,
        .primarySockName = NULL,
        .secondarySockName = NULL,
        .primarySocket = 0,
        .secondarySocket = 0,
    };
    return &manager;
}

static void ProcessSocketMessageReceived(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    QueueMsgData *queueData = (QueueMsgData *)data;
    if (queueData->msgLen + sizeof(QueueMsgData) != len) {
        SECURITY_LOG_ERROR("invalid input");
        FREE(queueData);
        return;
    }

    DeviceMessageReceiver messageReceiver = instance->messageReceiver;
    if (messageReceiver == NULL) {
        SECURITY_LOG_ERROR("messageReceiver is null");
        FREE(queueData);
        return;
    }
    messageReceiver(&queueData->srcIdentity, queueData->msgData, queueData->msgLen);
    FREE(queueData);
}

static void OnSocketMessageReceived(const DeviceIdentify *devId, const uint8_t *msg, uint32_t msgLen)
{
    if (devId == NULL || msg == NULL) {
        return;
    }
    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    WorkQueue *queue = instance->queue;
    if (queue == NULL) {
        SECURITY_LOG_ERROR("queue is null");
        return;
    }
    DeviceMessageReceiver messageReceiver = instance->messageReceiver;
    if (messageReceiver == NULL) {
        SECURITY_LOG_ERROR("messageReceiver is null");
        return;
    }
    uint32_t queueDataLen = sizeof(QueueMsgData) + msgLen;
    QueueMsgData *queueData = MALLOC(queueDataLen);
    if (queueData == NULL) {
        SECURITY_LOG_ERROR("malloc result null");
        return;
    }
    uint32_t ret = (uint32_t)memcpy_s(&queueData->srcIdentity, sizeof(DeviceIdentify), devId, sizeof(DeviceIdentify));
    if (ret != EOK) {
        SECURITY_LOG_ERROR("memcpy failed");
        FREE(queueData);
        return;
    }
    ret = (uint32_t)memcpy_s(queueData->msgData, msgLen, msg, msgLen);
    if (ret != EOK) {
        SECURITY_LOG_ERROR("memcpy failed");
        FREE(queueData);
        return;
    }
    queueData->msgLen = msgLen;
    ret = QueueWork(queue, ProcessSocketMessageReceived, (uint8_t *)queueData, queueDataLen);
    if (ret != WORK_QUEUE_OK) {
        SECURITY_LOG_ERROR("QueueWork failed, ret is %{public}u", ret);
        FREE(queueData);
        return;
    }
}

static void RemoveSocketNode(int32_t socket, ShutdownReason reason, bool isServer)
{
    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    ListHead *socketList = &instance->clientSocketList;
    if (isServer) {
        socketList = &instance->serverSocketList;
    }

    LockMutex(&instance->mutex);
    ListNode *node = NULL;
    ListNode *temp = NULL;
    FOREACH_LIST_NODE_SAFE (node, socketList, temp) {
        SocketNodeInfo *info = LIST_ENTRY(node, SocketNodeInfo, link);
        if (info->socket == socket) {
            SECURITY_LOG_INFO("Shutdown reason is %{public}u, device is %{public}x", reason, info->maskId);
            RemoveListNode(node);
            FREE(info);
        }
    }
    UnlockMutex(&instance->mutex);
}

static void ServerOnShutdown(int32_t socket, ShutdownReason reason)
{
    if (socket == 0) {
        return;
    }
    RemoveSocketNode(socket, reason, true);
}

static void ClientOnShutdown(int32_t socket, ShutdownReason reason)
{
    if (socket == 0) {
        return;
    }
    RemoveSocketNode(socket, reason, false);
}

static void TimerProcessWaitingTimeOut(const void *context)
{
    if (context == NULL) {
        return;
    }
    const int32_t socket = (const int32_t)context;

    Shutdown(socket);
    ShutdownReason reason = SHUTDOWN_REASON_LOCAL;
    ClientOnShutdown(socket, reason);
    SECURITY_LOG_INFO("SocketClosed, socket is %{public}d", socket);
}

static void CreateOrRestartSocketCloseTimer(int32_t socket)
{
    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    ListNode *node = NULL;
    int32_t timeSocket = socket;
    SocketNodeInfo *socketInfo = NULL;
    FOREACH_LIST_NODE (node, &instance->clientSocketList) {
        SocketNodeInfo *curr = LIST_ENTRY(node, SocketNodeInfo, link);
        if (curr->socket == socket) {
            socketInfo = curr;
            break;
        }
    }

    if (socketInfo == NULL) {
        return;
    }

    if (socketInfo->timeHandle != 0) {
        UtilsStopTimerTask(socketInfo->timeHandle);
    }
    SECURITY_LOG_INFO("SocketTimerWaiting, socket is %{public}d", socket);
    socketInfo->timeHandle =
        UtilsStartOnceTimerTask(WAITING_TIMEOUT_LEN, TimerProcessWaitingTimeOut, (const void *)timeSocket);
}

static void CreateOrRestartSocketCloseTimerWithLock(int32_t socket)
{
    DeviceSocketManager *inst = GetDeviceSocketManagerInstance();

    LockMutex(&inst->mutex);
    CreateOrRestartSocketCloseTimer(socket);
    UnlockMutex(&inst->mutex);
}

static bool GetIdentityByServerSocket(int32_t socket, DeviceIdentify *identity)
{
    if (identity == NULL) {
        return false;
    }

    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    bool find = false;
    LockMutex(&instance->mutex);
    ListNode *node = NULL;
    SocketNodeInfo *socketInfo;
    FOREACH_LIST_NODE (node, &instance->serverSocketList) {
        socketInfo = LIST_ENTRY(node, SocketNodeInfo, link);
        if (socketInfo->socket == socket) {
            *identity = socketInfo->identity;
            find = true;
            break;
        }
    }
    UnlockMutex(&instance->mutex);

    return find;
}

static SocketNodeInfo *CreateSocketNodeInfo(int32_t socket, const DeviceIdentify *identity)
{
    if (identity == NULL) {
        SECURITY_LOG_ERROR("Create socket node info invalid params");
        return NULL;
    }

    uint32_t maskId = MaskDeviceIdentity((const char *)identity->identity, DEVICE_ID_MAX_LEN);

    SocketNodeInfo *socketInfo = MALLOC(sizeof(SocketNodeInfo));
    if (socketInfo == NULL) {
        SECURITY_LOG_ERROR("malloc failed, socketInfo is null");
        return NULL;
    }
    socketInfo->socket = socket;
    socketInfo->maskId = maskId;
    socketInfo->timeHandle = 0;
    socketInfo->identity = *identity;
    SECURITY_LOG_INFO("Binding device is %{public}x, socket is %{public}d", maskId, socket);

    return socketInfo;
}

static void ServerOnBind(int32_t socket, PeerSocketInfo info)
{
    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    DeviceIdentify identity = {DEVICE_ID_MAX_LEN, {0}};
    if (!MessengerGetDeviceIdentifyByNetworkId(info.deviceId, &identity)) {
        SECURITY_LOG_ERROR("MessengerGetDeviceIdentifyByNetworkId failed");
        return;
    }
    LockMutex(&instance->mutex);
    ListNode *node = NULL;
    ListNode *temp = NULL;
    FOREACH_LIST_NODE_SAFE (node, &instance->serverSocketList, temp) {
        SocketNodeInfo *Nodeinfo = LIST_ENTRY(node, SocketNodeInfo, link);
        if (IsSameDevice(&Nodeinfo->identity, &identity)) {
            RemoveListNode(node);
            FREE(Nodeinfo);
        }
    }
    UnlockMutex(&instance->mutex);

    SocketNodeInfo *socketInfo = CreateSocketNodeInfo(socket, &identity);
    if (socketInfo == NULL) {
        return;
    }
    LockMutex(&instance->mutex);
    AddListNodeBefore(&socketInfo->link, &instance->serverSocketList);
    UnlockMutex(&instance->mutex);
}

static void ClientOnBind(int socket, const DeviceIdentify *devId)
{
    if (devId == NULL) {
        SECURITY_LOG_ERROR("client on bind invalid params");
        return;
    }
    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    SocketNodeInfo *socketInfo = CreateSocketNodeInfo(socket, devId);
    if (socketInfo == NULL) {
        return;
    }

    LockMutex(&instance->mutex);
    AddListNodeBefore(&socketInfo->link, &instance->clientSocketList);
    ListNode *node = NULL;
    ListNode *temp = NULL;
    FOREACH_LIST_NODE_SAFE (node, &instance->pendingSendList, temp) {
        PendingMsgData *msgData = LIST_ENTRY(node, PendingMsgData, link);
        if (!IsSameDevice(&msgData->destIdentity, devId)) {
            continue;
        }
        RemoveListNode(node);

        int sent = SendBytes(socket, msgData->msgData, msgData->msgLen);
        if (sent != 0) {
            SECURITY_LOG_ERROR("SendBytes error code = %{public}d", sent);
        }
        CreateOrRestartSocketCloseTimer(socket);
        FREE(msgData);
    }
    UnlockMutex(&instance->mutex);

    return;
}

static void ServerOnBytes(int32_t socket, const void *data, unsigned int dataLen)
{
    if (data == NULL) {
        SECURITY_LOG_ERROR("Server on bytes invalid params");
        return;
    }

    SECURITY_LOG_INFO("ServerOnBytes, socket is %{public}d", socket);
    DeviceIdentify identity = {DEVICE_ID_MAX_LEN, {0}};
    if (GetIdentityByServerSocket(socket, &identity) == false) {
        SECURITY_LOG_ERROR("Get identity by server list failed");
        return;
    }

    OnSocketMessageReceived(&identity, (const uint8_t *)data, (uint32_t)dataLen);
}

static int32_t ProcessCreateServer(const char *session, const char *pkg, int32_t *socketId)
{
    if (session == NULL || pkg == NULL || socketId == NULL) {
        SECURITY_LOG_ERROR("invalid params create server");
        return -1;
    }

    char sessionName[SOCKET_NAME_LEN + 1] = {0};
    char pkgName[PKG_NAME_LEN + 1] = {0};
    int32_t ret = memcpy_s(sessionName, SOCKET_NAME_LEN, session, SOCKET_NAME_LEN);
    if (ret != EOK) {
        SECURITY_LOG_ERROR("memcpy sessionName failed");
        return ret;
    }
    ret = memcpy_s(pkgName, PKG_NAME_LEN, pkg, PKG_NAME_LEN);
    if (ret != EOK) {
        SECURITY_LOG_ERROR("memcpy pkgName failed");
        return ret;
    }
    static QosTV serverQos[] = {
        {.qos = QOS_TYPE_MIN_BW, .value = 20},
        {.qos = QOS_TYPE_MAX_LATENCY, .value = 2000},
        {.qos = QOS_TYPE_MAX_WAIT_TIMEOUT, .value = 30000},
        {.qos = QOS_TYPE_MAX_BUFFER, .value = 10000},
        {.qos = QOS_TYPE_MAX_IDLE_TIMEOUT, .value = 30000},
    };
    static ISocketListener serverListener = {
        .OnBind = ServerOnBind,
        .OnShutdown = ServerOnShutdown,
        .OnBytes = ServerOnBytes,
    };

    SocketInfo socketInfo = {
        .name = sessionName,
        .pkgName = pkgName,
        .dataType = DATA_TYPE_BYTES,
    };

    int32_t socket = Socket(socketInfo);
    if (socket <= 0) {
        SECURITY_LOG_ERROR("Socket failed");
        return socket;
    }
    ret = Listen(socket, serverQos, sizeof(serverQos) / sizeof(QosTV), &serverListener);
    if (ret != 0) {
        SECURITY_LOG_ERROR("Listen Socket %{public}d failed", socket);
        Shutdown(socket);
        return ret;
    }

    *socketId = socket;
    return 0;
}

static bool CreateServer(DeviceSocketManager *inst)
{
    if (inst == NULL) {
        SECURITY_LOG_ERROR("Get Device Socket Manager Instance failed");
        return false;
    }

    int32_t socket = 0;
    if (ProcessCreateServer(inst->primarySockName, inst->pkgName, &socket) == 0) {
        SECURITY_LOG_INFO("Listen success, socket is %{public}d", socket);
        inst->primarySocket = socket;
    }
    if (ProcessCreateServer(inst->secondarySockName, inst->pkgName, &socket) == 0) {
        SECURITY_LOG_INFO("Listen success, socket is %{public}d", socket);
        inst->secondarySocket = socket;
    }

    if (inst->primarySocket == 0 && inst->secondarySocket == 0) {
        return false;
    }

    return true;
}

bool InitDeviceSocketManager(WorkQueue *queue, const MessengerConfig *config)
{
    if ((queue == NULL) || (config == NULL)) {
        return false;
    }
    DeviceSocketManager *inst = GetDeviceSocketManagerInstance();

    inst->primarySockName = config->primarySockName;
    inst->secondarySockName = config->secondarySockName;
    inst->pkgName = config->pkgName;
    inst->messageReceiver = config->messageReceiver;
    inst->sendResultNotifier = config->sendResultNotifier;
    inst->queue = queue;

    return CreateServer(inst);
}

bool DeInitDeviceSocketManager(void)
{
    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    Shutdown(instance->primarySocket);
    Shutdown(instance->secondarySocket);

    LockMutex(&instance->mutex);
    instance->primarySockName = NULL;
    instance->secondarySockName = NULL;
    instance->pkgName = NULL;
    instance->messageReceiver = NULL;
    instance->sendResultNotifier = NULL;
    instance->queue = NULL;
    instance->primarySocket = 0;
    instance->secondarySocket = 0;

    ListNode *node = NULL;
    ListNode *temp = NULL;
    FOREACH_LIST_NODE_SAFE (node, &instance->pendingSendList, temp) {
        PendingMsgData *msgData = LIST_ENTRY(node, PendingMsgData, link);
        RemoveListNode(node);
        FREE(msgData);
    }

    node = NULL;
    temp = NULL;
    FOREACH_LIST_NODE_SAFE (node, &instance->serverSocketList, temp) {
        SocketNodeInfo *serverInfo = LIST_ENTRY(node, SocketNodeInfo, link);
        RemoveListNode(node);
        FREE(serverInfo);
    }

    node = NULL;
    temp = NULL;
    FOREACH_LIST_NODE_SAFE (node, &instance->clientSocketList, temp) {
        SocketNodeInfo *clientInfo = LIST_ENTRY(node, SocketNodeInfo, link);
        RemoveListNode(node);
        FREE(clientInfo);
    }

    DestroyWorkQueue(instance->queue);
    UnlockMutex(&instance->mutex);

    return true;
}

static bool GetSocketByClientSocketList(const DeviceIdentify *devId, int32_t *socket)
{
    if (devId == NULL || socket == NULL) {
        return false;
    }

    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    bool find = false;
    LockMutex(&instance->mutex);
    ListNode *node = NULL;
    FOREACH_LIST_NODE (node, &instance->clientSocketList) {
        SocketNodeInfo *socketInfo = LIST_ENTRY(node, SocketNodeInfo, link);
        if (IsSameDevice(&socketInfo->identity, devId)) {
            *socket = socketInfo->socket;
            find = true;
            break;
        }
    }
    UnlockMutex(&instance->mutex);

    return find;
}

static void PushMsgDataToPendingList(uint32_t transNo, const DeviceIdentify *devId, const uint8_t *msg, uint32_t msgLen)
{
    if (devId == NULL || msg == NULL) {
        SECURITY_LOG_ERROR("Push msg data to pending list invalid params");
        return;
    }
    DeviceSocketManager *instance = GetDeviceSocketManagerInstance();

    PendingMsgData *data = MALLOC(sizeof(PendingMsgData) + msgLen);
    if (data == NULL) {
        SECURITY_LOG_ERROR("MALLOC data failed");
        return;
    }
    data->transNo = transNo;
    data->msgLen = msgLen;
    int32_t ret = memcpy_s(&data->destIdentity, sizeof(DeviceIdentify), devId, sizeof(DeviceIdentify));
    if (ret != EOK) {
        SECURITY_LOG_ERROR("memcpy failed");
        FREE(data);
        return;
    }
    ret = memcpy_s(data->msgData, msgLen, msg, msgLen);
    if (ret != EOK) {
        SECURITY_LOG_ERROR("memcpy failed");
        FREE(data);
        return;
    }

    LockMutex(&instance->mutex);
    AddListNodeBefore(&data->link, &instance->pendingSendList);
    UnlockMutex(&instance->mutex);
}

static void ClientOnFakeBind(int32_t socket, PeerSocketInfo info)
{
    SECURITY_LOG_INFO("Start FakeBind");
}

static void BindSync(int32_t socket, const DeviceIdentify *devId)
{
    if (devId == NULL) {
        SECURITY_LOG_ERROR("Bind sync invalid params");
        return;
    }
    static QosTV clientQos[] = {
        {.qos = QOS_TYPE_MIN_BW, .value = 20},
        {.qos = QOS_TYPE_MAX_LATENCY, .value = 2000},
        {.qos = QOS_TYPE_MAX_WAIT_TIMEOUT, .value = 5000},
        {.qos = QOS_TYPE_MAX_BUFFER, .value = 10000},
    };
    static ISocketListener clientListener = {
        .OnShutdown = ClientOnShutdown,
        .OnBind = ClientOnFakeBind,
    };
    int32_t ret = Bind(socket, clientQos, sizeof(clientQos) / sizeof(QosTV), &clientListener);
    SECURITY_LOG_INFO("Primary Bind ret is %{public}d", ret);
    if (ret == 0) {
        SECURITY_LOG_INFO("Bind Success");
        ClientOnBind(socket, devId);
        return;
    }

    SECURITY_LOG_ERROR("Bind Failed");
}

static int32_t ProcessBindSocket(const char *socketName, DeviceIdentify *devId, int32_t *socketId)
{
    if (socketName == NULL || devId == NULL || socketId == NULL) {
        SECURITY_LOG_ERROR("invalid params bind socket");
        return -1;
    }

    DeviceSocketManager *inst = GetDeviceSocketManagerInstance();
    char NetworkId[DEVICE_ID_MAX_LEN + 1] = {0};
    if (!MessengerGetNetworkIdByDeviceIdentify(devId, NetworkId, DEVICE_ID_MAX_LEN + 1)) {
        SECURITY_LOG_ERROR("Get NetworkId Failed");
        return -1;
    }
    uint32_t maskId = MaskDeviceIdentity((const char *)&devId->identity, DEVICE_ID_MAX_LEN);
    char name[SOCKET_NAME_LEN + 1] = {0};
    int32_t ret = memcpy_s(name, SOCKET_NAME_LEN, socketName, SOCKET_NAME_LEN);
    if (ret != EOK) {
        SECURITY_LOG_ERROR("memcpy name failed");
        return ret;
    }
    char clientName[SOCKET_NAME_LEN + 1] = {0};
    ret = snprintf_s(clientName, SOCKET_NAME_LEN, SOCKET_NAME_LEN - 1, "device.security.level.%lu",
        (unsigned long)maskId);
    if (ret < 0) {
        SECURITY_LOG_ERROR("snprintf failed");
        return ret;
    }
    SECURITY_LOG_INFO("clientName is %{public}s", clientName);
    char pkgName[PKG_NAME_LEN + 1] = {0};
    ret = memcpy_s(pkgName, PKG_NAME_LEN, inst->pkgName, PKG_NAME_LEN);
    if (ret != EOK) {
        SECURITY_LOG_ERROR("memcpy pkgName failed");
        return ret;
    }

    SocketInfo socketInfo = {
        .name = clientName,
        .pkgName = pkgName,
        .peerName = name,
        .peerNetworkId = NetworkId,
        .dataType = DATA_TYPE_BYTES,
    };
    int32_t socket = Socket(socketInfo);
    if (socket <= 0) {
        return -1;
    }
    *socketId = socket;
    return 0;
}

void *BindSyncWithPthread(void *arg)
{
    pthread_detach(pthread_self());
    if (arg == NULL) {
        SECURITY_LOG_ERROR("Bind sync with pthread invalid params");
        return NULL;
    }
    DeviceSocketManager *inst = GetDeviceSocketManagerInstance();
    DeviceIdentify *devId = (DeviceIdentify *)arg;

    DeviceIdentify identity = *devId;
    FREE(devId);

    int32_t socket = 0;
    if (ProcessBindSocket(inst->primarySockName, &identity, &socket) == 0) {
        SECURITY_LOG_INFO("Socket is %{public}d ", socket);
        BindSync(socket, &identity);
    }

    if (ProcessBindSocket(inst->secondarySockName, &identity, &socket) == 0) {
        SECURITY_LOG_INFO("Socket is %{public}d ", socket);
        BindSync(socket, &identity);
    }

    return NULL;
}

static void BindAsync(const DeviceIdentify *devId)
{
    if (devId == NULL) {
        SECURITY_LOG_ERROR("Bind async invalid params");
        return;
    }
    DeviceIdentify *identity = MALLOC(sizeof(DeviceIdentify));
    if (identity == NULL) {
        SECURITY_LOG_ERROR("MALLOC identity failed");
        return;
    }
    *identity = *devId;

    pthread_t id;
    pthread_create(&id, NULL, BindSyncWithPthread, identity);
}

void MessengerSendMsgTo(uint64_t transNo, const DeviceIdentify *devId, const uint8_t *msg, uint32_t msgLen)
{
    if (devId == NULL || msg == NULL || msgLen == 0 || msgLen > MSG_BUFF_MAX_LENGTH) {
        SECURITY_LOG_ERROR("invalid params");
        return;
    }

    static DeviceIdentify self = {0, {0}};
    uint32_t devType;
    MessengerGetSelfDeviceIdentify(&self, &devType);

    if (IsSameDevice(&self, devId)) {
        SECURITY_LOG_DEBUG("loopback msg");
        OnSocketMessageReceived(devId, msg, msgLen);
        return;
    }

    int32_t socket = 0;
    bool find = GetSocketByClientSocketList(devId, &socket);
    if (find && socket != 0) {
        int32_t ret = SendBytes(socket, msg, msgLen);
        if (ret != 0) {
            SECURITY_LOG_ERROR("SendBytes error code = %{public}d", ret);
            return;
        }
        CreateOrRestartSocketCloseTimerWithLock(socket);
        return;
    }

    PushMsgDataToPendingList(transNo, devId, msg, msgLen);
    BindAsync(devId);
}
