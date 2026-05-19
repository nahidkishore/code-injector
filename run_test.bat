@echo off
title Process Injection Test
color 0A
echo ========================================
echo   Process Injection Test - VM Lab
echo ========================================
echo.

:: প্রথমে চলমান notepad.exe কিল করুন
taskkill /f /im notepad.exe >nul 2>&1

:: ইনজেক্টর রান করুন
echo [1] Running Injector.exe...
Injector.exe

echo.
echo [2] Waiting 3 seconds...
timeout /t 3 /nobreak >nul

:: চেক করুন ইনজেকশন কাজ করছে কিনা
echo [3] Checking for injected process...
tasklist | findstr "notepad.exe"

echo.
echo [4] Check console output above for "Hello from Injected Process!"
echo ========================================
pause