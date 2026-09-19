// kraise_system.cpp - KRAISE RTOS monitor for QNX (Raspberry Pi 4)
// One source file, one executable, but 7 separate QNX processes at runtime.
//
// main -> supervisor -> 5 sensor processes
//      -> cli dashboard
//
// NOTE: the sensor readings are SIMULATED (see read_pir / read_dht11 /
// read_ultrasonic). No GPIO is touched. The timing/CPU numbers are real.
// Recovery here is fork+restart done by our own supervisor. It is NOT QNX HAM.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <sys/neutrino.h>   // channels, messages, pulses, ClockCycles
#include <sys/dispatch.h>   // name_attach / name_open
#include <sys/iomsg.h>      // _IO_CONNECT
#include <sys/syspage.h>    // cycles_per_sec

using namespace std;

// ---------------------------------------------------------------- settings

#define SHM_NAME      "/kraise_telemetry"
#define CHANNEL_NAME  "kraise/supervisor"

#define MAGIC         0x4B524149    // "KRAI", used to check the shared memory
#define NUM_SERVICES  5
#define NAME_LEN      20

#define PRIO_SUPERVISOR 20          // QNX SCHED_FIFO priorities
#define PRIO_SENSOR     15
#define PRIO_CLI         8

#define PULSE_HEARTBEAT  (_PULSE_CODE_MINAVAIL + 1)
#define PULSE_TICK       (_PULSE_CODE_MINAVAIL + 2)
#define MSG_SENSOR_DATA  (_IO_MAX + 10)   // above the QNX I/O range

#define SUPERVISOR_TICK_US 100000   // supervisor checks everything every 100ms
#define CLI_REFRESH_US     500000

#define HB_FAIL_MISSES   3          // 3 missed heartbeats = dead
#define CPU_LIMIT_PCT    80.0
#define IPC_LIMIT_US     5000

// states
#define HB_OK 0
#define HB_WARN 1
#define HB_FAIL 2

#define ST_START 0
#define ST_HEALTHY 1
#define ST_DEGRADED 2
#define ST_RECOVERING 3
#define ST_DOWN 4

// ---------------------------------------------------------- service table

struct Config {
    const char *name;
    int period_us;
    int deadline_us;
    int work;              // size of the simulated sensor workload
};

Config services[NUM_SERVICES] = {
    { "svc_pir_a",      100000,  50000,  20000 },
    { "svc_pir_b",      100000,  50000,  20000 },
    { "svc_dht11_a",    200000, 100000, 400000 },
    { "svc_dht11_b",    200000, 100000, 400000 },
    { "svc_ultrasonic",  50000,  25000, 100000 }
};

// ------------------------------------------------------ shared telemetry

struct Service {
    char     name[NAME_LEN];
    int      pid;
    uint32_t seq;

    uint64_t exec_us;      // wall clock time of the sensor job
    uint64_t cpu_us;       // cpu time really used by the job
    uint64_t ipc_us;       // MsgSend round trip
    double   cpu_pct;      // cpu_us compared to the period

    int      period_us;
    int      deadline_us;
    int      deadline_ok;
    uint32_t deadline_misses;

    uint64_t last_hb_ns;
    uint64_t hb_count;
    int      hb_missed;
    int      hb_state;

    int      state;
    uint32_t restarts;
    uint64_t fault_ns;
    uint64_t recovery_ms;

    int      value1;       // simulated reading (motion / temp / distance)
    int      value2;       // simulated humidity for DHT11
};

struct Shared {
    uint32_t magic;
    uint32_t size;
    int      count;
    pthread_mutex_t lock;

    int      supervisor_pid;
    int      system_state;
    uint32_t total_faults;
    uint32_t total_recoveries;

    int      last_fault;       // service index, -1 = none
    int      last_recovery;    // service index, -1 = none
    uint64_t last_rto_ms;

    uint64_t cycles_per_sec;
    Service  svc[NUM_SERVICES];
};

// ---------------------------------------------------------- QNX messages

