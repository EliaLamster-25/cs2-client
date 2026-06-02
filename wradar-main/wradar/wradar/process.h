#include <windows.h>
#include <iostream>
#include <string>
#include <Psapi.h>
#include <TlHelp32.h>

class Proc {
public:
    Proc() : m_ui64PID(0), m_hProcess(0), m_hSnapshot(0) {
        if (!this->OpenProcessHandle()) {
            std::cerr << "Couldn't open a handle to the cs2 process! Please make sure it is running" << std::endl;
        }
    }

    ~Proc() {
        CloseHandle(m_hProcess);
        CloseHandle(m_hSnapshot);
    }

    template <typename type>
    type ReadMemory(uintptr_t addr) {
        type ret;
        ReadProcessMemory(this->m_hProcess, (LPCVOID)addr, &ret, sizeof(type), 0);
        return ret;
    }

    template <class T>
    bool WriteMemory(uintptr_t addr, T v) {
        return this->WriteMemory(addr, v, sizeof(T));
    }

    template <class T>
    bool WriteMemory(uintptr_t addr, const void* buffer, size_t size) {
        SIZE_T written;
        return WriteProcessMemory(this->m_hProcess, (LPVOID)addr, buffer, size, &written) && written == size;
    }

    template <class T>
    bool WriteMemory(uintptr_t addr, T v, size_t size) {
        return WriteProcessMemory(this->m_hProcess, (LPVOID)addr, &v, size, 0);
    }

    uintptr_t GetModuleBase(std::string mod) {
        uintptr_t modBaseAddr = 0;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, (DWORD)this->m_ui64PID);

        if (hSnap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 modEntry;
            modEntry.dwSize = sizeof(modEntry);

            if (Module32First(hSnap, &modEntry)) {
                do {
                    if (!strcmp(modEntry.szModule, mod.c_str())) {
                        modBaseAddr = reinterpret_cast<uintptr_t>(modEntry.modBaseAddr);
                        break;
                    }
                } while (Module32Next(hSnap, &modEntry));
            }
        }

        CloseHandle(hSnap);
        return modBaseAddr;
    }
private:
    bool OpenProcessHandle() {
        PROCESSENTRY32 entry;
        entry.dwSize = sizeof(PROCESSENTRY32);

        m_hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

        if (Process32First(m_hSnapshot, &entry) == TRUE)
        {
            while (Process32Next(m_hSnapshot, &entry) == TRUE)
            {
                if (strcmp(entry.szExeFile, "cs2.exe") == 0)
                {
                    this->m_ui64PID = entry.th32ProcessID;
                    m_hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, entry.th32ProcessID);
                }
            }
        }

        return m_hProcess != INVALID_HANDLE_VALUE;
    }

    uintptr_t m_ui64PID;
    HANDLE m_hProcess, m_hSnapshot;
};