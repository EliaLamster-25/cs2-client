//#define WRADAR_USE_DRIVER
#include "wradar.h"

#include <string>
#include <vector>
#include <sstream>
#include <iostream>

#include <json/json.hpp>	

int main(int argc, char** argv) {
	ShowWindow(GetConsoleWindow(), SW_HIDE);

	WRadar* wradar = new WRadar();

	auto logerr = [](std::string format) {
		std::cerr << "[" << std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())) << "] [ERROR] " << format << std::endl;
	};

	

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--input") == 0) {
			if ((argc - 1) < (i + 1) && argv[i + 1][0] == '-') {
				logerr("No input file specified!");
				return 801;
			}
			std::string argument = argv[i + 1];

			auto split = [&](std::string str, char delim) {
				std::vector<std::string> tokens;
				std::string token;
				std::istringstream ss(str);
				while (std::getline(ss, token, delim)) tokens.push_back(token);
				return tokens;
				};

			auto logsuccess = [](std::string format) {
				std::cerr << "[" << std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())) << "] [INFO] " << format << std::endl;
				};

			std::vector<std::string> files;
			if (argument.contains(",")) {
				files = split(argument, ',');
			}
			else
				files.push_back(argument);

			for (auto& file : files) {
				std::filesystem::path offset = file;
				if (!std::filesystem::exists(offset)) {
					logerr(std::format("[INPUT] File '{}' does not exist!", offset.string()));
					return 802;
				}

				std::ifstream filestream(offset);
				if (!filestream.is_open()) {
					logerr(std::format("[INPUT] Could not open file '{}'!", offset.string()));
					return 803;
				}

				nlohmann::json json;
				try {
					filestream >> json;
				}
				catch (nlohmann::json::parse_error& e) {
					logerr(std::format("[INPUT] Could not parse file '{}' because of error: {}!", offset.string(), e.what()));
					return 804;
				}

				if (json.contains("client.dll")) {
					try {
						if (json["client.dll"].contains("classes")) {
							wradar->m_stRuntimeOffsets._piScore = json["client.dll"]["classes"]["C_Team"]["fields"]["m_iScore"];
							wradar->m_stRuntimeOffsets._pszTeamname = json["client.dll"]["classes"]["C_Team"]["fields"]["m_szTeamname"];

							wradar->m_stRuntimeOffsets._p_hPawn = json["client.dll"]["classes"]["CBasePlayerController"]["fields"]["m_hPawn"];

							wradar->m_stRuntimeOffsets._p_iszPlayerName = json["client.dll"]["classes"]["CBasePlayerController"]["fields"]["m_iszPlayerName"];
							wradar->m_stRuntimeOffsets._p_pGameSceneNode = json["client.dll"]["classes"]["C_BaseEntity"]["fields"]["m_pGameSceneNode"];
							wradar->m_stRuntimeOffsets._p_iHealth = json["client.dll"]["classes"]["C_BaseEntity"]["fields"]["m_iHealth"];
							wradar->m_stRuntimeOffsets._p_iTeamNum = json["client.dll"]["classes"]["C_BaseEntity"]["fields"]["m_iTeamNum"];
							wradar->m_stRuntimeOffsets._pWeaponServices = json["client.dll"]["classes"]["C_BasePlayerPawn"]["fields"]["m_pWeaponServices"];

							wradar->m_stRuntimeOffsets._p_m_vecOrigin = json["client.dll"]["classes"]["CGameSceneNode"]["fields"]["m_vecOrigin"];
							wradar->m_stRuntimeOffsets._p_m_angRotation = json["client.dll"]["classes"]["CGameSceneNode"]["fields"]["m_angRotation"];

							wradar->m_stRuntimeOffsets._p_bBombPlanted = json["client.dll"]["classes"]["C_CSGameRules"]["fields"]["m_bBombPlanted"];

							wradar->m_stRuntimeOffsets._p_hMyWeapons = json["client.dll"]["classes"]["CPlayer_WeaponServices"]["fields"]["m_hMyWeapons"];

							wradar->m_stRuntimeOffsets._p_AttributeManager = json["client.dll"]["classes"]["C_EconEntity"]["fields"]["m_AttributeManager"];
							wradar->m_stRuntimeOffsets._pItem = json["client.dll"]["classes"]["C_AttributeContainer"]["fields"]["m_Item"];
							wradar->m_stRuntimeOffsets._piItemDefinitionIndex = json["client.dll"]["classes"]["C_EconItemView"]["fields"]["m_iItemDefinitionIndex"];

							logsuccess(std::format("[INPUT] Successfully loaded schemas for client.dll from '{}'", offset.string()));
						}
						else {
							wradar->m_stRuntimeOffsets._pGlobalVars = json["client.dll"]["dwGlobalVars"];
							wradar->m_stRuntimeOffsets._pEntityList = json["client.dll"]["dwEntityList"];
							wradar->m_stRuntimeOffsets._pLocalPlayerPawn = json["client.dll"]["dwLocalPlayerPawn"];
							wradar->m_stRuntimeOffsets._pPlantedC4 = json["client.dll"]["dwPlantedC4"];
							wradar->m_stRuntimeOffsets._pGameRules = json["client.dll"]["dwGameRules"];

							logsuccess(std::format("[INPUT] Successfully loaded offsets for client.dll from '{}'", offset.string()));
						}

					}
					catch (nlohmann::json::out_of_range& e) {
						logerr(std::format("[INPUT] Could not parse file '{}' because of key-error: {}!", offset.string(), e.what()));
						return 805;
					}
					catch (nlohmann::json::type_error& e) {
						logerr(std::format("[INPUT] Could not parse file '{}' because of type-error: {}!", offset.string(), e.what()));
						return 806;
					}
					catch (std::exception& e) {
						logerr(std::format("[INPUT] Could not parse file '{}' because of error: {}!", offset.string(), e.what()));
					}
				}

			}
		
		}

		if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--show") == 0) {
			ShowWindow(GetConsoleWindow(), SW_SHOW);
		}

		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			wradar->m_bVerbose = true;
		}

		if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
			if ((argc - 1) < (i + 1)) {
				logerr("No port specified!");
				return 81;
			}
			wradar->m_lPort = std::stoi(argv[i + 1]);
		}

		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) {
			if((argc - 1) < (i + 1)) {
				logerr("No host specified!");
				return 82;
			}
			wradar->m_strHost = argv[i + 1];
		}

		if (strcmp(argv[i], "-fl") == 0 || strcmp(argv[i], "--file_log") == 0) {
			std::string strFile;
			if ((argc - 1) < (i + 1)) {
				logerr("[file_log] No file specified! Defaulting to timestamp");
				strFile = std::format("log_{}.txt", time(nullptr));
			}
			else
				strFile = argv[i + 1];

			if (!std::filesystem::exists(strFile)) {
				logerr("[file_log] File specified does not exist! Creating a new one!");
			}
			wradar->OpenFile(strFile);
		}
	}

	wradar->Start();

	delete wradar;
	return 0;
}