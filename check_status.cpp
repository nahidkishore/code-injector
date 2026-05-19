#include <windows.h>
#include <tlhelp32.h>
#include <iostream>

/**
 * Retrieves the Process ID (PID) of a running process by its executable name.
 * 
 * @param processName The name of the executable.
 * @return The DWORD PID if found; otherwise, 0.
 */
DWORD GetProcessIdByName(const char* processName) {
    DWORD processId = 0;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe32)) {
        do {
            if (_stricmp(pe32.szExeFile, processName) == 0) {
                processId = pe32.th32ProcessID;
                break;
            }
        } while (Process32Next(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return processId;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "      DWAgent Injection Status Check    " << std::endl;
    std::cout << "========================================" << std::endl;

    // Check 1: Original dwagent.exe (Target for suspension)
    DWORD dwPid = GetProcessIdByName("dwagent.exe");
    std::cout << "[CHECK] Original dwagent.exe: ";
    if (dwPid != 0) {
        std::cout << "PASS (Found PID: " << dwPid << ")" << std::endl;
    } else {
        std::cout << "FAIL (Not running or terminated)" << std::endl;
    }

    // Check 2: Injected Host Process
    const char* candidates[] = { "svchost.exe", "explorer.exe", "RuntimeBroker.exe" };
    bool hostFound = false;
    for (const char* host : candidates) {
        DWORD hPid = GetProcessIdByName(host);
        if (hPid != 0) {
            std::cout << "[CHECK] Injected Host (" << host << "): PASS (PID: " << hPid << ")" << std::endl;
            hostFound = true;
            break;
        }
    }
    if (!hostFound) {
        std::cout << "[CHECK] Injected Host: FAIL (No candidate hosts found)" << std::endl;
    }

    std::cout << "========================================" << std::endl;
    return 0;
}