/*
This file is part of "Rigs of Rods Server" (Relay mode)

Copyright 2007   Pierre-Michel Ricordel
Copyright 2014+  Rigs of Rods Community

"Rigs of Rods Server" is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3
of the License, or (at your option) any later version.

"Rigs of Rods Server" is distributed in the hope that it will
be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar. If not, see <http://www.gnu.org/licenses/>.
*/

#include "listener.h"

#include "rornet.h"
#include "messaging.h"
#include "sequencer.h"
#include "SocketW.h"
#include "logger.h"
#include "config.h"
#include "UnicodeStrings.h"
#include "utils.h"

#include <stdexcept>
#include <sstream>
#include <stdio.h>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>

#ifdef __GNUC__

#include <stdlib.h>

#endif

namespace {

using HandshakeDeadline = std::chrono::steady_clock::time_point;

const std::chrono::microseconds HANDSHAKE_POLL_INTERVAL =
        std::chrono::milliseconds(100);

bool SetPendingSocketPollTimeout(SWInetSocket *socket,
        const HandshakeDeadline& deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - now);
    const auto poll_timeout = std::min(remaining, HANDSHAKE_POLL_INTERVAL);
    if (poll_timeout.count() < 1) {
        return false;
    }
    socket->set_timeout(
            static_cast<Uint32>(poll_timeout.count() / 1000000),
            static_cast<Uint32>(poll_timeout.count() % 1000000));
    return true;
}

void ClosePendingSocketGracefully(SWInetSocket *&socket)
{
    if (socket == nullptr) {
        return;
    }
    SWBaseSocket::SWBaseError error;
    socket->disconnect(&error);
    delete socket;
    socket = nullptr;
}

void AbortPendingSocketForShutdown(SWInetSocket *&socket)
{
    if (socket == nullptr) {
        return;
    }
    // The listener thread is the sole owner here. SocketW::close_fd() closes
    // immediately, records myfd=-1, and resets progress state, so deleting the
    // SocketW object below cannot close the native descriptor a second time.
    socket->close_fd();
    delete socket;
    socket = nullptr;
}

} // namespace

int Listener::ReceiveHandshakeMessage(SWInetSocket *socket, int *type, int *source,
        unsigned int *stream_id, unsigned int *payload_len, char *payload,
        unsigned int payload_capacity)
{
    const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
    RoRnet::Header header;
    char *header_data = reinterpret_cast<char *>(&header);
    unsigned int header_received = 0;
    while (header_received < sizeof(header)) {
        if (GetThreadState() != ThreadState::RUNNING ||
                std::chrono::steady_clock::now() >= deadline) {
            return -2;
        }
        if (!SetPendingSocketPollTimeout(socket, deadline)) {
            return -2;
        }
        SWBaseSocket::SWBaseError error;
        const int result = socket->recv(header_data + header_received,
                static_cast<int>(sizeof(header) - header_received), &error);
        if (GetThreadState() != ThreadState::RUNNING) {
            return -2;
        }
        if (result > 0) {
            header_received += static_cast<unsigned int>(result);
        } else if (error != SWBaseSocket::timeout && error != SWBaseSocket::interrupted) {
            return -2;
        }
    }
    *type = header.command;
    *source = header.source;
    *stream_id = header.streamid;
    *payload_len = header.size;
    if (header.size > payload_capacity) {
        return -3;
    }
    if (header.size > 0) {
        memset(payload, 0, payload_capacity);
        unsigned int payload_received = 0;
        while (payload_received < header.size) {
            if (GetThreadState() != ThreadState::RUNNING ||
                    std::chrono::steady_clock::now() >= deadline) {
                return -1;
            }
            if (!SetPendingSocketPollTimeout(socket, deadline)) {
                return -1;
            }
            SWBaseSocket::SWBaseError error;
            const int result = socket->recv(payload + payload_received,
                    static_cast<int>(header.size - payload_received), &error);
            if (GetThreadState() != ThreadState::RUNNING) {
                return -1;
            }
            if (result > 0) {
                payload_received += static_cast<unsigned int>(result);
            } else if (error != SWBaseSocket::timeout && error != SWBaseSocket::interrupted) {
                return -1;
            }
        }
    }
    Messaging::StatsAddIncoming(sizeof(header) + header.size);
    return 0;
}

