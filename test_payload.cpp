#include <iostream>
#include <windows.h>

int main() {
    // This will run inside the hollowed process
    while (true) {
        std::cout << "Hello from Injected Process!" << std::endl;
        Sleep(2000); // Wait 2 seconds between prints
    }
    return 0;
}