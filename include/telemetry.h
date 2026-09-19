#ifndef RAVEN_TELEMETRY_H
#define RAVEN_TELEMETRY_H

#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/types.h>

#define RAVEN_MONITOR_NAME "raven/monitor"
#define RAVEN_FAULT_NAME   "raven/fault_detector"
#define RAVEN_RECOVERY_NAME "raven/recovery_manager"

// Priorities mapping (from docs/PRIORITY_POLICY.md)
#define PRIO_RECOVERY  60
#define PRIO_FAULT_DET 55
#define PRIO_MONITOR   50
#define PRIO_ULTRA     40
#define PRIO_PIR       35
#define PRIO_DHT11     30
#define PRIO_TELEMETRY 15
#define PRIO_BRIDGE    15
#define PRIO_ANALYTICS 10

typedef enum {
    SYS_HEALTHY = 0,
    SYS_WARNING = 1,
    SYS_DEGRADED = 2,
    SYS_CRITICAL = 3,
    SYS_OFFLINE = 4,
    SYS_RECOVERING = 5,
    SYS_IPC_DELAY = 6
} fault_state_t;

typedef enum {
    REC_NONE = 0,
    REC_DIAGNOSING = 1,
    REC_RECOVERING = 2,
    REC_VERIFYING = 3,
    REC_RESTORED = 4,
    REC_FAILED = 5
} recovery_state_t;

typedef struct {
    uint64_t t_fault_ns;
    uint64_t t_detection_ns;
    uint64_t t_recovery_start_ns;
    uint64_t t_service_restart_ns;
    uint64_t t_heartbeat_restore_ns;
    uint64_t t_verification_ns;
    uint32_t retry_count;
} recovery_metrics_t;

typedef struct {
    char service_id[32];
    pid_t pid;
    pid_t tid;
    int priority;
    int sched_policy;
    uint32_t seq_num;
    uint64_t timestamp_ns;
    uint32_t execution_time_us;
    uint32_t expected_period_us;
    uint32_t actual_period_us;
    uint32_t deadline_us;
    uint8_t deadline_status;
    float cpu_usage_pct;
    uint32_t ipc_latency_us;
    uint8_t heartbeat_ok;
    uint64_t heartbeat_age_us;
    fault_state_t fault_state;
    recovery_state_t recovery_state;
    recovery_metrics_t rec_metrics;
    float anomaly_score;
} telemetry_record_t;

// Message types for MsgSend
#define RAVEN_MSG_TELEMETRY (_IO_MAX + 1)
#define RAVEN_MSG_HEARTBEAT (_IO_MAX + 2)
#define RAVEN_MSG_FAULT_ALERT (_IO_MAX + 3)
#define RAVEN_MSG_RECOVERY_CMD (_IO_MAX + 4)
#define RAVEN_MSG_GET_STATE (_IO_MAX + 5)
#define RAVEN_MSG_IPC_METRIC (_IO_MAX + 6)
#define RAVEN_MSG_GET_IPC_STATS (_IO_MAX + 7)
#define RAVEN_MSG_INJECT_DELAY (_IO_MAX + 8)
#define RAVEN_MSG_SET_FAULT (_IO_MAX + 9)
#define RAVEN_MSG_SET_RECOVERY (_IO_MAX + 10)

#define RAVEN_IPC_NAME "raven/ipc_monitor"

typedef struct {
    char src_service[32];
    char dst_service[32];
    uint32_t latency_us;
    int success;
} ipc_metric_data_t;

typedef struct {
    uint16_t type;
    ipc_metric_data_t data;
} raven_ipc_metric_msg_t;

typedef struct {
    uint32_t current_latency_us;
    uint32_t avg_latency_us;
    uint32_t min_latency_us;
    uint32_t max_latency_us;
    uint32_t tx_count;
    uint32_t tx_failed;
    uint32_t tx_delayed;
} ipc_stats_t;

typedef struct {
    uint16_t type;
    ipc_stats_t data;
} raven_ipc_stats_msg_t;

typedef struct {
    uint16_t type;
    uint32_t delay_us;
} raven_delay_msg_t;

// Max services aggregator can track
#define MAX_SERVICES 16
typedef struct {
    uint16_t type;
    int num_records;
    telemetry_record_t records[MAX_SERVICES];
} raven_state_msg_t;

typedef struct {
    uint16_t type;
    telemetry_record_t data;
} raven_msg_t;

typedef struct {
    uint16_t type;
    char service_id[32];
    fault_state_t fault_state;
} raven_set_fault_msg_t;

typedef struct {
    uint16_t type;
    char service_id[32];
    recovery_state_t recovery_state;
    recovery_metrics_t metrics;
} raven_set_recovery_msg_t;

#endif