int Listener::SendHandshakeMessage(SWInetSocket *socket, int type, int source,
        unsigned int stream_id, unsigned int length, const char *payload)
{
    if (length > RORNET_MAX_MESSAGE_LENGTH) {
        return -4;
    }
    const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
    RoRnet::Header header = {};
    header.command = type;
    header.source = source;
    header.streamid = stream_id;
    header.size = length;
    std::vector<char> frame(sizeof(header) + length);
    std::memcpy(frame.data(), &header, sizeof(header));
    if (length > 0) {
        std::memcpy(frame.data() + sizeof(header), payload, length);
    }

    unsigned int sent = 0;
    while (sent < frame.size()) {
        if (GetThreadState() != ThreadState::RUNNING ||
                std::chrono::steady_clock::now() >= deadline) {
            return -1;
        }
        if (!SetPendingSocketPollTimeout(socket, deadline)) {
            return -1;
        }
        SWBaseSocket::SWBaseError error;
        const int result = socket->send(frame.data() + sent,
                static_cast<int>(frame.size() - sent), &error);
        if (GetThreadState() != ThreadState::RUNNING) {
            return -1;
        }
        if (result > 0) {
            sent += static_cast<unsigned int>(result);
        } else if (error != SWBaseSocket::timeout && error != SWBaseSocket::interrupted) {
            return -1;
        }
    }
    Messaging::StatsAddOutgoing(static_cast<int>(frame.size()));
    return 0;
}

Listener::Listener(Sequencer *sequencer) :
        m_sequencer(sequencer) {
}

bool Listener::Initialize() {
    // Make sure it's not started twice
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_thread_state != ThreadState::NOT_RUNNING)
    {
        return true;
    }

    // Start listening on the socket
    SWBaseSocket::SWBaseError error;
    m_listen_socket.bind(Config::getListenPort(), &error);
    if (error != SWBaseSocket::ok) {
        Logger::Log(LOG_ERROR, "FATAL Listerer: %s", error.get_error().c_str());
        return false;
    }
    m_listen_socket.listen();
    m_listen_socket.set_timeout(1, 0);

    // Start the thread
    m_thread = std::thread(&Listener::ThreadMain, this);
    m_thread_state = ThreadState::RUNNING;

    return true;
}

void Listener::Shutdown() {
    // Make sure it's not shut down twice
    m_sequencer->BeginShutdown();

    bool join_thread = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_thread_state == ThreadState::NOT_RUNNING) {
            return;
        }
        m_thread_state = ThreadState::STOP_REQUESTED;
        if (!m_join_claimed) {
            m_join_claimed = true;
            join_thread = true;
        } else {
            m_shutdown_complete.wait(lock, [this]() {
                return m_thread_state == ThreadState::NOT_RUNNING;
            });
            return;
        }
    }

    Logger::Log(LOG_VERBOSE, "Stopping listener thread...");
    WakeAccept();
    if (join_thread && m_thread.joinable()) {
        m_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_thread_state = ThreadState::NOT_RUNNING;
    }
    m_shutdown_complete.notify_all();
    Logger::Log(LOG_VERBOSE, "Listener thread stopped");
}

void Listener::WakeAccept()
{
    SWInetSocket wake_socket;
    SWInetSocket::SWBaseError error;
    wake_socket.set_timeout(1, 0);
    wake_socket.connect(Config::getListenPort(), "127.0.0.1", &error);
    // This local object is owned by the shutdown caller. The wake connection
    // needs no graceful TCP teardown; closing it immediately avoids adding a
    // wait while the listener thread is leaving accept()/handshake processing.
    wake_socket.close_fd();
}

