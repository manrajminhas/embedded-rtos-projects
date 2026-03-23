# Embedded Real-Time Operating Systems (RTOS) Portfolio

[![Hardware](https://img.shields.io/badge/Hardware-STM32F4_Discovery-blue.svg)](https://www.st.com/en/evaluation-tools/stm32f4discovery.html)
[![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS_v9.0.0-orange.svg)](https://www.freertos.org/)
[![IDE](https://img.shields.io/badge/IDE-Atollic_TrueSTUDIO-purple.svg)]()
[![Language](https://img.shields.io/badge/Language-C-green.svg)]()

This repository contains two bare-metal embedded software projects built for the **STM32F4 Discovery Board** (ARM Cortex-M4). Both projects heavily utilize **FreeRTOS** to manage strict timing constraints, hardware interrupts, and concurrent task execution. 

These projects explore advanced embedded systems architecture, dynamic memory management, bare-metal peripheral interfacing (GPIO, ADC, SPI-like shift registers), and deterministic scheduling algorithms.

---

## 📂 Repository Structure

* `Traffic_Light_Project/`: A multi-state intersection control system driven by ADC inputs and shift registers.
* `DDS_Scheduler_Project/`: A custom Earliest Deadline First (EDF) scheduler built on top of FreeRTOS.

---

## 🚦 Project 1: Traffic Light System

### Overview
A real-time traffic light controller simulating a single-lane intersection. The system uses a potentiometer to dynamically adjust the flow rate of traffic at runtime. Cars are represented by LEDs driven by daisy-chained shift registers, accumulating at red lights and flowing through during green lights.

### Hardware Interfacing
* **GPIO:** Configured Port C for Push-Pull outputs to drive the Red (PC0), Amber (PC1), and Green (PC2) traffic LEDs.
* **ADC (Analog-to-Digital Converter):** Initialized ADC1 (Channel 13 on PC3) to read analog voltage from the potentiometer (0-4095 range) to dictate traffic density.
* **Shift Registers (74HC595):** Implemented a serial-to-parallel converter using PC6 (Data), PC7 (Clock), and PC8 (Reset) to control 19 individual car LEDs using only 3 MCU pins.

### RTOS Architecture (Tasks & IPC)
The system is divided into four highly concurrent tasks, communicating strictly via 5 FreeRTOS Queues (no global variables used for IPC):
1. **`Adjust_Traffic` (Priority 1):** Polls the ADC and dispatches the 12-bit value to control light timing and car generation rates.
2. **`Traffic_light` (Priority 1):** A state machine managing the light phases. Uses FreeRTOS Software Timers to dynamically alter Green and Red phase durations based on traffic load while keeping Amber constant at 2000ms.
3. **`Create_Traffic` (Priority 1):** Generates a 19-bit integer pattern representing the road. Shifts bits forward on Green, and accumulates bits (bumper-to-bumper) on Red/Amber.
4. **`Display_Traffic` (Priority 1):** Receives the 19-bit pattern and pulses the GPIO clock line to shift the data serially into the 74HC595 registers.

---

## ⏱️ Project 2: Deadline-Driven Scheduler (EDF)

### Overview
A custom, dynamic Priority-Inheriting EDF Scheduler built on top of FreeRTOS. Standard FreeRTOS uses static priorities, which can lead to starvation. This project replaces the default scheduling paradigm with a dynamic Deadline-Driven Scheduler (DDS) that calculates absolute deadlines at runtime and preemptively sorts tasks to guarantee the CPU is always given to the task closest to its deadline.

### Core Architecture
* **F-Tasks vs. DD-Tasks:** The system abstracts standard FreeRTOS Tasks (F-Tasks) into Deadline-Driven Tasks (DD-Tasks). The DDS tracks metadata including `release_time`, `absolute_deadline`, and `completion_time`.
* **Strict Priority Preemption:** The DDS actively elevates the FreeRTOS priority of the task with the nearest deadline to `HIGH` while demoting all others to `LOW`, forcing the native kernel to execute the correct task.
* **Dynamic Lists & Memory:** Utilizes `heap_4.c` to safely `malloc` and `free` linked-list nodes, actively sorting tasks into `Active`, `Completed`, and `Overdue` queues.
* **Generator & Interface Functions:** A Generator Task uses software timers to periodically release tasks. Auxiliary tasks communicate with the DDS exclusively via safe interface functions (`release_dd_task`, `complete_dd_task`) using Queue-based messaging.
* **System Telemetry:** A low-priority Monitor Task routinely traverses the internal lists to provide real-time performance logging to the Serial Wire Viewer (SWV) without starving the active worker tasks.

### Evaluated Test Benches
The scheduler was mathematically verified against three specific CPU utilization hyper-periods (1500ms):
1. **Under-Subscribed (TB1):** System mathematically guarantees all deadlines. Validated flawless EDF sorting.
2. **Over-Subscribed (TB2):** CPU utilization is ~101%. Proves the system safely catches, handles, and reports mathematically inevitable deadline misses via queue timeouts without crashing.
3. **100% Utilization (TB3):** Demonstrates real-world RTOS overhead (tick interrupts, context switching). Overhead compounds to cause cascading microsecond misses, accurately logged by the DDS.

---

## 🛠️ Hardware & Software Setup

### Prerequisites
* **Board:** STM32F407G-DISC1
* **IDE:** Atollic TrueSTUDIO for STM32
* **RTOS:** FreeRTOS v9.0.0+ 

### Build & Flash Instructions
1. Clone this repository.
2. Open TrueSTUDIO and select **File -> Import... -> Existing Projects into Workspace**.
3. Select either the `DDS_Scheduler_Project` or `Traffic_Light_Project` folder.
4. Ensure `heap_1.c` is excluded from the build, and `heap_4.c` is included (located in `FreeRTOS_Source/portable/MemMang`).
5. Click **Build** (Hammer Icon).
6. Connect the STM32F4 board via Mini-USB and click **Debug** to flash the firmware.
7. Enable the **SWV Console** (Port 0) in the debug configuration to view live system telemetry via the ITM trace port.

---

## 🧠 Strict Constraints & Technical Learnings
* **No Global Variables for IPC:** All inter-task communication is strictly handled via thread-safe FreeRTOS Queues (`xQueueSend` / `xQueueReceive`).
* **No Task Notifications:** Built custom Queue architectures to bypass standard Task Notification shortcuts.
* **RTOS Jitter & Timing:** Mastered the translation of system ticks (`pdMS_TO_TICKS`) and mitigated priority inversion and race conditions within hardware timer callbacks.
