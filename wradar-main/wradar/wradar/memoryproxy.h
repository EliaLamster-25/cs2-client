#pragma once

#ifdef WRADAR_USE_DRIVER
#include "driver.h"
#define WR_D
#else
#include "process.h"
#endif

#include <map>



class Proxy {
public:
	Proxy();
    ~Proxy();

    template <typename type>
    type ReadMemory(uintptr_t addr) {
#ifdef WR_D
        return m_pDrv->ReadMemory<type>(addr);
#else
        return m_pProc->ReadMemory<type>(addr);
#endif
    }


    template <class T>
    bool WriteMemory(uintptr_t addr, T v) {
#ifdef WR_D
        return m_pDrv->WriteMemory(addr, v);
#else
        return m_pProc->WriteMemory(addr, v);
#endif
    }

    uintptr_t GetModuleBase(std::string mod);
private:
#ifdef WR_D
    Driver* m_pDrv;
#else
    Proc* m_pProc;
#endif

    std::map<std::string, uintptr_t> m_mModuleCache;
};
