#include "Client.h"
#include "Log.h"

Client& Client::GetInstance(const ClientData& data)
{
    static Client instance(data);
    return instance;
}

bool WriteInt16ToPipe(HANDLE pipe, wchar_t value)
{
    DWORD bytesWritten;
    uint8_t buffer[2];

    buffer[0] = value & 0x00FF;
    buffer[1] = (value & 0xFF00) >> 8;

    return (WriteFile(pipe, buffer, 2, &bytesWritten, NULL) && (bytesWritten == 2));
}

bool WriteInt32ToPipe(HANDLE pipe, uint32_t value)
{
    DWORD bytesWritten;
    uint8_t buffer[4];

    buffer[0] =  value & 0x000000FF;
    buffer[1] = (value & 0x0000FF00) >> 8;
    buffer[2] = (value & 0x00FF0000) >> 16;
    buffer[3] = (value & 0xFF000000) >> 24;

    return (WriteFile(pipe, buffer, 4, &bytesWritten, NULL) && (bytesWritten == 4));
}

bool WriteStringToPipe(HANDLE pipe, const std::string& str)
{
    DWORD bytesWritten;
    uint32_t length = (uint32_t)str.length();

    if (!WriteInt32ToPipe(pipe, length))
        return false;

    if (!WriteFile(pipe, str.c_str(), length, &bytesWritten, NULL) || (bytesWritten != length))
        return false;

    return true;
}

bool WriteWideStringToPipe(HANDLE pipe, const std::wstring& str)
{
    uint32_t length = (uint32_t)str.length();
    if (!WriteInt32ToPipe(pipe, length))
        return false;

    for (size_t i = 0; i < length; ++i)
    {
        if (!WriteInt16ToPipe(pipe, str[i]))
            return false;
    }
    return true;
}

bool WriteSeasonsToPipe(HANDLE pipe, const std::vector<SeasonInfo>& seasons)
{
    uint32_t length = (uint32_t)seasons.size();
    if (!WriteInt32ToPipe(pipe, length))
        return false;

    for (size_t i = 0; i < length; ++i)
    {
        if (!WriteInt32ToPipe(pipe, seasons[i]._seasonID) ||
            !WriteInt32ToPipe(pipe, seasons[i]._seasonType) ||
            !WriteStringToPipe(pipe, seasons[i]._modName) ||
            !WriteStringToPipe(pipe, seasons[i]._displayName) ||
            !WriteStringToPipe(pipe, seasons[i]._participationToken))
            return false;
    }
    return true;
}

bool Client::WriteDataToPipe(HANDLE pipe) const
{
    if (!WriteStringToPipe(pipe, _data._username) ||
        !WriteStringToPipe(pipe, _data._password) ||
        !WriteStringToPipe(pipe, _data._authToken) ||
        !WriteStringToPipe(pipe, _data._refreshToken) ||
        !WriteStringToPipe(pipe, _data._gameURL) ||
        !WriteStringToPipe(pipe, _data._chatURL) ||
        !WriteStringToPipe(pipe, _data._branch) ||
        !WriteInt32ToPipe(pipe, _data._updateFlag) ||
        !WriteSeasonsToPipe(pipe, _data._seasons))
    {
        Logger::LogMessage(LOG_LEVEL_ERROR, "Failed to write client data to the stdin pipe.");
        return false;
    }

    CloseHandle(pipe);
    return TRUE;
}