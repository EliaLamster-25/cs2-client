#include "wradar.h"


#include "memoryproxy.h"

#include <cs2_dumper/offsets.h>
#include <cs2_dumper/client_dll.hpp>

#include <json/json.hpp>


#include <array>
#include <ixwebsocket/IXWebSocketServer.h>

WRadar::WRadar() : m_hMutex(0), m_bVerbose(false), m_strHost("0.0.0.0"), m_lPort(8080)
{
	this->m_pProxy = new Proxy();
    this->m_osFile = {};

    this->m_stRuntimeOffsets = {};
    ZeroMemory(&this->m_stRuntimeOffsets, sizeof(RO));

    this->m_stRuntimeOffsets._pGlobalVars = cs2_dumper::offsets::client_dll::dwGlobalVars;
    this->m_stRuntimeOffsets._pEntityList = cs2_dumper::offsets::client_dll::dwEntityList;
    this->m_stRuntimeOffsets._pLocalPlayerPawn = cs2_dumper::offsets::client_dll::dwLocalPlayerPawn;
    this->m_stRuntimeOffsets._pPlantedC4 = cs2_dumper::offsets::client_dll::dwPlantedC4;
    this->m_stRuntimeOffsets._pGameRules = cs2_dumper::offsets::client_dll::dwGameRules;
    
    this->m_stRuntimeOffsets._piScore = cs2_dumper::schemas::client_dll::C_Team::m_iScore;
    this->m_stRuntimeOffsets._pszTeamname = cs2_dumper::schemas::client_dll::C_Team::m_szTeamname;

    this->m_stRuntimeOffsets._p_hPawn = cs2_dumper::schemas::client_dll::CBasePlayerController::m_hPawn;

    this->m_stRuntimeOffsets._p_iszPlayerName = cs2_dumper::schemas::client_dll::CBasePlayerController::m_iszPlayerName;
    this->m_stRuntimeOffsets._p_pGameSceneNode = cs2_dumper::schemas::client_dll::C_BaseEntity::m_pGameSceneNode;
    this->m_stRuntimeOffsets._p_iHealth = cs2_dumper::schemas::client_dll::C_BaseEntity::m_iHealth;
    this->m_stRuntimeOffsets._p_iTeamNum = cs2_dumper::schemas::client_dll::C_BaseEntity::m_iTeamNum;
    this->m_stRuntimeOffsets._pWeaponServices = cs2_dumper::schemas::client_dll::C_BasePlayerPawn::m_pWeaponServices;

    this->m_stRuntimeOffsets._p_m_vecOrigin = cs2_dumper::schemas::client_dll::CGameSceneNode::m_vecOrigin;
    this->m_stRuntimeOffsets._p_m_angRotation = cs2_dumper::schemas::client_dll::CGameSceneNode::m_angRotation;

    this->m_stRuntimeOffsets._p_bBombPlanted = cs2_dumper::schemas::client_dll::C_CSGameRules::m_bBombPlanted;


    this->m_stRuntimeOffsets._p_hMyWeapons = cs2_dumper::schemas::client_dll::CPlayer_WeaponServices::m_hMyWeapons;

    this->m_stRuntimeOffsets._p_AttributeManager = cs2_dumper::schemas::client_dll::C_EconEntity::m_AttributeManager;
    this->m_stRuntimeOffsets._pItem = cs2_dumper::schemas::client_dll::C_AttributeContainer::m_Item;
    this->m_stRuntimeOffsets._piItemDefinitionIndex = cs2_dumper::schemas::client_dll::C_EconItemView::m_iItemDefinitionIndex;

    this->m_stRuntimeOffsets._pGameEntitySystem = cs2_dumper::offsets::client_dll::dwGameEntitySystem;
    this->m_stRuntimeOffsets._nHighestEntityIndex = cs2_dumper::offsets::client_dll::dwGameEntitySystem_highestEntityIndex;

}