struct SensorMsg {
    uint16_t type;         // must be first for QNX messages
    uint16_t id;
    int      pid;
    uint32_t seq;
    int      deadline_ok;
    uint64_t exec_us;
    uint64_t cpu_us;
    int      value1;
    int      value2;
};

struct Reply {
    uint16_t type;
    int      status;
};

union InMsg {
    uint16_t      type;
    struct _pulse pulse;
    SensorMsg     data;
};

// --------------------------------------------------------- small helpers

volatile sig_atomic_t stop_flag = 0;

void handle_stop(int sig) { (void)sig; stop_flag = 1; }

void catch_signals() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// cpu time used by this thread, using pthread_getcpuclockid + clock_gettime
uint64_t cpu_ns() {
    clockid_t cid;
    struct timespec ts;
    if (pthread_getcpuclockid(pthread_self(), &cid) != 0) return 0;
    if (clock_gettime(cid, &ts) == -1) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void set_priority(const char *who, int prio) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0)
        printf("[%s] warning: SCHED_FIFO %d not set (%s)\n", who, prio, strerror(rc));
}

// lock with a timeout so a dead process can never freeze the CLI
int lock_shm(Shared *s, int ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += (long)ms * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; t.tv_sec++; }
    return pthread_mutex_timedlock(&s->lock, &t);
}

void unlock_shm(Shared *s) { pthread_mutex_unlock(&s->lock); }

const char *hb_text(int s) {
    if (s == HB_OK) return "OK";
    if (s == HB_WARN) return "WARN";
    return "FAIL";
}

const char *state_text(int s) {
    if (s == ST_START) return "START";
    if (s == ST_HEALTHY) return "HEALTHY";
    if (s == ST_DEGRADED) return "DEGRADED";
    if (s == ST_RECOVERING) return "RECOVERING";
    return "DOWN";
}

// ------------------------------------------------------- shared memory

// supervisor creates the shared memory and fills in the starting values
Shared *create_shm() {
    shm_unlink(SHM_NAME);          // remove anything left by an old run

    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd == -1) { perror("shm_open"); return NULL; }

    if (ftruncate(fd, sizeof(Shared)) == -1) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }

    void *p = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return NULL; }

    Shared *s = (Shared *)p;
    memset(s, 0, sizeof(Shared));

    // the mutex lives in shared memory, so it must be PROCESS_SHARED
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (pthread_mutex_init(&s->lock, &attr) != 0) {
        perror("pthread_mutex_init");
        munmap(p, sizeof(Shared));
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);

    s->magic = MAGIC;
    s->size = sizeof(Shared);
    s->count = NUM_SERVICES;
    s->system_state = ST_START;
    s->last_fault = -1;
    s->last_recovery = -1;
    s->cycles_per_sec = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    if (s->cycles_per_sec == 0) s->cycles_per_sec = 1;

    for (int i = 0; i < NUM_SERVICES; i++) {
        Service *t = &s->svc[i];
        snprintf(t->name, NAME_LEN, "%s", services[i].name);
        t->period_us = services[i].period_us;
        t->deadline_us = services[i].deadline_us;
        t->deadline_ok = 1;
        t->state = ST_START;
        t->last_hb_ns = now_ns();
    }
    return s;
}

// sensors and cli open the shared memory that the supervisor made
Shared *open_shm() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd == -1) return NULL;

    struct stat st;
    if (fstat(fd, &st) == -1 || (size_t)st.st_size < sizeof(Shared)) {
        close(fd);
        return NULL;
    }

    void *p = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;

    Shared *s = (Shared *)p;
    if (s->magic != MAGIC || s->size != sizeof(Shared) || s->count != NUM_SERVICES) {
        munmap(p, sizeof(Shared));
        return NULL;
    }
    return s;
}

// -------------------------------------------------- simulated sensors

volatile double sink = 0;

// deterministic busy work so the CPU/exec times we measure are real
void do_work(int loops) {
    double a = 0;
    for (int i = 0; i < loops; i++) a += i * 1.000001;
    sink = a;
}

