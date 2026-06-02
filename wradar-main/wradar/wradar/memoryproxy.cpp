#include "memoryproxy.h"


Proxy::Proxy()
{
	this->m_mModuleCache = {};
#ifdef WR_D
	m_pDrv = new Driver();
#else
	m_pProc = new Proc();
#endif
}

Proxy::~Proxy()
{
#ifdef WR_D
	delete m_pDrv;
#else
	delete m_pProc;
#endif
}

uintptr_t Proxy::GetModuleBase(std::string mod)
{
	auto [it, inserted] = m_mModuleCache.try_emplace(
		mod,
#ifdef WR_D
		m_pDrv->GetModuleBase(mod)
#else
		m_pProc->GetModuleBase(mod)
#endif
	);

	return it->second;
}


