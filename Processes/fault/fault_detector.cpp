#include "../common/shm_telemetry.h"
#include "../common/ipc_protocol.h"
#include "../common/timing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <time.h>
#include <signal.h>

const char* health_to_str(ServiceHealth h) {
    switch(h) {
        case HEALTH_OK: return "OK";
        case HEALTH_DEGRADED: return "DEGRADED";
        case HEALTH_UNHEALTHY: return "UNHEALTHY";
        case HEALTH_FAILED: return "FAILED";
        case HEALTH_RECOVERING: return "RECOVERING";
        default: return "UNKNOWN";
    }
}

int main() {
    printf("[FAULT_DETECTOR] Starting Rule Engine (PID: %d)...\n", getpid());

    int shm_fd = -1;
    while ((shm_fd = shm_open(RAVEN_SHM_NAME, O_RDWR, 0666)) == -1) usleep(100000);
    RavenSharedMemory* shm = (RavenSharedMemory*)mmap(NULL, sizeof(RavenSharedMemory), 
                                  PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    uint64_t last_eval_us[MAX_SERVICES] = {0};
    uint32_t last_seq[MAX_SERVICES] = {0};
    bool tracking[MAX_SERVICES] = {false};

    int chid = ChannelCreate(0);
    int coid = ConnectAttach(ND_LOCAL_NODE, 0, chid, _NTO_SIDE_CHANNEL, 0);
    
    struct sigevent event;
    SIGEV_PULSE_INIT(&event, coid, 25, _PULSE_CODE_MINAVAIL + 10, 0);
    timer_t timer_id;
    timer_create(CLOCK_MONOTONIC, &event, &timer_id);
    
    struct itimerspec itime;
    itime.it_value.tv_sec = 0;
    itime.it_value.tv_nsec = 50000000;
    itime.it_interval.tv_sec = 0;
    itime.it_interval.tv_nsec = 50000000;
    timer_settime(timer_id, 0, &itime, NULL);

    while (1) {
        struct _msg_info info;
        struct _pulse pulse;
        if (MsgReceive(chid, &pulse, sizeof(pulse), &info) == -1) continue;
        uint64_t current_time = monotonic_time_us();

        pthread_mutex_lock(&shm->lock);

        for (int i = 0; i < MAX_SERVICES; i++) {
            SensorTelemetry* t = &shm->services[i];
            
            if (!t->active) continue;

            // Detect if process was externally killed by checking /proc or sending signal 0
            if (kill(t->pid, 0) == -1 && t->health_state != HEALTH_FAILED && t->health_state != HEALTH_RECOVERING) {
                t->health_state = HEALTH_FAILED;
                t->current_fault = FAULT_PROCESS_FAILURE;
                t->heartbeat_ok = false;
                t->service_ok = false;
                t->state_transition_timestamp_us = current_time;
                printf("[FAULT_DETECTOR] External Process Death Detected on %s (PID: %d)\n", t->service_name, t->pid);
                continue;
            }

            if (!tracking[i]) {
                last_eval_us[i] = current_time;
                last_seq[i] = t->sequence;
                tracking[i] = true;
            }

            while (current_time - last_eval_us[i] >= t->expected_period_us && t->health_state != HEALTH_FAILED) {
                last_eval_us[i] += t->expected_period_us;
                ServiceHealth old_health = t->health_state;
                
                if (t->sequence == last_seq[i]) {
                    t->heartbeat_history = ((t->heartbeat_history << 1) | 1) & 0x07;
                    t->consecutive_misses++;
                } else {
                    t->heartbeat_history = ((t->heartbeat_history << 1) | 0) & 0x07;
                    t->consecutive_misses = 0;
                    last_seq[i] = t->sequence;
                }

                int bits = 0;
                for (int b=0; b<3; b++) if ((t->heartbeat_history >> b) & 1) bits++;

                if (t->consecutive_misses >= 3) {
                    t->health_state = HEALTH_FAILED;
                    t->current_fault = FAULT_HEARTBEAT_FAILED;
                    t->heartbeat_ok = false;
                    t->service_ok = false;
                    t->sensor_value = NAN;
                    // Trigger HAM explicitly by killing the hung process
                    printf("[FAULT_DETECTOR] %s deadlocked! Issuing SIGKILL to trigger HAM.\n", t->service_name);
                    kill(t->pid, SIGKILL);
                } else if (bits >= 2) {
                    t->health_state = HEALTH_UNHEALTHY;
                    t->heartbeat_ok = false;
                    t->service_ok = false;
                    t->sensor_value = NAN;
                } else if (bits == 1) {
                    t->health_state = HEALTH_DEGRADED;
                    t->heartbeat_ok = true; 
                    t->service_ok = true; 
                } else if (t->health_state != HEALTH_RECOVERING) {
                    t->health_state = HEALTH_OK;
                    t->heartbeat_ok = true;
                    t->service_ok = true;
                }

                if (old_health != t->health_state) {
                    t->state_transition_timestamp_us = current_time;
                }
            }
        }
        
        pthread_mutex_unlock(&shm->lock);
    }

    return 0;
}
