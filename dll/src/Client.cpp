#include <filesystem>
#include <cpprest/http_client.h>
#include <Windows.h>
#include <minizip/unzip.h>
#include "LuaAPI.h"
#include "ChatClient.h"
#include "Client.h"
#include "ClientHandlers.h"
#include "EventManager.h"
#include "Version.h"
#include "JSONObject.h"
#include "URI.h"
#include "Log.h"

Client& Client::GetInstance()
{
    static Client instance;
    if (!instance.IsValid())
    {
        instance.ReadDataFromPipe();
    }
    return instance;
}

bool ReadInt16FromPipe(HANDLE pipe, uint16_t& value)
{
    DWORD bytesRead;
    uint8_t buffer[2];

    if (!ReadFile(pipe, &buffer, 2, &bytesRead, NULL) || (bytesRead != 2))
        return false;

    value = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8);

    return true;
}

bool ReadInt32FromPipe(HANDLE pipe, uint32_t& value)
{
    DWORD bytesRead;
    uint8_t buffer[4];

    if (!ReadFile(pipe, &buffer, 4, &bytesRead, NULL) || (bytesRead != 4))
        return false;

    value = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) | ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);

    return true;
}

bool ReadStringFromPipe(HANDLE pipe, std::string& str)
{
    DWORD bytesRead;
    uint32_t length;

    if (!ReadInt32FromPipe(pipe, length))
        return false;

    char* buffer = new char[length + 1];
    if (!ReadFile(pipe, (LPVOID)buffer, length, &bytesRead, NULL) || (bytesRead != length))
    {
        delete[] buffer;
        return false;
    }

    buffer[length] = '\0';
    str = buffer;

    delete[] buffer;
    return true;
}

bool ReadWideStringFromPipe(HANDLE pipe, std::wstring& str)
{
    uint32_t length;

    if (!ReadInt32FromPipe(pipe, length))
        return false;

    wchar_t* buffer = new wchar_t[length + 1];
    for (size_t i = 0; i < length; ++i)
    {
        if (!ReadInt16FromPipe(pipe, (uint16_t&)buffer[i]))
        {
            delete[] buffer;
            return false;
        }
    }

    buffer[length] = '\0';
    str = buffer;

    delete[] buffer;
    return true;
}

bool ReadSeasonsFromPipe(HANDLE pipe, std::vector<SeasonInfo>& seasons)
{
    uint32_t count;
    if (!ReadInt32FromPipe(pipe, count))
        return false;

    for (uint32_t i = 0; i < count; ++i)
    {
        SeasonInfo season;

        if (!ReadInt32FromPipe(pipe, season._seasonID) ||
            !ReadInt32FromPipe(pipe, season._seasonType) ||
            !ReadStringFromPipe(pipe, season._modName) ||
            !ReadStringFromPipe(pipe, season._displayName) ||
            !ReadStringFromPipe(pipe, season._participationToken))
            return false;

        seasons.push_back(season);
    }
    return true;
}

bool ExtractZIPUpdate(const std::filesystem::path& path)
{
    const char* pathString = path.u8string().c_str();
    unzFile zipFile = unzOpen(pathString);
    if ((zipFile) && (unzLocateFile(zipFile, "GDCommunityLauncher.exe", 0) != UNZ_END_OF_LIST_OF_FILE))
    {
        std::filesystem::path filenamePath = std::filesystem::current_path() / "GDCommunityLauncher.exe";
        std::filesystem::path tempPath = filenamePath;
        tempPath += ".tmp";

        std::ofstream out(tempPath, std::ifstream::binary | std::ifstream::out);

        if ((!out.is_open()) || (unzOpenCurrentFile(zipFile) != UNZ_OK))
        {
            Logger::LogMessage(LOG_LEVEL_ERROR, "Failed to extract files from \"%\"", pathString);
            return false;
        }

        int bytesRead = 0;
        char buffer[1024];
        do
        {
            bytesRead = unzReadCurrentFile(zipFile, buffer, 1024);
            if (bytesRead > 0)
            {
                out.write(buffer, bytesRead);
            }
        }
        while (bytesRead > 0);

        out.close();
        unzCloseCurrentFile(zipFile);
        unzClose(zipFile);

        std::filesystem::rename(tempPath, filenamePath);

        return true;
    }
    else
    {
        Logger::LogMessage(LOG_LEVEL_ERROR, "Could not open \"%\" for updating", pathString);
        return false;
    }
}