// SIMULATED PIR: 1 = motion
int read_pir(uint32_t seq, int loops) {
    do_work(loops);
    return (seq % 17 == 0) ? 1 : 0;
}

// SIMULATED DHT11: temperature in C, humidity in %
void read_dht11(uint32_t seq, int loops, int *temp, int *hum) {
    do_work(loops);
    *temp = 24 + (seq % 5);
    *hum = 55 + (seq % 10);
}

// SIMULATED ultrasonic: distance in cm
int read_ultrasonic(uint32_t seq, int loops) {
    do_work(loops);
    return 30 + (seq % 40);
}

// ================================================== SENSOR PROCESS

void run_sensor(int id) {
    Config *cfg = &services[id];
    const char *me = cfg->name;

    catch_signals();
    printf("[%s] PID: %d\n", me, getpid());
    set_priority(me, PRIO_SENSOR);

    // wait for the supervisor's shared memory
    Shared *shm = NULL;
    for (int i = 0; i < 100 && !stop_flag; i++) {
        shm = open_shm();
        if (shm) break;
        usleep(50000);
    }
    if (!shm) {
        printf("[%s] error: no telemetry shared memory\n", me);
        exit(EXIT_FAILURE);
    }

    // connect to the supervisor's named channel
    int coid = -1;
    for (int i = 0; i < 200 && !stop_flag; i++) {
        coid = name_open(CHANNEL_NAME, 0);
        if (coid != -1) break;
        usleep(50000);
    }
    if (coid == -1) {
        printf("[%s] error: name_open failed (%s)\n", me, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // own channel + connection, the timer will send pulses to it
    int chid = ChannelCreate(0);
    if (chid == -1) { perror("ChannelCreate"); exit(EXIT_FAILURE); }

    int self_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (self_coid == -1) { perror("ConnectAttach"); exit(EXIT_FAILURE); }

    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, self_coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_TICK, id);

    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        perror("timer_create");
        exit(EXIT_FAILURE);
    }

    struct itimerspec it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = cfg->period_us / 1000000;
    it.it_value.tv_nsec = (cfg->period_us % 1000000) * 1000L;
    it.it_interval = it.it_value;
    if (timer_settime(timer, 0, &it, NULL) == -1) {
        perror("timer_settime");
        exit(EXIT_FAILURE);
    }

    if (lock_shm(shm, 200) == 0) {
        shm->svc[id].pid = getpid();
        unlock_shm(shm);
    }
    printf("[%s] started, period %d us, deadline %d us\n",
           me, cfg->period_us, cfg->deadline_us);

    uint32_t seq = 0;
    struct _pulse pulse;

    while (!stop_flag) {
        // wait for the next timer pulse
        int rcvid = MsgReceive(chid, &pulse, sizeof(pulse), NULL);
        if (rcvid == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (rcvid != 0) { MsgError(rcvid, ENOSYS); continue; }
        if (pulse.code != PULSE_TICK) continue;

        seq++;

        // measure wall clock with ClockCycles and cpu time with the cpu clock
        uint64_t c0 = ClockCycles();
        uint64_t cpu0 = cpu_ns();

        int v1 = 0, v2 = 0;
        if (id == 0 || id == 1) v1 = read_pir(seq, cfg->work);
        else if (id == 2 || id == 3) read_dht11(seq, cfg->work, &v1, &v2);
        else v1 = read_ultrasonic(seq, cfg->work);

        uint64_t cpu1 = cpu_ns();
        uint64_t c1 = ClockCycles();

        uint64_t exec_us = (c1 - c0) * 1000000ULL / shm->cycles_per_sec;
        uint64_t cpu_us = (cpu1 > cpu0) ? (cpu1 - cpu0) / 1000 : 0;
        double cpu_pct = (double)cpu_us * 100.0 / cfg->period_us;
        if (cpu_pct > 100.0) cpu_pct = 100.0;
        int deadline_ok = (exec_us <= (uint64_t)cfg->deadline_us) ? 1 : 0;

        // send the sample to the supervisor and time the round trip
        SensorMsg msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_SENSOR_DATA;
        msg.id = id;
        msg.pid = getpid();
        msg.seq = seq;
        msg.deadline_ok = deadline_ok;
        msg.exec_us = exec_us;
        msg.cpu_us = cpu_us;
        msg.value1 = v1;
        msg.value2 = v2;

        Reply reply;
        memset(&reply, 0, sizeof(reply));

        uint64_t t0 = now_ns();
        int rc = MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply));
        uint64_t ipc_us = (now_ns() - t0) / 1000;

        if (rc == -1) {
            printf("[%s] MsgSend failed: %s\n", me, strerror(errno));
            name_close(coid);
            coid = name_open(CHANNEL_NAME, 0);
            if (coid == -1) break;
        }

        // heartbeat pulse
        MsgSendPulse(coid, -1, PULSE_HEARTBEAT, id);

        // update the shared telemetry
        if (lock_shm(shm, 50) == 0) {
            Service *t = &shm->svc[id];
            t->pid = getpid();
            t->seq = seq;
            t->exec_us = exec_us;
            t->cpu_us = cpu_us;
            t->ipc_us = ipc_us;
            t->cpu_pct = cpu_pct;
            t->deadline_ok = deadline_ok;
            if (!deadline_ok) t->deadline_misses++;
            t->value1 = v1;
            t->value2 = v2;
            unlock_shm(shm);
        }
    }

    timer_delete(timer);
    if (coid != -1) name_close(coid);
    ConnectDetach(self_coid);
    ChannelDestroy(chid);
    munmap(shm, sizeof(Shared));
    printf("[%s] stopped (pid %d)\n", me, getpid());
    exit(EXIT_SUCCESS);
}

