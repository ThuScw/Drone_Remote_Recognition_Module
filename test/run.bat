@echo off
REM === GB 46750-2025 / MAVLink Host-Side Test Suite ===
REM Requires g++ in PATH (MinGW-w64, MSYS2, or similar).
REM Double-click to build and run.

setlocal

echo Building test suite...
g++ -std=c++14 -Wall -Wextra -g -D_USE_MATH_DEFINES ^
    -I./stubs -I./stubs/driver -I../main -I../main/data -I../main/protocol ^
    -I../main/telemetry -I../main/broadcast -I../main/logging ^
    -o tests.exe ^
    test_main.cpp test_crc.cpp test_parser.cpp test_rid_messages.cpp ^
    test_fault_log.cpp test_status_machine.cpp test_flight_log.cpp ^
    ../main/data/mavlink_crc.cpp ^
    ../main/data/mavlink_parser.cpp ^
    ../main/protocol/rid_messages.cpp ^
    ../main/telemetry/fault_log.cpp ^
    ../main/logging/flight_log.cpp

if %ERRORLEVEL% neq 0 (
    echo.
    echo === BUILD FAILED ===
    pause
    exit /b 1
)

echo.
echo Build OK — running tests...
echo.
tests.exe

echo.
echo === Done ===
pause