WRadar::~WRadar()
{
    ReleaseMutex(m_hMutex);
    CloseHandle(m_hMutex);

    delete m_pProxy;
}

class Vector3
{
public:
    float x;
    float y;
    float z;
};

class Vector2
{
public:
    float x;
    float y;
};

struct C_NetworkUtlVectorBase {
    uint32_t count;
    uint32_t pad;
    uintptr_t data;
};


void WRadar::Start() {
    this->Logln("Starting WRadar...");

    HANDLE hMutex = CreateMutex(NULL, TRUE, "WRadar");
    if (hMutex == NULL) {
        this->Logln(std::format("Failed to create the mutex ({})", GetLastError()).c_str());

        ShowWindow(GetConsoleWindow(), SW_SHOW);
        MessageBoxA(GetConsoleWindow(), std::format("There was an error creating the mutex ({})", GetLastError()).c_str(), "wradar", MB_ICONERROR);
        return;
    }
    
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        this->Logln("Another instance is already running!");
        ShowWindow(GetConsoleWindow(), SW_SHOW);
        MessageBoxA(GetConsoleWindow(), "Another instance is already running! You can now open your browser and watch.", "wradar", MB_ICONERROR);
        return;
    }

    ix::initNetSystem();
    this->Logln("Initalized Windows Socket 2 Library");

    ix::WebSocketServer server(8080, "0.0.0.0");
    server.setOnClientMessageCallback([](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& socket, const ix::WebSocketMessagePtr& message) {
        if (message->type == ix::WebSocketMessageType::Message) {
            std::cout << "Received: " << message->str << std::endl;
        }
    });

    server.listen();
    server.start();

    this->Logln(std::format("Web Socket Server started on ({}:{})", this->m_strHost, this->m_lPort));

    for (;;) {
        static bool once = false;

        if (this->m_pProxy->GetModuleBase("client.dll") == 0) {
            if (!once) {
                this->Logln("CS2 has not been started yet!");
                once = true;
            }

            delete this->m_pProxy;
            this->m_pProxy = new Proxy();

            Sleep(5000);
            continue;
        }

        if (once) {
            this->Logln("CS2 has been found!");
            once = false;
        }


        uintptr_t pGlobalVars = m_pProxy->ReadMemory<uintptr_t>(m_pProxy->GetModuleBase("client.dll") + this->m_stRuntimeOffsets._pGlobalVars);
        if (!pGlobalVars) {
            this->Logln("Couldn't find GlobalVars this may mean your offsets are outdated! Try downloading the newest ones and running wradar with the -i <file> or --input <file> arguments!");

            ShowWindow(GetConsoleWindow(), SW_SHOW);
            MessageBoxA(GetConsoleWindow(), "Couldn't find GlobalVars this may mean your offsets are outdated! Try downloading the newest ones and running wradar with the -i <file> or --input <file> arguments!", "wradar", MB_ICONERROR | MB_OK);
            return;
        }
        std::string map = (m_pProxy->ReadMemory<std::array<char, 32>>(m_pProxy->ReadMemory<uintptr_t>(pGlobalVars + 0x178)).data());
        if (map.ends_with(".vpk")) {
            map = map.substr(5, (map.size() - 5) - 4);
        } else {
            map = "maps/<empty>.vpk";
        }

        uintptr_t localPlayerPawn = m_pProxy->ReadMemory<uintptr_t>(m_pProxy->GetModuleBase("client.dll") + this->m_stRuntimeOffsets._pLocalPlayerPawn);
        

        uintptr_t entityList = m_pProxy->ReadMemory<uintptr_t>(m_pProxy->GetModuleBase("client.dll") + this->m_stRuntimeOffsets._pEntityList);
        auto entity_from_handle = [](uintptr_t pointer) {
            return (pointer & 0x7FFF);
        };

        nlohmann::json info;
        info["map"] = map;
        info["score"] = nlohmann::json::array();

        uintptr_t pNetworkGameClient = m_pProxy->ReadMemory<uintptr_t>(m_pProxy->GetModuleBase("client.dll") + this->m_stRuntimeOffsets._pGameEntitySystem);
        int _highestEntityIndex = m_pProxy->ReadMemory<int>(pNetworkGameClient + this->m_stRuntimeOffsets._nHighestEntityIndex);

        for (int i = 64; i < _highestEntityIndex; i++)
        {
            uintptr_t listEntry = m_pProxy->ReadMemory<uintptr_t>(entityList + (0x8 * (entity_from_handle(i) >> 9)) + 0x10);
            if (!listEntry)
                continue;
            uintptr_t listEntry2 = m_pProxy->ReadMemory<uintptr_t>(listEntry + 120 * (i & 0x1FF));
            if (!listEntry2)
                continue;

            int teamScore = m_pProxy->ReadMemory<int>(listEntry2 + this->m_stRuntimeOffsets._piScore);
            uintptr_t teamName = m_pProxy->ReadMemory<uintptr_t>(listEntry2 + this->m_stRuntimeOffsets._pszTeamname);
            if (!teamName) {
                continue;
            }

            auto buffer = m_pProxy->ReadMemory<std::array<char, 128>>(listEntry2 + this->m_stRuntimeOffsets._pszTeamname);
            if (strcmp(buffer.data(), "CT") == 0 || strcmp(buffer.data(), "TERRORIST") == 0) {
                nlohmann::json score;
                score["team"] = buffer.data();
                score["score"] = teamScore;
                info["score"].push_back(score);
            }
        }

        info["players"] = nlohmann::json::array();
        for (int i = 1; i < 64; i++) {
            uintptr_t listEntry = m_pProxy->ReadMemory<uintptr_t>(entityList + (0x8 * (entity_from_handle(i) >> 9)) + 0x10);
            if (!listEntry)
                continue;

            uintptr_t playerController = m_pProxy->ReadMemory<uintptr_t>(listEntry + 112 * (i & 0x1FF));
            if (!playerController)
                continue;

            uintptr_t pawnHandle = m_pProxy->ReadMemory<uintptr_t>(playerController + this->m_stRuntimeOffsets._p_hPawn);
            if (!pawnHandle)
                continue;

            uintptr_t listEntry2 = m_pProxy->ReadMemory<uintptr_t>(entityList + 0x8 * ((entity_from_handle(pawnHandle)) >> 9) + 16);
            if (!listEntry2)
                continue;

            uintptr_t pCSPlayerPawn = m_pProxy->ReadMemory<uintptr_t>(listEntry2 + 112 * (pawnHandle & 0x1FF));
            if (!pCSPlayerPawn)
                continue;


            auto buffer = m_pProxy->ReadMemory<std::array<char, 128>>(playerController + this->m_stRuntimeOffsets._p_iszPlayerName);

            uintptr_t pGameSceneNode = m_pProxy->ReadMemory<uintptr_t>(pCSPlayerPawn + this->m_stRuntimeOffsets._p_pGameSceneNode);

            int iHealth = m_pProxy->ReadMemory<int>(pCSPlayerPawn + this->m_stRuntimeOffsets._p_iHealth);
            if (iHealth < 1)
                continue;
            int iTeam = m_pProxy->ReadMemory<int>(pCSPlayerPawn + this->m_stRuntimeOffsets._p_iTeamNum);

            Vector3 vector = m_pProxy->ReadMemory<Vector3>(
                pGameSceneNode + this->m_stRuntimeOffsets._p_m_vecOrigin);
            Vector3 eyeAngle = m_pProxy->ReadMemory<Vector3>(
                pGameSceneNode + this->m_stRuntimeOffsets._p_m_angRotation);

            nlohmann::json player;
            player["id"] = i;
            player["name"] = buffer.data();
            player["team"] = iTeam;
            player["health"] = iHealth;
            player["position"] = nlohmann::json::array();
            player["angle"] = nlohmann::json::array();
            player["stats"] = nlohmann::json::array();
            player["weapons"] = nlohmann::json::array();

            nlohmann::json pos;
            pos["x"] = vector.x;
            pos["y"] = vector.y;
            pos["z"] = vector.z;
            player["position"].push_back(pos);

            nlohmann::json angle;
            angle["x"] = eyeAngle.x;
            angle["y"] = eyeAngle.y;
            angle["z"] = eyeAngle.z;
            player["angle"].push_back(angle);

            nlohmann::json stats;
            angle["kills"] = 0;
            angle["assists"] = 0;
            angle["deaths"] = 0;
            player["stats"].push_back(stats);

            uintptr_t weaponServices = m_pProxy->ReadMemory<uintptr_t>(pCSPlayerPawn + this->m_stRuntimeOffsets._pWeaponServices);
            C_NetworkUtlVectorBase hMyWeapons = m_pProxy->ReadMemory<C_NetworkUtlVectorBase>(weaponServices + this->m_stRuntimeOffsets._p_hMyWeapons);
            for (unsigned int i = 0; i < hMyWeapons.count; i++) {
                uint32_t handle = m_pProxy->ReadMemory<uint32_t>(hMyWeapons.data + i * sizeof(uint32_t));
                if (!handle)
                    continue;

                int entityIndex = handle & 0x7FF;
                uintptr_t listEntry = m_pProxy->ReadMemory<uintptr_t>(entityList + (0x8 * (entity_from_handle(entityIndex) >> 9)) + 0x10);
                if (!listEntry)
                    continue;
                uintptr_t listEntry2 = m_pProxy->ReadMemory<uintptr_t>(listEntry + 112 * (entityIndex & 0x1FF));
                if (!listEntry2)
                    continue;

                short defIndex = m_pProxy->ReadMemory<short>(
                    listEntry2 + this->m_stRuntimeOffsets._p_AttributeManager +
                    this->m_stRuntimeOffsets._pItem +
                    this->m_stRuntimeOffsets._piItemDefinitionIndex
                );

                nlohmann::json weapon;
                weapon["id"] = defIndex;
                player["weapons"].push_back(weapon);
            }

            info["players"].push_back(player);

        }

        uintptr_t gameRules = m_pProxy->ReadMemory<uintptr_t>(m_pProxy->GetModuleBase("client.dll") + this->m_stRuntimeOffsets._pGameRules);
        bool bombPlanted = m_pProxy->ReadMemory<bool>(gameRules + this->m_stRuntimeOffsets._p_bBombPlanted);

        info["bomb"] = nlohmann::json::array();

        nlohmann::json bombInfo;
        bombInfo["planted"] = bombPlanted;
        if (bombPlanted) {
            uintptr_t c4 = m_pProxy->ReadMemory<uintptr_t>(m_pProxy->ReadMemory<uintptr_t>(m_pProxy->GetModuleBase("client.dll") + this->m_stRuntimeOffsets._pPlantedC4));
            uintptr_t gamescene = m_pProxy->ReadMemory<uintptr_t>(c4 + this->m_stRuntimeOffsets._p_pGameSceneNode);
            Vector3 pos = m_pProxy->ReadMemory<Vector3>(gamescene + this->m_stRuntimeOffsets._p_m_vecOrigin);

            bombInfo["x"] = pos.x;
            bombInfo["y"] = pos.y;
            bombInfo["z"] = pos.z;
        }

        info["bomb"].push_back(bombInfo);

        for (auto& cl : server.getClients()) {
            cl->sendBinary(nlohmann::json::to_msgpack(info));
        }

        std::this_thread::sleep_for(std::chrono::nanoseconds(1));
	}
}

void WRadar::Logln(std::string format)
{
    if (this->m_osFile)
        this->m_osFile << "[" << std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())) << "] " << format << std::endl;
    

    if (!this->m_bVerbose)
        return;

    std::cout << "[" << std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())) << "] " << format << std::endl;
}

void WRadar::OpenFile(std::filesystem::path file)
{
    this->m_osFile.open(file);
}