// ================================================== SUPERVISOR PROCESS

int start_service(int id) {
    int pid = fork();
    if (pid == -1) { perror("fork"); return -1; }
    if (pid == 0) run_sensor(id);      // child becomes the sensor
    return pid;
}

// our own recovery: kill leftovers, fork again, count the restart
void restart_service(Shared *shm, int id, const char *reason) {
    Service *t = &shm->svc[id];
    printf("[SUPERVISOR] fault on %s (pid %d): %s\n", services[id].name, t->pid, reason);
    printf("[SUPERVISOR] KRAISE supervisory recovery: restarting %s\n", services[id].name);

    if (t->pid > 0) {
        int status;
        kill(t->pid, SIGKILL);
        waitpid(t->pid, &status, 0);
    }

    t->fault_ns = now_ns();
    t->state = ST_RECOVERING;
    t->hb_state = HB_FAIL;
    shm->total_faults++;
    shm->last_fault = id;

    int pid = start_service(id);
    if (pid == -1) { t->state = ST_DOWN; t->pid = 0; return; }

    t->pid = pid;
    t->seq = 0;
    t->hb_missed = 0;
    t->last_hb_ns = now_ns();
    t->restarts++;
    shm->total_recoveries++;
    shm->last_recovery = id;

    printf("[SUPERVISOR] %s restarted, new PID %d (restart %u)\n",
           services[id].name, pid, t->restarts);
}

