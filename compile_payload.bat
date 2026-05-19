@echo off
echo Compiling test_payload.cpp...
g++ -o test_payload.exe test_payload.cpp -mconsole -static
if %errorlevel% equ 0 (
    echo [SUCCESS] test_payload.exe created
) else (
    echo [FAILED] Compilation error
)
pause