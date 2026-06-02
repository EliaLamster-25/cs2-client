#pragma once

#include <ixwebsocket/IXSocketServer.h>

#include <filesystem>
#include <fstream>

class Proxy;

class WRadar {
public:
	WRadar();
	~WRadar();

	void Start();

	bool m_bVerbose;
	std::string m_strHost;
	int m_lPort;
	std::ofstream m_osFile;


	void Logln(std::string format);
	void OpenFile(std::filesystem::path file);

	struct RO {
		uintptr_t _pGlobalVars;
		uintptr_t _pLocalPlayerPawn;
		uintptr_t _pEntityList;
		uintptr_t _pPlantedC4;
		uintptr_t _pGameRules;

		uintptr_t _piScore;
		uintptr_t _pszTeamname;

		uintptr_t _p_hPawn;

		uintptr_t _p_iszPlayerName;
		uintptr_t _p_pGameSceneNode;
		uintptr_t _p_iHealth;
		uintptr_t _p_iTeamNum;
		uintptr_t _pWeaponServices;


		// GameSceneNode
		uintptr_t _p_m_vecOrigin;
		uintptr_t _p_m_angRotation;

		// GameRules
		uintptr_t _p_bBombPlanted;

		// Weapon Services
		uintptr_t _p_hMyWeapons;

		//
		uintptr_t _p_AttributeManager;
		uintptr_t _pItem;
		uintptr_t _piItemDefinitionIndex;

		uintptr_t _pGameEntitySystem;
		uintptr_t _nHighestEntityIndex;
	} m_stRuntimeOffsets;
private:
	Proxy* m_pProxy;

	HANDLE m_hMutex;
};