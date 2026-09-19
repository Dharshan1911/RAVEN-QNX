#include "../common/shm_telemetry.h"
#include "../common/timing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>

int main() {
    printf("[RECOVERY_MGR] Starting RTO Verification & Tracking (PID: %d)...\n", getpid());

    int shm_fd = -1;
    while ((shm_fd = shm_open(RAVEN_SHM_NAME, O_RDWR, 0666)) == -1) usleep(100000);
    RavenSharedMemory* shm = (RavenSharedMemory*)mmap(NULL, sizeof(RavenSharedMemory), 
                                  PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    uint64_t failure_time[MAX_SERVICES] = {0};
    bool was_failed[MAX_SERVICES] = {false};

    while (1) {
        pthread_mutex_lock(&shm->lock);
        uint64_t current_time = monotonic_time_us();

        for (int i = 0; i < MAX_SERVICES; i++) {
            SensorTelemetry* t = &shm->services[i];
            if (t->pid == 0) continue; 

            if (t->health_state == HEALTH_FAILED && !was_failed[i]) {
                failure_time[i] = t->state_transition_timestamp_us;
                was_failed[i] = true;
            }

            if (t->health_state == HEALTH_RECOVERING && was_failed[i]) {
                if (t->consecutive_misses == 0 && t->heartbeat_history == 0 && t->sequence >= 5) {
                    t->health_state = HEALTH_OK;
                    uint64_t rto_us = current_time - failure_time[i];
                    was_failed[i] = false;
                    
                    shm->last_rto_us = rto_us;
                    strncpy(shm->last_recovery_svc, t->service_name, sizeof(shm->last_recovery_svc));
                    
                    printf("\n[HAM RECOVERY VERIFIED] %s is HEALTHY. RTO: %llu us\n", t->service_name, rto_us);
                }
            }
        }
        pthread_mutex_unlock(&shm->lock);
        usleep(10000); 
    }

    return 0;
}