void Client::ReadDataFromPipe()
{
    if (!IsValid())
    {
        HANDLE pipe = GetStdHandle(STD_INPUT_HANDLE);

        std::string updatePath;
        if (!ReadStringFromPipe(pipe, _data._username) ||
            !ReadStringFromPipe(pipe, _data._authToken) ||
            !ReadStringFromPipe(pipe, _data._refreshToken) ||
            !ReadStringFromPipe(pipe, _data._hostName) ||
            !ReadStringFromPipe(pipe, updatePath) ||
            !ReadSeasonsFromPipe(pipe, _data._seasons))
        {
            Logger::LogMessage(LOG_LEVEL_ERROR, "Failed to read client data from stdin pipe.");
            return;
        }

        CloseHandle(pipe);

        if ((!updatePath.empty()) && (!ExtractZIPUpdate(updatePath)))
        {
            Logger::LogMessage(LOG_LEVEL_ERROR, "Failed to update GDCommunityLauncher.exe");
            return;
        }

        UpdateLeagueInfoText();
        UpdateVersionInfoText();
    }
}

void Client::UpdateRefreshToken()
{
    pplx::create_task([]()
    {
        Client& client = Client::GetInstance();
        try
        {
            URI endpoint = URI(client.GetHostName()) / "api" / "Account" / "refresh-token";
            web::http::client::http_client httpClient((utility::string_t)endpoint);

            web::json::value requestBody;
            requestBody[U("refreshToken")] = JSONString(client.GetRefreshToken());

            web::http::http_request request(web::http::methods::POST);
            request.set_body(requestBody);

            web::http::http_response response = httpClient.request(request).get();
            if (response.status_code() == web::http::status_codes::OK)
            {
                web::json::value responseBody = response.extract_json().get();
                web::json::value authTokenValue = responseBody[U("access_token")];
                web::json::value refreshTokenValue = responseBody[U("refresh_token")];
                if ((!authTokenValue.is_null()) && (!refreshTokenValue.is_null()))
                {
                    client._data._authToken = JSONString(authTokenValue.as_string());
                    client._data._refreshToken = JSONString(refreshTokenValue.as_string());
                }
            }
            else
            {
                throw std::runtime_error("Server responded with status code " + std::to_string(response.status_code()));
            }
        }
        catch (const std::exception& ex)
        {
            Logger::LogMessage(LOG_LEVEL_WARN, "Failed to refresh token: %", ex.what());
        }
    });
}

void Client::UpdateConnectionStatus()
{
    Client& client = Client::GetInstance();
    try
    {
        URI endpoint = URI(client.GetHostName()) / "api" / "Account" / "status";

        web::http::client::http_client httpClient((utility::string_t)endpoint);
        web::http::http_request request(web::http::methods::GET);

        web::http::http_response response = httpClient.request(request).get();
        bool status = (response.status_code() == web::http::status_codes::OK);
        if (client._online != status)
        {
            client._online = status;
            client.UpdateLeagueInfoText();

            if (status)
                EventManager::Publish(GDCL_EVENT_CONNECT);
            else
                EventManager::Publish(GDCL_EVENT_DISCONNECT);
        }
    }
    catch (const std::exception& ex)
    {
        if (client._online == true)
        {
            Logger::LogMessage(LOG_LEVEL_ERROR, "Failed to query server status: % %", client._online, ex.what());
            client._online = false;
            client.UpdateLeagueInfoText();
            EventManager::Publish(GDCL_EVENT_DISCONNECT);
        }
    }
}

void Client::UpdateVersionInfoText()
{
    typedef const char* (__thiscall* GetVersionProto)(void*);

    _versionInfoText.clear();

    GetVersionProto callback = (GetVersionProto)HookManager::GetOriginalFunction("Engine.dll", EngineAPI::EAPI_NAME_GET_VERSION);
    PULONG_PTR engine = EngineAPI::GetEngineHandle();

    if ((callback) && (engine))
    {
        const char* result = callback((void*)*engine);
        _versionInfoText = result;
        _versionInfoText += "\n{^F}GDCL v";
        _versionInfoText += GDCL_VERSION;
        _versionInfoText += " (";
        _versionInfoText += GetUsername();
        _versionInfoText += ")";
    }
}

