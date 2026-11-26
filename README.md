# Cooldown Timer

A Windows desktop cooldown timer with Python and C++ implementations. It can show floating countdown windows and detect user-provided image templates on screen to start timers.

## Features

- Floating, draggable countdown windows
- Screen-region selection and OpenCV template matching
- Tray menu, hotkeys, sound alerts, and flashing countdown display
- Timer positions and settings saved locally in `CoolDownTimer.ini`

## Python version

Requirements: Windows, Python 3.10 or later, and the packages listed in `requirements.txt`.

```powershell
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
python cooldown_timer.py
```

For automatic image detection, provide your own `trigger_1.png` and `trigger_2.png` templates in the same folder as the script. These templates are not included because they are specific to the screen content being recognized. The application may request screen-capture and keyboard-hook permissions from Windows security software.

## C++ version

Requirements: Windows, CMake, a C++17 compiler (such as Visual Studio Build Tools), and an OpenCV installation discoverable by CMake.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Run `build\Release\cooldown_timer_cpp.exe` after a successful build. OpenCV DLLs may need to be on `PATH` or beside the executable, depending on how OpenCV is installed.

## Usage

- F2: create a timer manually
- F3: reset timers
- F4: show or hide timer windows
- Tray menu: show, hide, reset, configure, or exit

The C++ version supports adding image templates through its settings interface. The Python version looks for `trigger_1.png` and `trigger_2.png` beside the script. Template images and local settings are not included in this repository.

## Notes

- This project is Windows-specific and captures screen content for image matching.
- `CoolDownTimer.ini` is created locally and can contain screen coordinates and window positions; do not share your personal copy.
- The repository excludes local build outputs, machine-specific editor settings, unrelated experiments, and artwork whose redistribution rights have not been confirmed.
