# 🖥️ MyTaskManager — Windows Process & Thread Manager (C++/CLI)

<p align="center">
  <img src="https://img.shields.io/badge/C++-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white" alt="C++"/>
  <img src="https://img.shields.io/badge/Windows%20API-0078D4?style=for-the-badge&logo=windows&logoColor=white" alt="Windows API"/>
  <img src="https://img.shields.io/badge/Windows%20Forms-0078D4?style=for-the-badge&logo=windows&logoColor=white" alt="Windows Forms"/>
  <img src="https://img.shields.io/badge/Multithreading-orange?style=for-the-badge" alt="Multithreading"/>
  <img src="https://img.shields.io/badge/Visual%20Studio-5C2D91?style=for-the-badge&logo=visualstudio&logoColor=white" alt="Visual Studio"/>
</p>

---

## 📖 Project Overview

**MyTaskManager** is a Windows desktop application built with **C++/CLI, Windows Forms, and the Windows API**. It provides a graphical interface for monitoring running processes and their threads, inspecting resource usage, and performing selected process-management operations.

The application refreshes process information automatically and also includes a manual refresh button. A companion command-line program, **MyWorker.exe**, can be launched from the interface to demonstrate process monitoring and multithreaded work. The worker processes PGM grayscale images and writes analysis results to CSV files.

---

## ✨ Features

- 📋 **Process monitoring** — view PID, process name, CPU usage, priority class, state, memory usage, thread count, and owner.
- 🧵 **Thread monitoring** — inspect the TID, owning PID, CPU usage, relative priority, and state of threads belonging to the selected process.
- 🔄 **Automatic and manual refresh** — update the data on demand or automatically every 15 seconds.
- ↕️ **Column sorting** — click a process-table header to sort by that column; click it again to reverse the direction.
- 🔍 **Process filters** — hide system processes, show only the selected PID, and enable thread information for the selected process.
- ▶️ **Launch an executable** — choose an `.exe` file through a file dialog and start it using `CreateProcessW`.
- ⏸️ **Suspend and resume** — suspend or resume the selected process where Windows permissions allow it.
- 🛑 **Terminate a process** — request process termination after a confirmation dialog.
- ⚙️ **Change process priority** — choose Idle, Below normal, Normal, Above normal, or High.
- 🧩 **CPU affinity** — select which available logical CPU cores a process may use.
- 📝 **Operation log** — record management operations, errors, and detected process completions.
- 🛡️ **Access-aware reporting** — display unavailable values when Windows does not provide access to particular process information.

---

## 🕹️ Visual Guide & Usage

### 1️⃣ Step 1: View Running Processes

Start the application to populate the process table. Each row represents a process and includes its identifier, resource usage, priority, state, memory statistics, thread count, and owner.

Click **Оновити** to refresh the information manually. Otherwise, the application refreshes automatically every 15 seconds.

### 2️⃣ Step 2: Inspect Threads and Filter the List

Select a process in the process table. Use **Показати потоки** to request its thread information. The thread table displays each thread's identifier, associated PID, CPU usage, relative priority, and state.

Use the checkboxes to hide system processes or limit the list to the selected PID. Click **Скинути фільтри** to clear the filters and reset column sorting.

### 3️⃣ Step 3: Manage a Process

Right-click a process row to open the context menu. Available operations include:

- **Завершити процес** — request termination after confirmation.
- **Призупинити / Відновити** — suspend or resume the process.
- **Пріоритет процесу** — change its priority class.
- **Відповідність ядрам** — configure the process's CPU affinity mask.

Successful operations and errors are recorded in the log at the bottom of the window. Some operations may require running the application with administrator privileges. Windows-protected processes may remain inaccessible.

### 4️⃣ Step 4: Launch the Worker Program

Click **Запустити** and select `MyWorker.exe`. The current implementation passes the following arguments to the selected executable:

```text
--threads 4 --dir images --out result --dark 50 --bright 200 --repeat 50
```

