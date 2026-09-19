# QNX Momentics System Profiler Workflow

This document explains how to generate a true `.kev` kernel event trace from the running RAVEN system and analyze it in the QNX Momentics IDE.

## 1. Trace Generation on QNX

First, ensure that the RAVEN system is running:
```sh
/data/home/qnxuser/kraise/kraise_system &
```

Once the CLI dashboard appears and verifies that all services are `HEALTHY`, start the QNX `tracelogger`. We use a dedicated trace script (`run_kraise_trace.sh`) to capture exactly 20 seconds of execution, while injecting a fault at the 8-second mark to capture the recovery event.

Run the provided script:
```sh
sh /data/home/qnxuser/kraise/run_kraise_trace.sh
```

**What the script does under the hood:**
It executes the following QNX tracing command:
```sh
tracelogger -f /data/home/qnxuser/kraise/kraise_trace.kev -s 20 -n 1 &
```
- `-f`: Output `.kev` file location.
- `-s 20`: Stops tracing automatically after 20 seconds.
- `-n 1`: Flushes the buffer every 1 second to prevent dropped events.

It then injects a fault by running `slay -f svc_pir_a` to force a process death, allowing the KRAISE Supervisor to trigger its recovery mechanism while being traced.

## 2. Transferring the Trace to Windows

Once the script completes and `kraise_trace.kev` is generated, transfer it securely to your Windows host using `scp`. 

Because of specific cipher compatibilities with the QNX 8.0 `sshd`, use the following flags:
```bat
scp -c aes256-ctr -o MACs=hmac-sha2-256 qnxuser@169.254.43.14:/data/home/qnxuser/kraise/kraise_trace.kev .
```
*(Replace `169.254.43.14` with your actual QNX Raspberry Pi IP address.)*

## 3. Analysis in QNX Momentics

1. Open **QNX Momentics IDE** on your host PC.
2. Switch to the **System Profiler** perspective.
3. Go to **File -> Import... -> QNX -> System Profiler Trace Data**.
4. Select the transferred `kraise_trace.kev` file.

### What the Trace Demonstrates:
- **CPU Usage:** True scheduling behavior of the `SCHED_FIFO` threads.
- **IPC Activity:** Microkernel message passing (`MsgSend`, `MsgReceive`, `MsgReply`) between sensor boundaries and the Supervisor.
- **Interrupts/Timers:** The exact firing cadence of the `SIGEV_PULSE` hardware timers waking the blocked sensor processes.
- **Process Recovery:** The exact timeline of the `svc_pir_a` process dying, the `waitpid()` orchestrator waking up, and the `fork()` creating the replacement process.