void Client::UpdateLeagueInfoText()
{
    _leagueInfoText.clear();

    if (_activeSeason)
    {
        _leagueInfoText += L"\n";
        _leagueInfoText += std::wstring(_activeSeason->_displayName.begin(), _activeSeason->_displayName.end());
    }
    _leagueInfoText += L"\n";
    _leagueInfoText += std::wstring(_data._username.begin(), _data._username.end());

    if ((_online) && (_activeSeason))
    {
        if (GameAPI::IsCloudStorageEnabled())
        {
            _leagueInfoText += L" {^Y}(Disable Cloud Saving)";
        }
        else if (EngineAPI::IsMultiplayer())
        {
            _leagueInfoText += L" {^Y}(Multiplayer)";
        }
        else
        {
            if ((_points > 0) && (_rank > 0))
            {
                _leagueInfoText += L" {^L}(Rank ";
                _leagueInfoText += std::to_wstring(_rank);
                _leagueInfoText += L" ~ ";
            }
            else
            {
                _leagueInfoText += L" {^L}(";
            }
            _leagueInfoText += std::to_wstring(_points);
            _leagueInfoText += L" points)";
        }
    }
    else
    {
        _leagueInfoText += L" {^R}(Offline)";
    }
}

void Client::SetActiveSeason(const std::string& modName, bool hardcore)
{
    _activeSeason = nullptr;
    for (size_t i = 0; i < _data._seasons.size(); ++i)
    {
        SeasonInfo& season = _data._seasons[i];
        if ((modName == season._modName) && ((1 + hardcore) == season._seasonType))
        {
            _activeSeason = &season;
            break;
        }
    }
    UpdateLeagueInfoText();
}

void Client::SetActiveCharacter(const std::wstring& name, bool hasToken)
{
    _activeCharacter._name = name;
    _activeCharacter._hasToken = hasToken;
}

void Client::UpdateSeasonStanding()
{
    pplx::create_task([this]()
    {
        try
        {
            URI endpoint = URI(GetHostName()) / "api" / "Season" / "participant" / std::to_string(GetParticipantID()) / "standing";
            web::http::client::http_client httpClient((utility::string_t)endpoint);
            web::http::http_request request(web::http::methods::GET);

            web::http::http_response response = httpClient.request(request).get();
            switch (response.status_code())
            {
                case web::http::status_codes::OK:
                {
                    web::json::value responseBody = response.extract_json().get();
                    web::json::value pointTotal = responseBody[U("pointTotal")];
                    web::json::value rank = responseBody[U("rank")];

                    _points = pointTotal.as_integer();
                    _rank = rank.as_integer();
                    UpdateLeagueInfoText();
                    break;
                }
                default:
                {
                    throw std::runtime_error("Server responded with status code " + std::to_string(response.status_code()));
                }
            }
        }
        catch (const std::exception& ex)
        {
            Logger::LogMessage(LOG_LEVEL_WARN, "Failed to update season standing: %", ex.what());
        }
    });
}