// runs every tick: find dead processes, check heartbeats, deadlines, cpu, ipc
void check_services(Shared *shm) {
    uint64_t now = now_ns();

    // any child that exited?
    int status;
    int dead;
    while ((dead = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < NUM_SERVICES; i++) {
            if (shm->svc[i].pid == dead) {
                shm->svc[i].state = ST_DOWN;
                printf("[SUPERVISOR] %s (pid %d) died\n", services[i].name, dead);
            }
        }
    }

    int bad = 0, warn = 0;

    for (int i = 0; i < NUM_SERVICES; i++) {
        Service *t = &shm->svc[i];
        uint64_t period_ns = (uint64_t)services[i].period_us * 1000;

        if (t->state == ST_DOWN) {
            restart_service(shm, i, "process death");
            bad = 1;
            continue;
        }
        if (t->state == ST_START) { warn = 1; continue; }

        // one period of lateness is normal jitter, after that we count misses
        uint64_t late = (now > t->last_hb_ns) ? now - t->last_hb_ns : 0;
        t->hb_missed = (late >= 2 * period_ns) ? (int)(late / period_ns) - 1 : 0;

        if (t->hb_missed == 0) t->hb_state = HB_OK;
        else if (t->hb_missed < HB_FAIL_MISSES) t->hb_state = HB_WARN;
        else t->hb_state = HB_FAIL;

        if (t->hb_state == HB_FAIL) {
            restart_service(shm, i, "heartbeat failure");
            bad = 1;
            continue;
        }

        if (t->state == ST_RECOVERING) { warn = 1; continue; }

        int degraded = 0;
        if (!t->deadline_ok) degraded = 1;
        if (t->cpu_pct > CPU_LIMIT_PCT) degraded = 1;
        if (t->ipc_us > IPC_LIMIT_US) degraded = 1;
        if (t->hb_state == HB_WARN) degraded = 1;

        t->state = degraded ? ST_DEGRADED : ST_HEALTHY;
        if (degraded) warn = 1;
    }

    if (bad) shm->system_state = ST_DOWN;
    else if (warn) shm->system_state = ST_DEGRADED;
    else shm->system_state = ST_HEALTHY;
}

