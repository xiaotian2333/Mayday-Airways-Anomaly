#pragma once
#include "anomaly/sdk/base.h"

// Mayday Airways (Unity IL2CPP) 内建服务：金币/债务读写。
// 宿主在 GameAssembly.dll 可用后发布（id="mayday.money"）；
// il2cpp 线程 attach、类解析、参数装箱等细节由宿主封装，插件只消费高层语义。
// 失败时返回 ANOMALY_STATUS_V1_UNAVAILABLE，message 为稳定诊断 key：
// "status.unavailable"（游戏符号未就绪）、"status.not_in_flight"（未开局）、
// "status.attach_failed"（il2cpp 线程附加失败），由插件自行本地化。

#define ANOMALY_MAYDAY_MONEY_SERVICE_V1_ID "mayday.money"
#define ANOMALY_MAYDAY_MONEY_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnomalyMaydayMoneyServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // 读取当前金币（直接读 SyncVar 语义）
    AnomalyStatusV1 (ANOMALY_CALL *get_money)(void* user, int32_t* out_money);
    // 设置金币（直接写 SyncVar，不进游戏记账统计）
    AnomalyStatusV1 (ANOMALY_CALL *set_money)(void* user, int32_t value);
    // 增减金币（走游戏 ServerAddMoney 记账入口）
    AnomalyStatusV1 (ANOMALY_CALL *add_money)(void* user, int32_t delta);
    // 读取当前债务
    AnomalyStatusV1 (ANOMALY_CALL *get_debt)(void* user, int32_t* out_debt);
    // 设置债务（ServerAddDebt 到目标差额语义）
    AnomalyStatusV1 (ANOMALY_CALL *set_debt)(void* user, int32_t value);
} AnomalyMaydayMoneyServiceV1;

#ifdef __cplusplus
}
#endif
