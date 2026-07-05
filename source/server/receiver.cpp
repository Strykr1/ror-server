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

#include "receiver.h"

#include "SocketW.h"
#include "sequencer.h"
#include "messaging.h"
#include "ScriptEngine.h"
#include "logger.h"

#include <cstring>
#include <cassert>


Receiver::Receiver(Sequencer *sequencer):
    m_sequencer(sequencer)
{
}

Receiver::~Receiver() {
    assert(m_thread_state == ThreadState::NOT_RUNNING);
}

void Receiver::Start(Client* client) {
    std::lock_guard<std::mutex> lock(m_mutex); // Scoped

    assert(m_thread_state == ThreadState::NOT_RUNNING);
    m_client = client;
    m_thread_state = ThreadState::RUNNING;
    try {
        m_thread = std::thread(&Receiver::ThreadMain, this);
    } catch (...) {
        m_thread_state = ThreadState::NOT_RUNNING;
        throw;
    }
}

void Receiver::RequestStop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex); // Scoped
        if (m_thread_state == ThreadState::RUNNING) {
            m_thread_state = ThreadState::STOP_REQUESTED;
        }
    }
}

void Receiver::Join() {
    if (!m_thread.joinable()) {
        return;
    }
    m_thread.join();

    {
        std::lock_guard<std::mutex> lock(m_mutex); // Scoped
        m_thread_state = ThreadState::NOT_RUNNING;
    }

}

Receiver::ThreadState Receiver::GetThreadState()
{
    std::lock_guard<std::mutex> lock(m_mutex); // Scoped
    return m_thread_state;
}

void Receiver::ThreadMain() {
    Logger::Log(LOG_DEBUG, "Started receiver thread (user ID %d)", m_client->GetUserId());

    m_client->SetReceiveData(true);
    Logger::Log(LOG_VERBOSE, "UID %d is switching to FLOW", m_client->GetUserId());

    while (this->GetThreadState() == ThreadState::RUNNING) {
        if (!this->ThreadReceiveMessage()) {
            m_sequencer->disconnectClient(m_client->GetUserId(), "Game connection closed");
            break;
        }

        if (m_recv_header.command != RoRnet::MSG2_STREAM_DATA &&
            m_recv_header.command != RoRnet::MSG2_STREAM_DATA_DISCARDABLE) {
            Logger::Log(LOG_VERBOSE, "got message: type: %d, source: %d:%d, len: %d",
                        (int)m_recv_header.command, (int)m_recv_header.source, (int)m_recv_header.streamid, (int)m_recv_header.size);
        }

        if (m_recv_header.command < 1000u || m_recv_header.command > 1050u) {
            m_sequencer->disconnectClient(m_client->GetUserId(), "Protocol error 3");
            break;
        }

        m_sequencer->queueMessage(m_client->GetUserId(),
            (int)m_recv_header.command, m_recv_header.streamid, m_recv_payload, m_recv_header.size);
    }

    Logger::Log(LOG_DEBUG, "Receiver thread (user ID %d) exits", m_client->GetUserId());
}

bool Receiver::ThreadReceiveMessage()
{
    if (!this->ThreadReceiveHeader())
    {
        return false; // Stop thread.
    }

    if (m_recv_header.size > 0)
    {
        if (!this->ThreadReceivePayload())
        {
            return false; // Stop thread.
        }
    }

    Messaging::StatsAddIncoming((int)sizeof(RoRnet::Header) + (int)m_recv_header.size);
    return true; // Continue receiving.
}

bool Receiver::ThreadReceiveHeader() //!< @return false if thread should be stopped, true to continue.
{
    std::memset((void*)&m_recv_header, 0, sizeof(RoRnet::Header));
    if (!ThreadReceiveExact(reinterpret_cast<char*>(&m_recv_header), sizeof(RoRnet::Header))) {
        return false; // stop thread.
    }

    if (m_recv_header.size > RORNET_MAX_MESSAGE_LENGTH)
    {
        // Oversized payload
        Logger::Log(LOG_WARN, "Receiver: payload too long: %d/ max. %d bytes", (int)m_recv_header.size, RORNET_MAX_MESSAGE_LENGTH);
        return false; // Stop thread.
    }

    return true; // continue receiving.
}

bool Receiver::ThreadReceivePayload() //!< @return false if thread should be stopped, true to continue.
{
    std::memset(m_recv_payload, 0, RORNET_MAX_MESSAGE_LENGTH);
    if (!ThreadReceiveExact(m_recv_payload, m_recv_header.size)) {
        return false; // stop thread.
    }

    return true; // continue receiving.
}

bool Receiver::ThreadReceiveExact(char* buffer, unsigned int length)
{
    unsigned int received = 0;
    while (received < length) {
        if (GetThreadState() != ThreadState::RUNNING ||
                m_client->GetShutdownMode() == Client::ShutdownMode::SERVER_SHUTDOWN) {
            return false;
        }

        SWBaseSocket::SWBaseError error;
        const int result = m_client->GetSocket()->recv(
                buffer + received, static_cast<int>(length - received), &error);
        if (result > 0) {
            received += static_cast<unsigned int>(result);
            continue;
        }
        if (error == SWBaseSocket::timeout || error == SWBaseSocket::interrupted) {
            continue;
        }
        Logger::Log(LOG_WARN, "Receiver: receive error: %s", error.get_error().c_str());
        return false;
    }
    return true;
}