void run_supervisor() {
    catch_signals();
    printf("[SUPERVISOR] PID: %d\n", getpid());
    set_priority("SUPERVISOR", PRIO_SUPERVISOR);

    Shared *shm = create_shm();
    if (!shm) {
        printf("[SUPERVISOR] error: cannot create shared memory\n");
        exit(EXIT_FAILURE);
    }
    shm->supervisor_pid = getpid();

    // QNX named channel that the sensors connect to
    name_attach_t *att = name_attach(NULL, CHANNEL_NAME, 0);
    if (att == NULL) { perror("name_attach"); exit(EXIT_FAILURE); }
    printf("[SUPERVISOR] channel %s ready (chid %d)\n", CHANNEL_NAME, att->chid);

    // timer that pulses our own channel every 100ms
    int self_coid = ConnectAttach(0, 0, att->chid, _NTO_SIDE_CHANNEL, 0);
    if (self_coid == -1) { perror("ConnectAttach"); exit(EXIT_FAILURE); }

    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, self_coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_TICK, 0);

    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        perror("timer_create");
        exit(EXIT_FAILURE);
    }

    struct itimerspec it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = 0;
    it.it_value.tv_nsec = SUPERVISOR_TICK_US * 1000L;
    it.it_interval = it.it_value;
    if (timer_settime(timer, 0, &it, NULL) == -1) {
        perror("timer_settime");
        exit(EXIT_FAILURE);
    }

    // start the five sensor processes
    for (int i = 0; i < NUM_SERVICES; i++) {
        int pid = start_service(i);
        if (pid == -1) continue;
        if (lock_shm(shm, 200) == 0) {
            shm->svc[i].pid = pid;
            shm->svc[i].last_hb_ns = now_ns();
            unlock_shm(shm);
        }
        usleep(30000);
    }
    printf("[SUPERVISOR] all services started, RECOVERY: ACTIVE\n");

    InMsg msg;
    while (!stop_flag) {
        memset(&msg, 0, sizeof(msg));
        int rcvid = MsgReceive(att->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) {
            if (errno == EINTR) continue;
            break;
        }

        // pulses arrive with rcvid 0
        if (rcvid == 0) {
            if (msg.pulse.code == PULSE_HEARTBEAT) {
                int id = msg.pulse.value.sival_int;
                if (id < 0 || id >= NUM_SERVICES) continue;
                if (lock_shm(shm, 50) == 0) {
                    Service *t = &shm->svc[id];
                    t->last_hb_ns = now_ns();
                    t->hb_count++;
                    t->hb_missed = 0;
                    t->hb_state = HB_OK;
                    if (t->state == ST_RECOVERING || t->state == ST_START) {
                        if (t->fault_ns > 0) {
                            t->recovery_ms = (now_ns() - t->fault_ns) / 1000000;
                            shm->last_rto_ms = t->recovery_ms;
                        }
                        t->state = ST_HEALTHY;
                        printf("[SUPERVISOR] %s healthy again (pid %d, recovery %"
                               PRIu64 " ms)\n", services[id].name, t->pid, t->recovery_ms);
                    }
                    unlock_shm(shm);
                }
            } else if (msg.pulse.code == PULSE_TICK) {
                if (lock_shm(shm, 50) == 0) {
                    check_services(shm);
                    unlock_shm(shm);
                }
            } else if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(msg.pulse.scoid);
            }
            continue;
        }

        // reply to the connect message that name_open() sends
        if (msg.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }
        if (msg.type > _IO_BASE && msg.type <= _IO_MAX) { MsgError(rcvid, ENOSYS); continue; }

        // our sensor data message
        if (msg.type == MSG_SENSOR_DATA) {
            int id = msg.data.id;
            if (id >= 0 && id < NUM_SERVICES && lock_shm(shm, 50) == 0) {
                Service *t = &shm->svc[id];
                t->pid = msg.data.pid;
                t->seq = msg.data.seq;
                t->exec_us = msg.data.exec_us;
                t->cpu_us = msg.data.cpu_us;
                t->deadline_ok = msg.data.deadline_ok;
                t->value1 = msg.data.value1;
                t->value2 = msg.data.value2;
                unlock_shm(shm);
            }
            Reply rep;
            memset(&rep, 0, sizeof(rep));
            rep.type = MSG_SENSOR_DATA;
            rep.status = 0;
            MsgReply(rcvid, EOK, &rep, sizeof(rep));
            continue;
        }

        MsgError(rcvid, ENOSYS);   // never leave a sender blocked
    }

    printf("[SUPERVISOR] stopping services\n");
    for (int i = 0; i < NUM_SERVICES; i++)
        if (shm->svc[i].pid > 0) kill(shm->svc[i].pid, SIGTERM);
    usleep(300000);
    for (int i = 0; i < NUM_SERVICES; i++) {
        if (shm->svc[i].pid > 0) {
            int status;
            kill(shm->svc[i].pid, SIGKILL);
            waitpid(shm->svc[i].pid, &status, 0);
        }
    }

    timer_delete(timer);
    ConnectDetach(self_coid);
    name_detach(att, 0);
    munmap(shm, sizeof(Shared));
    shm_unlink(SHM_NAME);
    exit(EXIT_SUCCESS);
}

// ================================================== CLI PROCESS

void print_waiting() {
    printf("\033[2J\033[H");
    printf("==========================================================\n");
    printf("                 KRAISE RTOS MONITOR\n");
    printf("==========================================================\n\n");
    printf("QNX          : ONLINE\n");
    printf("SUPERVISOR   : NOT REACHABLE\n");
    printf("TELEMETRY    : %s not available yet\n\n", SHM_NAME);
    printf("==========================================================\n");
    fflush(stdout);
}