bool Client::Initialize()
{
    Logger::SetMinimumLogLevel(LOG_LEVEL_WARN);
    LuaAPI::Initialize();

    // Initialize the chat client in a separate thread
    pplx::task<void> task = pplx::create_task([]()
    {
        ChatClient& chatClient = ChatClient::GetInstance();
    });

    // Initialize the game engine hooks
    if (!HookManager::CreateHook("Engine.dll", EngineAPI::EAPI_NAME_GET_VERSION, &HandleGetVersion) ||
        !HookManager::CreateHook("Engine.dll", EngineAPI::EAPI_NAME_RENDER, &HandleRender) ||
        !HookManager::CreateHook("Engine.dll", EngineAPI::EAPI_NAME_LOAD_WORLD, &HandleLoadWorld) ||
        !HookManager::CreateHook("Engine.dll", EngineAPI::EAPI_NAME_SET_REGION_OF_NOTE, &HandleSetRegionOfNote) ||
        !HookManager::CreateHook("Engine.dll", EngineAPI::EAPI_NAME_HANDLE_KEY_EVENT, &HandleKeyEvent) ||
        !HookManager::CreateHook("Engine.dll", EngineAPI::EAPI_NAME_RENDER_STYLED_TEXT_2D, &HandleRenderStyledText2D) ||
        !HookManager::CreateHook("Engine.dll", EngineAPI::EAPI_NAME_LUA_INITIALIZE, &HandleLuaInitialize) ||
        !HookManager::CreateHook("Game.dll", GameAPI::GAPI_NAME_SET_MAIN_PLAYER, &HandleSetMainPlayer) ||
        !HookManager::CreateHook("Game.dll", GameAPI::GAPI_NAME_ON_CARAVAN_INTERACT, &HandleSetTransferOpen) ||
        !HookManager::CreateHook("Game.dll", GameAPI::GAPI_NAME_SAVE_NEW_FORMAT_DATA, &HandleSaveNewFormatData) ||
        !HookManager::CreateHook("Game.dll", GameAPI::GAPI_NAME_SAVE_TRANSFER_STASH, &HandleSaveTransferStash) ||
        !HookManager::CreateHook("Game.dll", GameAPI::GAPI_NAME_LOAD_TRANSFER_STASH, &HandleLoadTransferStash) ||
        !HookManager::CreateHook("Game.dll", GameAPI::GAPI_NAME_BESTOW_TOKEN, &HandleBestowToken) ||
        !HookManager::CreateHook("Game.dll", GameAPI::GAPI_NAME_UNLOAD_WORLD, &HandleUnloadWorld) ||
        !HookManager::CreateHook("Game.dll", GameAPI::GAPI_NAME_SEND_CHAT_MESSAGE, &HandleSendChatMessage) ||
        !HookManager::CreateHook("Game.dll", GameAPI::GAPI_NAME_SYNC_DUNGEON_PROGRESS, &HandleSyncDungeonProgress))
    {
        Logger::LogMessage(LOG_LEVEL_ERROR, "Failed to create one or more game hooks.");
        return false;
    }

    // Initialize threads to handle refreshing the server token/connection status
    static auto refreshServerTokenThread = std::make_shared<UpdateThread<>>(&Client::UpdateRefreshToken, 1000, 1800000);
    static auto connectionStatusThread = std::make_shared<UpdateThread<>>(&Client::UpdateConnectionStatus, 1000, 30000);

    refreshServerTokenThread->Update(0);
    connectionStatusThread->Update(0);

    UpdateVersionInfoText();

    return true;
}

void Client::Cleanup()
{
    LuaAPI::Cleanup();

    HookManager::DeleteHook("Engine.dll", EngineAPI::EAPI_NAME_GET_VERSION);
    HookManager::DeleteHook("Engine.dll", EngineAPI::EAPI_NAME_RENDER);
    HookManager::DeleteHook("Engine.dll", EngineAPI::EAPI_NAME_LOAD_WORLD);
    HookManager::DeleteHook("Engine.dll", EngineAPI::EAPI_NAME_SET_REGION_OF_NOTE);
    HookManager::DeleteHook("Engine.dll", EngineAPI::EAPI_NAME_HANDLE_KEY_EVENT);
    HookManager::DeleteHook("Engine.dll", EngineAPI::EAPI_NAME_RENDER_STYLED_TEXT_2D);
    HookManager::DeleteHook("Engine.dll", EngineAPI::EAPI_NAME_LUA_INITIALIZE);
    HookManager::DeleteHook("Game.dll", GameAPI::GAPI_NAME_SET_MAIN_PLAYER);
    HookManager::DeleteHook("Game.dll", GameAPI::GAPI_NAME_ON_CARAVAN_INTERACT);
    HookManager::DeleteHook("Game.dll", GameAPI::GAPI_NAME_SAVE_NEW_FORMAT_DATA);
    HookManager::DeleteHook("Game.dll", GameAPI::GAPI_NAME_SAVE_TRANSFER_STASH);
    HookManager::DeleteHook("Game.dll", GameAPI::GAPI_NAME_LOAD_TRANSFER_STASH);
    HookManager::DeleteHook("Game.dll", GameAPI::GAPI_NAME_BESTOW_TOKEN);
    HookManager::DeleteHook("Game.dll", GameAPI::GAPI_NAME_UNLOAD_WORLD);
    HookManager::DeleteHook("Game.dll", GameAPI::GAPI_NAME_SEND_CHAT_MESSAGE);
    HookManager::DeleteHook("Game.dll", GameAPI::GAPI_NAME_SYNC_DUNGEON_PROGRESS);
}