void Listener::ThreadMain() {
    Logger::Log(LOG_DEBUG, "Listerer thread starting");

    SWBaseSocket::SWBaseError error;
    //await connections
    while (GetThreadState() == ThreadState::RUNNING) {
        Logger::Log(LOG_VERBOSE, "Listener awaiting connections");
        SWInetSocket *ts = static_cast<SWInetSocket *>(m_listen_socket.accept(&error));
        if (error != SWBaseSocket::ok) {
            if (GetThreadState() == ThreadState::STOP_REQUESTED) {
                Logger::Log(LOG_ERROR, "INFO Listener shutting down");
            } else {
                Logger::Log(LOG_ERROR, "ERROR Listener: %s", error.get_error().c_str());
            }
            continue;
        }
        if (ts == nullptr) {
            continue;
        }

        if (GetThreadState() != ThreadState::RUNNING || !m_sequencer->IsAcceptingClients()) {
            AbortPendingSocketForShutdown(ts);
            break;
        }

        Logger::Log(LOG_VERBOSE, "Listener got a new connection");

        //receive a magic
        int type;
        int source;
        unsigned int len;
        unsigned int streamid;
        char buffer[RORNET_MAX_MESSAGE_LENGTH];

        try {
            // this is the start of it all, it all starts with a simple hello
            if (ReceiveHandshakeMessage(ts, &type, &source, &streamid, &len,
                                        buffer, RORNET_MAX_MESSAGE_LENGTH))
                throw std::runtime_error("ERROR Listener: receiving first message");

            // make sure our first message is a hello message
            if (type != RoRnet::MSG2_HELLO) {
                SendHandshakeMessage(ts, RoRnet::MSG2_WRONG_VER, 0, 0, 0, nullptr);
                throw std::runtime_error("ERROR Listener: protocol error");
            }

            // check client version
            if (source == 5000 && (std::string(buffer) == "MasterServer")) {
                Logger::Log(LOG_VERBOSE, "Master Server knocked ...");
                // send back some information, then close socket
                char tmp[2048] = "";
                sprintf(tmp, "protocol:%s\nrev:%s\nbuild_on:%s_%s\n", RORNET_VERSION, VERSION, __DATE__, __TIME__);
                if (SendHandshakeMessage(ts, RoRnet::MSG2_MASTERINFO, 0, 0, (unsigned int) strlen(tmp), tmp)) {
                    throw std::runtime_error("ERROR Listener: sending master info");
                }
                // close socket
                ClosePendingSocketGracefully(ts);
                continue;
            }

            // compare the versions if they are compatible
            if (strncmp(buffer, RORNET_VERSION, strlen(RORNET_VERSION))) {
                // not compatible
                SendHandshakeMessage(ts, RoRnet::MSG2_WRONG_VER, 0, 0, 0, nullptr);
                throw std::runtime_error("ERROR Listener: bad version: " + std::string(buffer) + ". rejecting ...");
            }

            // compatible version, continue to send server settings
            std::string motd_str;
            {
                std::vector<std::string> lines;
                if (!Utils::ReadLinesFromFile(Config::getMOTDFile(), lines))
                {
                    for (const auto& line : lines)
                        motd_str += line + "\n";
                }
            }

            Logger::Log(LOG_DEBUG, "Listener sending server settings");
            RoRnet::ServerInfo settings;
            memset(&settings, 0, sizeof(RoRnet::ServerInfo));
            settings.has_password = !Config::getPublicPassword().empty();
            strncpy(settings.info, motd_str.c_str(), motd_str.size());
            strncpy(settings.protocolversion, RORNET_VERSION, strlen(RORNET_VERSION));
            strncpy(settings.servername, Config::getServerName().c_str(), Config::getServerName().size());
            strncpy(settings.terrain, Config::getTerrainName().c_str(), Config::getTerrainName().size());

            if (SendHandshakeMessage(ts, RoRnet::MSG2_HELLO, 0, 0, (unsigned int) sizeof(RoRnet::ServerInfo),
                                     reinterpret_cast<char *>(&settings)))
                throw std::runtime_error("ERROR Listener: sending version");

            //receive user infos
            if (ReceiveHandshakeMessage(ts, &type, &source, &streamid, &len,
                                        buffer, RORNET_MAX_MESSAGE_LENGTH)) {
                std::stringstream error_msg;
                error_msg << "ERROR Listener: receiving user infos\n"
                          << "ERROR Listener: got that: "
                          << type;
                throw std::runtime_error(error_msg.str());
            }

            if (type != RoRnet::MSG2_USER_INFO)
                throw std::runtime_error("Warning Listener: no user name");

            if (len > sizeof(RoRnet::UserInfo))
                throw std::runtime_error("Error: did not receive proper user credentials");
            Logger::Log(LOG_INFO, "Listener creating a new client...");

            RoRnet::UserInfo *user = (RoRnet::UserInfo *) buffer;
            user->authstatus = RoRnet::AUTH_NONE;

            if (!m_sequencer->IsAcceptingClients()) {
                throw std::runtime_error("ERROR Listener: server is shutting down");
            }

            // authenticate
            user->username[RORNET_MAX_USERNAME_LEN - 1] = 0;
            std::string nickname = Str::SanitizeUtf8(user->username);
            const unsigned int prospective_uid = m_sequencer->GetProspectiveAuthUid();
            user->authstatus = m_sequencer->AuthorizeNick(
                    std::string(user->usertoken, 40), nickname, prospective_uid);
            strncpy(user->username, nickname.c_str(), RORNET_MAX_USERNAME_LEN - 1);

            if (Config::isPublic()) {
                Logger::Log(LOG_DEBUG, "password login: %s == %s?",
                            Config::getPublicPassword().c_str(),
                            std::string(user->serverpassword, 40).c_str());
                if (strncmp(Config::getPublicPassword().c_str(), user->serverpassword, 40)) {
                    SendHandshakeMessage(ts, RoRnet::MSG2_WRONG_PW, 0, 0, 0, nullptr);
                    throw std::runtime_error("ERROR Listener: wrong password");
                }

                Logger::Log(LOG_DEBUG, "user used the correct password, "
                        "creating client!");
            } else {
                Logger::Log(LOG_DEBUG, "no password protection, creating client");
            }

            if (Config::getRankedOnly()) {
                Logger::Log(LOG_DEBUG, "ranked-only server: checking user status");
                if (user->authstatus == RoRnet::AUTH_NONE) {
                    Logger::Log(LOG_DEBUG, "ranked-only server: rejecting non-ranked user");
                    SendHandshakeMessage(ts, RoRnet::MSG2_NO_RANK, 0, 0, 0, nullptr);
                    throw std::runtime_error("ERROR Listener: no auth status");
                }
            }

            //create a new client
            if (!m_sequencer->createClient(ts, *user)) {
                throw std::runtime_error("ERROR Listener: admission closed during authentication");
            }
            ts = nullptr; // Client now owns the accepted socket.
            Logger::Log(LOG_DEBUG, "listener returned!");
        }
        catch (std::runtime_error &e) {
            Logger::Log(LOG_ERROR, e.what());
            if (GetThreadState() == ThreadState::RUNNING) {
                ClosePendingSocketGracefully(ts);
            } else {
                AbortPendingSocketForShutdown(ts);
            }
        } catch (const std::exception& e) {
            Logger::Log(LOG_ERROR, "Listener handshake failed: %s", e.what());
            if (GetThreadState() == ThreadState::RUNNING) {
                ClosePendingSocketGracefully(ts);
            } else {
                AbortPendingSocketForShutdown(ts);
            }
        } catch (...) {
            Logger::Log(LOG_ERROR, "Listener handshake failed with an unknown error");
            if (GetThreadState() == ThreadState::RUNNING) {
                ClosePendingSocketGracefully(ts);
            } else {
                AbortPendingSocketForShutdown(ts);
            }
        }
    }

    // The listener thread owns the listening descriptor. A listening socket has
    // no peer with which to perform SocketW's graceful disconnect handshake.
    // close_fd() closes it immediately and marks it invalid for the destructor.
    m_listen_socket.close_fd();

}

Listener::ThreadState Listener::GetThreadState()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_thread_state;
}
