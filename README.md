# Capstone Project: Digital VLSI Circuit Testing Pipeline

## Project Overview
This project synthesizes core real-time embedded concepts into a cohesive system themed around Digital VLSI Diagnostics and Semiconductor Manufacturing. It utilizes a dual-core architecture to strictly separate real-time hardware probing functions from non-deterministic network observability tasks.

Physical circuits are simulated to be "probed" at a high rate. The system monitors for propagation delay and voltage limit violations (setup/hold time failures). Data is passed through a deterministic IPC pipeline, and shared manufacturing yield statistics are safely aggregated and displayed to a web dashboard.

## System Architecture Diagram

```text
CORE 1 (Real-Time Plane)                                    | CORE 0 (Observability)
                                                            |
 [ Wafer / PCB ] --(Samples)--> [ vlsi_probe_task ]         |
                                      ||                    |
                                 FreeRTOS Queue             |
                                      ||                    |
                             [ signal_eval_task ]           |
                                      ||                    |
                        (SetBits: PROBED & EVALUATED)       |
                                      ||                    |
 [ E-STOP Button ]               [ Event Group ]            |
      ||                              ||                    |
  (Interrupt)                 [ yield_coord_task ] -------->|---> [ HTTP Web Server ]
      ||                              || (Mutex Protected   |      (Wi-Fi Dashboard)
      ||                              ||  Stats Update)     |
      ++===> [ Task Notification ] <==++                    |
                     ||                                     |
         [ estop_responder_task ]                           |
                     ||                                     |
              [ Halt Alarm LED ]                            |
