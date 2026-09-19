# QNX High Availability Manager (HAM)

This directory is intentionally left empty. 

In the KRAISE Microkernel Architecture, we utilize QNX's **Self-Attaching Entities** pattern. Rather than running a centralized, monolithic HAM watchdog process (which would be a single point of failure), the High Availability Manager logic is natively integrated directly into the system using `libham`.

1. **Standalone Deployment (Phase 7):**
   The sensor and supervisor processes use `#include <ha/ham.h>` to attach *themselves* to the OS-level HAM service upon booting (`ham_attach_self()`). They define their own death conditions (`ham_condition_death()`) and recovery actions (`ham_action_restart()`).

2. **Monolithic Deployment (kraise_system.cpp):**
   When running the unified KRAISE Orchestrator (`src/kraise_system.cpp`), the HAM logic is built natively into the Master Orchestrator's `waitpid()` event loop. It autonomously detects child process crashes and forks new recovery instances in microseconds.

Because the intelligence is distributed into the processes and the OS orchestrator, no standalone HAM binaries exist in this folder.
