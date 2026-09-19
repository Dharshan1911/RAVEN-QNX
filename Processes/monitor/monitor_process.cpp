#include "../common/shm_telemetry.h"
#include "../common/ipc_protocol.h"
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

int main() {
    printf("[MONITOR] Starting Central Monitor Process (PID: %d)...\n", getpid());

    // 1. Setup Shared Memory
    shm_unlink(RAVEN_SHM_NAME); 
    int shm_fd = shm_open(RAVEN_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("[MONITOR] shm_open failed");
        return -1;
    }
    ftruncate(shm_fd, sizeof(RavenSharedMemory));

    RavenSharedMemory* shm = (RavenSharedMemory*)mmap(NULL, sizeof(RavenSharedMemory), 
                                  PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    memset(shm, 0, sizeof(RavenSharedMemory));

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&shm->lock, &attr);

    // 2. Setup QNX IPC Channel
    int chid = ChannelCreate(0);
    if (chid == -1) {
        perror("[MONITOR] ChannelCreate failed");
        return -1;
    }

    // Publish IPC routing info to sensors via SHM
    shm->monitor_pid = getpid();
    shm->monitor_chid = chid;

    // 3. Setup Internal Timer Pulse (To periodically print the status table)
    int coid = ConnectAttach(ND_LOCAL_NODE, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent event;
    SIGEV_PULSE_INIT(&event, coid, 10, PULSE_MONITOR_TICK, 0);

    timer_t timer_id;
    timer_create(CLOCK_MONOTONIC, &event, &timer_id);

    struct itimerspec itime;
    itime.it_value.tv_sec = 2;
    itime.it_value.tv_nsec = 0;
    itime.it_interval.tv_sec = 2;
    itime.it_interval.tv_nsec = 0;
    timer_settime(timer_id, 0, &itime, NULL);

    printf("[MONITOR] Channel created (CHID: %d). Listening for pulses & messages...\n", chid);

    // 4. Main MsgReceive Loop
    while (1) {
        struct _msg_info info;
        union {
            struct _pulse pulse;
            RavenMessage req;
        } msg;

        int rcvid = MsgReceive(chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) {
            perror("[MONITOR] MsgReceive failed");
            continue;
        }

        if (rcvid == 0) {
            // --- RECEIVED A PULSE ---
            switch (msg.pulse.code) {
                case PULSE_SENSOR_READY:
                    printf("[MONITOR] EVENT: Sensor %d reported READY (Val: %d).\n", msg.pulse.value.sival_int, msg.pulse.value.sival_int);
                    break;
                case PULSE_HEARTBEAT:
                    // Lightweight heartbeat notification; detailed telemetry is already in SHM
                    // printf("[MONITOR] EVENT: Heartbeat pulse from sensor %d.\n", msg.pulse.value.sival_int);
                    break;
                case PULSE_DEADLINE_MISS:
                    printf("[MONITOR] FAULT DETECTED: Sensor %d missed its deadline!\n", msg.pulse.value.sival_int);
                    break;
                case _PULSE_CODE_DISCONNECT:
                    ConnectDetach(msg.pulse.scoid);
                    break;
                case PULSE_MONITOR_TICK:
                    // Timer fired: Print the shared memory table
                    pthread_mutex_lock(&shm->lock);
                    printf("\n--- SYSTEM TELEMETRY (via Shared Memory) ---\n");
                    for (int i = 0; i < MAX_SERVICES; i++) {
                        if (shm->services[i].active) {
                            printf(" %-14s [PID: %d] SEQ: %-4u CPU: %-5.1f%% EXEC: %-5lluus VAL: %.2f\n",
                                   shm->services[i].service_name, shm->services[i].pid,
                                   shm->services[i].sequence, shm->services[i].cpu_usage,
                                   shm->services[i].execution_time_us, shm->services[i].sensor_value);
                        }
                    }
                    pthread_mutex_unlock(&shm->lock);
                    break;
                default:
                    break;
            }
        } 
        else if (rcvid > 0) {
            // --- RECEIVED A SYNCHRONOUS MESSAGE ---
            RavenReply reply;
            memset(&reply, 0, sizeof(reply));
            
            if (msg.req.type == STATUS_REQUEST) {
                reply.status_code = 200; // OK
                // In a real system, we'd read SHM and reply with the status of msg.req.sensor_idx
            } 
            else if (msg.req.type == SENSOR_DATA_REQUEST) {
                pthread_mutex_lock(&shm->lock);
                reply.data_value = shm->services[msg.req.sensor_idx].sensor_value;
                pthread_mutex_unlock(&shm->lock);
                reply.status_code = 200;
            }
            
            MsgReply(rcvid, 0, &reply, sizeof(reply));
        }
    }

    return 0;
}
