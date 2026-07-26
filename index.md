# Digital VLSI Circuit Testing Pipeline
**Real-Time Embedded Systems Capstone**

## 1. Project Overview
This project synthesizes core real-time embedded concepts into a cohesive system designed for Digital VLSI Diagnostics and Semiconductor Manufacturing. Utilizing a dual-core ESP32 architecture, the system strictly separates real-time hardware probing functions from non-deterministic network observability tasks. Physical circuits are "probed" at a high rate, monitoring for propagation delay and voltage limit violations (setup/hold time failures). Data passes through a deterministic IPC pipeline, and shared manufacturing yield statistics are safely aggregated and displayed to a live Wi-Fi web dashboard.

## 2. Recruiter-Aimed Portfolio Section
**Hardware-First Engineering Focus**
As an engineer specializing in the Digital VLSI Circuits track, my primary focus is on hands-on hardware engineering, physical circuit design, and bringing physical components to life. This project demonstrates my ability to bridge the gap between physical hardware and low-level firmware. By implementing a FreeRTOS dual-core pipeline, I ensured that critical hardware probing tasks remain deterministic and isolated from software-level network latency. I excel in environments where computer repair, component-level debugging, and strict hardware-software integration are required to hit rigorous manufacturing yields.

## 3. Demo Video
https://youtu.be/dp5mt0t1oXA

## 4. System Architecture
* **CORE 1 (Real-Time Plane):** `vlsi_probe_task` -> Queue -> `signal_eval_task` -> Event Group -> `yield_coord_task` -> `estop_responder_task`
* **CORE 0 (Observability Plane):** `webmonitor_task` (Wi-Fi Dashboard)
*(See repository for full Concurrency Diagram image).*

## 5. Task Table & WCET Evidence
| Task Name              | Priority | Period | WCET    | System Role                                                |
| -----------------------| -------- |------- | --------| -----------------------------------------------------------|
| `estop_responder_task` | 16       | Async  | ~45 µs  | Halts fab line on defect/e-stop (Direct Task Notification) |
| `vlsi_probe_task`      | 15       | 20 ms  | ~150 µs | Samples hardware data points                               |
| `signal_eval_task`     | 10       | Async  | ~100 µs | Parses timing violations                                   |
| `yield_coord_task`     | 8        | Async  | ~60 µs  | Aggregates yields (Mutex Protected)                        |
| `background_log_task`  | 2        | 100 ms | ~148 ms | Background compute load                                    |
| `webmonitor_task`      | 5        | N/A    | N/A     | Renders Wi-Fi Dashboard                                    |

## 6. Hazard Analysis & Standard Mapping
**Failure Induced:** Removing the `Mutex` lock around the global yield tracking variables (`total_wafers_tested` and `total_defects_found`).
**Result:** Because 32-bit reads/writes are generally atomic on Xtensa, a crash does not immediately occur. However, if the structure size increased, or if multiple Core 1 tasks attempted to log defects simultaneously while Core 0 read the memory bus, we would violate memory safety rules. This results in "torn reads," causing the web dashboard to report physically impossible manufacturing data.
