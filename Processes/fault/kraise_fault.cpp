#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syspage.h>

#include "../common/kraise_telemetry.h"
#include "../common/kraise_ipc.h"
#include "../common/kraise_config.h"

uint64_t cyclesToUs(uint64_t cycles) {
    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    return (cycles * 1000000ULL) / cps;
}

int main() {
    printf("[FAULT_DETECTOR] Starting PID %d\n", getpid());
    
    struct sched_param param;
    param.sched_priority = PRIO_FAULT_DETECTOR;
    sched_setscheduler(0, SCHED_FIFO, &param);

    int shm_fd = -1;
    while ((shm_fd = shm_open(KRAISE_SHM_NAME, O_RDWR, 0666)) == -1) { usleep(100000); }
    KraiseSharedMemory* shm = (KraiseSharedMemory*)mmap(NULL, sizeof(KraiseSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    int rec_coid = -1;
    while((rec_coid = name_open(RECOVERY_ATTACH_NAME, 0)) == -1) { usleep(100000); }

    while (1) {
        if (shm->header.shutdown_requested) break;
        
        uint64_t now_cycles = ClockCycles();

        for (int i = 0; i < MAX_SERVICES; i++) {
            pthread_mutex_lock(&shm->lock);
            SensorTelemetry t = shm->services[i];
            pthread_mutex_unlock(&shm->lock);

            if (!t.active) continue;
            if (t.health_state == HEALTH_FAILED || t.health_state == HEALTH_RECOVERING) continue;

            bool fault_detected = false;
            char fault_reason[64] = "";
            
            // 1. Heartbeat Check
            uint64_t time_since_hb = cyclesToUs(now_cycles - t.last_heartbeat);
            if (time_since_hb > HEARTBEAT_TIMEOUT_US) {
                pthread_mutex_lock(&shm->lock);
                shm->services[i].heartbeat_ok = false;
                shm->services[i].heartbeat_misses++;
                shm->services[i].consecutive_misses++;
                pthread_mutex_unlock(&shm->lock);
                
                if (shm->services[i].consecutive_misses >= HEARTBEAT_MISS_LIMIT) {
                    fault_detected = true;
                    strcpy(fault_reason, "HEARTBEAT_LOSS");
                }
            } else {
                pthread_mutex_lock(&shm->lock);
                shm->services[i].heartbeat_ok = true;
                shm->services[i].consecutive_misses = 0;
                pthread_mutex_unlock(&shm->lock);
            }

            // 2. Deadline Check (Manual Fault Injection)
            char manual_fault[128];
            snprintf(manual_fault, sizeof(manual_fault), "/tmp/fault_%s", t.name);
            if (access(manual_fault, F_OK) == 0) {
                fault_detected = true;
                strcpy(fault_reason, "MANUAL_FAULT_INJECTION");
                unlink(manual_fault); // clear it
            }

            if (fault_detected) {
                pthread_mutex_lock(&shm->lock);
                shm->services[i].health_state = HEALTH_FAILED;
                shm->services[i].fault_count++;
                strcpy(shm->services[i].fault_reason, fault_reason);
                pthread_mutex_unlock(&shm->lock);

                printf("[FAULT] Detected failure on %s (%s). Notifying Recovery.\n", t.name, fault_reason);
                MsgSendPulse(rec_coid, -1, PULSE_FAULT, i);
            }
        }
        usleep(100000); // 10Hz evaluation
    }
    return 0;
}
