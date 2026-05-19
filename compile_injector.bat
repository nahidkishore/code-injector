@echo off
echo Compiling Injector.exe...
g++ -o Injector.exe GetProcessId.cpp -mconsole -static -lws2_32
if %errorlevel% equ 0 (
    echo [SUCCESS] Injector.exe created
) else (
    echo [FAILED] Compilation error
)
pause