void run_cli() {
    catch_signals();
    printf("[KRAISE WATCH] PID: %d\n", getpid());
    set_priority("KRAISE WATCH", PRIO_CLI);

    Shared *shm = NULL;
    static Shared copy;             // last good snapshot, printed outside the lock

    while (!stop_flag) {
        if (shm == NULL) {
            shm = open_shm();
            if (shm == NULL) {
                print_waiting();
                usleep(CLI_REFRESH_US);
                continue;
            }
        }

        int stale = 1;
        if (lock_shm(shm, 100) == 0) {
            memcpy(&copy, shm, sizeof(Shared));
            unlock_shm(shm);
            stale = 0;
        }

        printf("\033[2J\033[H");
        printf("==========================================================\n");
        printf("                 KRAISE RTOS MONITOR\n");
        printf("==========================================================\n\n");
        printf("QNX          : ONLINE\n");
        printf("SUPERVISOR   : RUNNING (pid %d)\n", copy.supervisor_pid);
        printf("RECOVERY     : ACTIVE (KRAISE supervisory recovery)\n\n");
        printf("SYSTEM       : %s%s\n\n", state_text(copy.system_state),
               stale ? "  (telemetry busy)" : "");
        printf("FAULTS       : %u\n", copy.total_faults);
        printf("RECOVERIES   : %u\n\n", copy.total_recoveries);

        printf("----------------------------------------------------------\n");
        printf("%-14s %-7s %-6s %-9s %-8s %-8s\n",
               "SERVICE", "PID", "CPU%", "EXEC(us)", "CPU(us)", "IPC(us)");
        printf("----------------------------------------------------------\n");
        for (int i = 0; i < NUM_SERVICES; i++) {
            Service *t = &copy.svc[i];
            double pct = (t->cpu_pct >= 0 && t->cpu_pct <= 100) ? t->cpu_pct : 0;
            printf("%-14s %-7d %-6.2f %-9" PRIu64 " %-8" PRIu64 " %-8" PRIu64 "\n",
                   services[i].name, t->pid, pct, t->exec_us, t->cpu_us, t->ipc_us);
        }
        printf("----------------------------------------------------------\n\n");

        printf("%-14s %-8s %-8s %-12s %s\n", "SERVICE", "DL", "HB", "STATE", "RESTARTS");
        printf("----------------------------------------------------------\n");
        for (int i = 0; i < NUM_SERVICES; i++) {
            Service *t = &copy.svc[i];
            printf("%-14s %-8s %-8s %-12s %u\n", services[i].name,
                   t->deadline_ok ? "OK" : "MISS", hb_text(t->hb_state),
                   state_text(t->state), t->restarts);
        }
        printf("----------------------------------------------------------\n\n");

        if (copy.last_fault >= 0 && copy.last_fault < NUM_SERVICES)
            printf("LAST FAULT    : %s\n", services[copy.last_fault].name);
        else
            printf("LAST FAULT    : NONE\n");

        if (copy.last_recovery >= 0 && copy.last_recovery < NUM_SERVICES)
            printf("LAST RECOVERY : %s\n", services[copy.last_recovery].name);
        else
            printf("LAST RECOVERY : NONE\n");

        if (copy.last_rto_ms > 0)
            printf("RTO           : %" PRIu64 " ms\n", copy.last_rto_ms);
        else
            printf("RTO           : N/A\n");

        printf("\nSensor values are simulated, not real GPIO readings.\n");
        printf("==========================================================\n");
        fflush(stdout);

        usleep(CLI_REFRESH_US);
    }

    if (shm) munmap(shm, sizeof(Shared));
    exit(EXIT_SUCCESS);
}

// ================================================== MAIN

int supervisor_pid = -1;
int cli_pid = -1;

void stop_children(int sig) {
    (void)sig;
    if (supervisor_pid > 0) kill(supervisor_pid, SIGTERM);
    if (cli_pid > 0) kill(cli_pid, SIGTERM);
    stop_flag = 1;
}

int main() {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[ORCHESTRATOR] Booting KRAISE...\n");
    printf("[ORCHESTRATOR] PID: %d\n", getpid());

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_children;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    supervisor_pid = fork();
    if (supervisor_pid == -1) { perror("fork"); exit(EXIT_FAILURE); }
    if (supervisor_pid == 0) run_supervisor();

    usleep(600000);    // give the supervisor time to make the shared memory

    cli_pid = fork();
    if (cli_pid == -1) { perror("fork"); exit(EXIT_FAILURE); }
    if (cli_pid == 0) run_cli();

    int left = 2;
    while (left > 0) {
        int status;
        int pid = wait(&status);
        if (pid == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (pid == supervisor_pid && cli_pid > 0) kill(cli_pid, SIGTERM);
        left--;
    }

    printf("[ORCHESTRATOR] KRAISE stopped.\n");
    return 0;
}
