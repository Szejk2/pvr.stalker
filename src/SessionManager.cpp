/*
 *      Copyright (C) 2016  Jamal Edey
 *      http://www.kenshisoft.com/
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Kodi; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 *  http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 */

#include "SessionManager.h"

#include "client.h"
#include "Utils.h"

#if defined(_WIN32) || defined(_WIN64)
#define usleep(usec) Sleep((DWORD)(usec)/1000)
#else
#include <unistd.h>
#endif

using namespace ADDON;
using namespace SC;

SessionManager::SessionManager() {
    m_identity = nullptr;
    m_profile = nullptr;
    m_api = nullptr;
    m_statusCallback = nullptr;
    m_authenticated = false;
    m_watchdog = nullptr;
}

SessionManager::~SessionManager() {
    m_identity = nullptr;
    m_profile = nullptr;
    m_api = nullptr;
    m_statusCallback = nullptr;

    if (m_watchdog) {
        StopWatchdog();
        delete m_watchdog;
        m_watchdog = nullptr;
    }
    m_watchdog = nullptr;
}

SError SessionManager::DoHandshake() {
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

    Json::Value parsed;

    if (!m_api->STBHandshake(parsed)) {
        XBMC->Log(LOG_ERROR, "%s: STBHandshake failed", __FUNCTION__);
        return SERROR_AUTHENTICATION;
    }

    if (parsed["js"].isMember("token"))
        SC_STR_SET(m_identity->token, parsed["js"]["token"].asCString());

    XBMC->Log(LOG_DEBUG, "%s: token=%s", __FUNCTION__, m_identity->token);

    if (parsed["js"].isMember("not_valid"))
        m_identity->valid_token = !Utils::GetIntFromJsonValue(parsed["js"]["not_valid"]);

    return SERROR_OK;
}

SError SessionManager::DoAuth() {
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

    Json::Value parsed;
    SError ret(SERROR_OK);

    if (!m_api->STBDoAuth(parsed)) {
        XBMC->Log(LOG_ERROR, "%s: STBDoAuth failed", __FUNCTION__);
        return SERROR_AUTHENTICATION;
    }

    if (parsed.isMember("js") && !parsed["js"].asBool())
        ret = SERROR_AUTHENTICATION;

    return ret;
}

SError SessionManager::GetProfile(bool authSecondStep) {
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

    Json::Value parsed;
    SError ret(SERROR_OK);

    if (!m_api->STBGetProfile(authSecondStep, parsed)) {
        XBMC->Log(LOG_ERROR, "%s: STBGetProfile failed", __FUNCTION__);
        return SERROR_AUTHENTICATION;
    }

    sc_stb_profile_defaults(m_profile);

    if (parsed["js"].isMember("store_auth_data_on_stb"))
        m_profile->store_auth_data_on_stb = Utils::GetBoolFromJsonValue(parsed["js"]["store_auth_data_on_stb"]);

    if (parsed["js"].isMember("status"))
        m_profile->status = Utils::GetIntFromJsonValue(parsed["js"]["status"]);

    SC_STR_SET(m_profile->msg, !parsed["js"].isMember("msg")
                               ? ""
                               : parsed["js"]["msg"].asCString());

    SC_STR_SET(m_profile->block_msg, !parsed["js"].isMember("block_msg")
                                     ? ""
                                     : parsed["js"]["block_msg"].asCString());

    if (parsed["js"].isMember("watchdog_timeout"))
        m_profile->watchdog_timeout = Utils::GetIntFromJsonValue(parsed["js"]["watchdog_timeout"]);

    if (parsed["js"].isMember("timeslot"))
        m_profile->timeslot = Utils::GetDoubleFromJsonValue(parsed["js"]["timeslot"]);

    XBMC->Log(LOG_DEBUG, "%s: timeslot=%f", __FUNCTION__, m_profile->timeslot);

    switch (m_profile->status) {
        case 0:
            break;
        case 2:
            ret = DoAuth();
            if (ret != SERROR_OK)
                return ret;

            return GetProfile(true);
        case 1:
        default:
            m_lastUnknownError = m_profile->msg;
            XBMC->Log(LOG_ERROR, "%s: status=%i | msg=%s | block_msg=%s", __FUNCTION__, m_profile->status,
                      m_profile->msg, m_profile->block_msg);
            return SERROR_UNKNOWN;
    }

    return ret;
}

SError SessionManager::Authenticate() {
    SError ret(SERROR_OK);
    bool wasAuthenticated(m_authenticated);
    int maxRetires(5);
    int numRetries(0);

    m_authMutex.lock();

    m_authenticated = false;
    StopWatchdog();
    m_lastUnknownError.clear();

    if (wasAuthenticated && m_statusCallback != nullptr)
        m_statusCallback(SERROR_AUTHORIZATION);

    while (!m_authenticated && ++numRetries <= maxRetires) {
        // notify once after the first try failed
        if (numRetries == 2) {
            if (m_statusCallback != nullptr)
                m_statusCallback(SERROR_AUTHENTICATION);
        }

        // don't sleep on first try
        if (numRetries > 1)
            usleep(5000000);

        if (!m_hasUserDefinedToken && SERROR_OK != (ret = DoHandshake()))
            continue;

        if (SERROR_OK != (ret = GetProfile()))
            continue;

        StartWatchdog();
        m_authenticated = true;

        if (wasAuthenticated && m_statusCallback != nullptr)
            m_statusCallback(SERROR_OK);
    }

    m_authMutex.unlock();

    return ret;
}

void SessionManager::StartWatchdog() {
    if (!m_watchdog) {
        m_watchdog = new CWatchdog((unsigned int) m_profile->timeslot, m_api, [this](SError err) {
            if (err == SERROR_AUTHORIZATION)
                Authenticate();
        });
    }

    if (m_watchdog)
        m_watchdog->Start();
}

void SessionManager::StopWatchdog() {
    if (m_watchdog)
        m_watchdog->Stop();
}