These arguments configure four worker threads, the input directory (`images`), the output directory (`result`), the dark and bright pixel thresholds, and the number of processing repetitions. Make sure the input directory contains supported `.pgm` files before running the worker.

> **Note:** The launch arguments are currently hard-coded in the GUI. The selected executable should accept these arguments; they are intended for the companion `MyWorker.exe` program.

---

## 🧪 Companion Worker: PGM Image Analyzer

`MyWorker.exe` is a native C++ command-line program that processes grayscale images in **ASCII PGM (P2)** format. It distributes the input files among a configurable number of Windows threads and computes image statistics.

### What it calculates

- Pixel count, minimum and maximum pixel values.
- Mean and median pixel intensity.
- A 256-value pixel-intensity histogram for each image.
- Fraction of pixels below the `--dark` threshold and above the `--bright` threshold.
- Processing duration, number of errors, and per-thread input-file assignments.

### Command-line examples

Generate sample PGM images:

```bat
MyWorker.exe --generate --dir images --count 24
```

Process images using four worker threads:

```bat
MyWorker.exe --threads 4 --dir images --out result --dark 50 --bright 200 --repeat 1
```

The `--threads` value must be between 1 and 32, and the thresholds must satisfy `0 <= dark < bright <= 255`.

### Output files

The worker writes its results to the directory specified by `--out`:

- `summary.csv` — summary statistics for successfully processed images.
- `errors.csv` — file names and error descriptions for failed inputs.
- `manifest.csv` — mapping of worker threads to the files assigned to them.
- `histogram_<filename>.csv` — 256 histogram entries for each successfully processed image.

---

## 🛠️ Tech Stack

- **Languages:** C++ and C++/CLI
- **GUI:** Windows Forms
- **Operating system APIs:** Windows API (`CreateProcessW`, process/thread information, priorities, suspension, and affinity)
- **Libraries:** Windows SDK, PSAPI, Advapi32
- **Worker features:** C++17 filesystem support, standard library, Windows threads
- **IDE:** Visual Studio

---

## 📂 Key Project Files

- `MyTaskManager.h` / `MainForm.h` — main Windows Forms interface, process table, thread table, filters, context menu, and event handlers. Use the actual header filename present in your solution consistently with the include in the `.cpp` file.
- `MyTaskManager.cpp` — application entry point and startup of the main form.
- `ProcAPI.h` — process/thread data structures and the `ProcApi::Monitor` interface.
- `ProcAPI.cpp` — Windows API implementation for collecting process data and performing supported operations.
- `MyWorker.cpp` — command-line PGM image generator and multithreaded image analyzer; builds to `MyWorker.exe`.

---

## 🚀 How to Run

1. Clone the repository:

   ```bash
   git clone https://github.com/USERNAME/REPOSITORY.git
   ```

2. Open the solution (`.sln`) in Visual Studio.
3. Make sure the Windows Forms project is configured for **C++/CLI** and the worker project is configured for a C++ standard that supports `std::filesystem` (C++17 or later).
4. Build the projects. If you want to use the launch integration, ensure the worker output is named `MyWorker.exe` and prepare an `images` folder containing P2 `.pgm` files.
5. Run the task manager. Run as administrator only when elevated access is needed for the operation you intend to perform.

---

## ⚠️ Important Notes

- Process-management operations can cause unsaved work to be lost. Only use them on processes you understand and intend to manage.
- Access to process metrics and management operations depends on Windows permissions and process protection.
- The GUI currently supplies fixed command-line arguments when launching an executable. Update `btnLaunch_Click` if you need to provide different arguments or a different working directory.

---

## 👤 Author

**XkrasherX**

<p>
  <a href="https://github.com/XkrasherX">
    <img src="https://img.shields.io/badge/GitHub-gray?style=for-the-badge&logo=github" alt="GitHub"/>
  </a>
</p>

---

<p align="center">
  ✨ <i>If you found this project useful for learning the Windows API, process management, and multithreading, feel free to give it a star!</i> ✨
</p>
