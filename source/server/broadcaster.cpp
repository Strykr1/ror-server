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

#include "broadcaster.h"

#include "logger.h"
#include "messaging.h"
#include "SocketW.h"
#include "sequencer.h"

#include <cassert>
#include <cstring>
#include <map>
#include <algorithm>
#include <chrono>

Broadcaster::Broadcaster(Sequencer *sequencer) :
    m_sequencer(sequencer) {
}


Broadcaster::~Broadcaster() {
}

bool Broadcaster::SendWelcome(Client* client, const RoRnet::UserInfo& user) {
    assert(m_thread_state == ThreadState::NOT_RUNNING);
    m_client = client;

    QueueEntry welcome;
    welcome.type = RoRnet::MSG2_WELCOME;
    welcome.uid = client->GetUserId();
    welcome.streamid = 0;
    welcome.datalen = sizeof(user);
    std::memcpy(welcome.data, &user, sizeof(user));
    return ThreadTransmitMessage(welcome, SendMode::FINITE_DEADLINE) ==
            TransmitResult::COMPLETED;
}


void Broadcaster::Start(Client* client) {
    std::lock_guard<std::mutex> scoped_lock(m_mutex);

    assert(m_client == client);
    m_is_dropping_packets = false;
    m_packet_drop_counter = 0;
    m_packet_good_counter = 0;
    m_msg_queue.clear();

    m_thread_state = ThreadState::RUNNING;
    try {
        m_thread = std::thread(&Broadcaster::ThreadMain, this);
    } catch (...) {
        m_thread_state = ThreadState::NOT_RUNNING;
        throw;
    }
}


void Broadcaster::RequestStop() {
    const int uid = (m_client != nullptr) ? m_client->GetUserId() : -1;
    {
        std::lock_guard<std::mutex> scoped_lock(m_mutex);
        switch (m_thread_state) {
        case ThreadState::RUNNING:
            Logger::Log(LOG_DEBUG, "Broadcaster::Stop() (client_id %d) Thread state is RUNNING -> stopping", uid);
            m_thread_state = ThreadState::STOP_REQUESTED;
            break;
        case ThreadState::NOT_RUNNING:
            Logger::Log(LOG_DEBUG, "Broadcaster::Stop() (client_id %d) Thread state is NOT_RUNNING -> nothing to do", uid);
            break;
        case ThreadState::STOP_REQUESTED:
            Logger::Log(LOG_DEBUG, "Broadcaster::Stop() (client_id %d) Thread state is STOP_REQUESTED -> nothing to do", uid);
            break;
        }
    }

    m_queue_cond.notify_one(); // Unblock the thread.
}

void Broadcaster::NotifyServerShutdown() {
    m_queue_cond.notify_one();
}

void Broadcaster::Join() {
    if (!m_thread.joinable()) {
        return;
    }
    m_thread.join(); // Wait for thread to exit.

    {
        std::lock_guard<std::mutex> scoped_lock(m_mutex);
        m_thread_state = ThreadState::NOT_RUNNING;
    }

}


void Broadcaster::ThreadMain() {
    Logger::Log(LOG_DEBUG, "Started broadcaster thread (client_id %d)", m_client->GetUserId());

    bool exit_loop = false;
    while (!exit_loop) {
        QueueEntry message;
        ThreadState state = this->ThreadWaitForMessage(message);

        if (m_client->GetShutdownMode() == Client::ShutdownMode::SERVER_SHUTDOWN) {
            {
                std::lock_guard<std::mutex> scoped_lock(m_mutex);
                m_msg_queue.clear();
            }
            ThreadTransmitServerLeave();
            exit_loop = true;
        } else if (state == ThreadState::STOP_REQUESTED) {
            Logger::Log(LOG_DEBUG, "Broadcaster thread (client_id %d) was requested to stop", m_client->GetUserId());
            // Drain complete frames. A server-shutdown transition may discard
            // only frames which have not started.
            bool active_frame_incomplete = false;
            while (m_client->GetShutdownMode() == Client::ShutdownMode::NORMAL) {
                QueueEntry queued_message;
                {
                    std::lock_guard<std::mutex> scoped_lock(m_mutex);
                    if (m_msg_queue.empty()) {
                        break;
                    }
                    queued_message = m_msg_queue.front();
                    m_msg_queue.pop_front();
                }
                const TransmitResult result = this->ThreadTransmitMessage(
                        queued_message, SendMode::NORMAL);
                if (result != TransmitResult::COMPLETED) {
                    active_frame_incomplete = true;
                    break;
                }
            }
            if (!active_frame_incomplete &&
                    m_client->GetShutdownMode() == Client::ShutdownMode::SERVER_SHUTDOWN) {
                {
                    std::lock_guard<std::mutex> scoped_lock(m_mutex);
                    m_msg_queue.clear();
                }
                ThreadTransmitServerLeave();
            }
            exit_loop = true;
        } else {
            const TransmitResult result = this->ThreadTransmitMessage(
                    message, SendMode::NORMAL);
            if (result != TransmitResult::COMPLETED) {
                if (result == TransmitResult::FAILED &&
                        m_client->GetShutdownMode() == Client::ShutdownMode::NORMAL) {
                    m_sequencer->disconnectClient(m_client->GetUserId(), "Broadcaster: Send error", true, true);
                }
                exit_loop = true;
            }
        }
    }

    Logger::Log(LOG_DEBUG, "Broadcaster thread (client_id %d) exits", m_client->GetUserId());
}


Broadcaster::ThreadState Broadcaster::ThreadWaitForMessage(QueueEntry& out_message) {
    std::unique_lock<std::mutex> uni_lock(m_mutex); // Scoped
    m_queue_cond.wait(uni_lock, [this]() {
        return m_thread_state == ThreadState::STOP_REQUESTED ||
                m_client->GetShutdownMode() == Client::ShutdownMode::SERVER_SHUTDOWN ||
                !m_msg_queue.empty();
    });
    if (!m_msg_queue.empty()) {
        out_message = m_msg_queue.front();
        m_msg_queue.pop_front();
    }
    return m_thread_state;
}


Broadcaster::TransmitResult Broadcaster::ThreadTransmitMessage(
        QueueEntry const& msg, SendMode mode) {
    int type = msg.type;
    if (type == RoRnet::MSG2_INVALID)
        return TransmitResult::COMPLETED;
    if (type == RoRnet::MSG2_STREAM_DATA_DISCARDABLE)
        type = RoRnet::MSG2_STREAM_DATA;

    RoRnet::Header header = {};
    header.command = type;
    header.source = msg.uid;
    header.size = msg.datalen;
    header.streamid = msg.streamid;

    if (msg.datalen > RORNET_MAX_MESSAGE_LENGTH) {
        Logger::Log(LOG_ERROR, "UID: %d - attempt to send too long payload", msg.uid);
        return TransmitResult::FAILED;
    }

    char frame[sizeof(RoRnet::Header) + RORNET_MAX_MESSAGE_LENGTH];
    const unsigned int frame_length = sizeof(header) + msg.datalen;
    std::memcpy(frame, &header, sizeof(header));
    if (msg.datalen > 0) {
        std::memcpy(frame + sizeof(header), msg.data, msg.datalen);
    }

    bool deadline_active = (mode == SendMode::FINITE_DEADLINE);
    auto deadline = std::chrono::steady_clock::time_point::max();
    if (deadline_active) {
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    unsigned int sent = 0;
    while (sent < frame_length) {
        if (!deadline_active &&
                m_client->GetShutdownMode() == Client::ShutdownMode::SERVER_SHUTDOWN) {
            deadline_active = true;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        if (deadline_active && std::chrono::steady_clock::now() >= deadline) {
            return (mode == SendMode::NORMAL)
                    ? TransmitResult::SHUTDOWN_DEADLINE_EXPIRED
                    : TransmitResult::FAILED;
        }
        SWBaseSocket::SWBaseError error;
        const int result = m_client->GetSocket()->send(
                frame + sent, static_cast<int>(frame_length - sent), &error);
        if (result > 0) {
            sent += static_cast<unsigned int>(result);
            continue;
        }
        if (error == SWBaseSocket::timeout || error == SWBaseSocket::interrupted) {
            continue;
        }
        Logger::Log(LOG_ERROR, "Broadcaster send error: %s", error.get_error().c_str());
        return TransmitResult::FAILED;
    }
    Messaging::StatsAddOutgoing(frame_length);
    return TransmitResult::COMPLETED;
}

bool Broadcaster::ThreadTransmitServerLeave() {
    static const char message[] = "server shutting down (try to reconnect later!)";
    QueueEntry leave;
    leave.type = RoRnet::MSG2_USER_LEAVE;
    leave.uid = m_client->GetUserId();
    leave.streamid = 0;
    leave.datalen = sizeof(message) - 1;
    std::memcpy(leave.data, message, leave.datalen);
    return ThreadTransmitMessage(leave, SendMode::FINITE_DEADLINE) ==
            TransmitResult::COMPLETED;
}


void Broadcaster::QueueMessage(int type, int uid, unsigned int streamid, unsigned int len, const char *data) {
    if (len > RORNET_MAX_MESSAGE_LENGTH) {
        Logger::Log(LOG_ERROR, "UID: %d - attempt to queue too long payload", uid);
        return;
    }
    QueueEntry msg;
    msg.type = (RoRnet::MessageType)type;
    msg.uid = uid;
    msg.streamid = streamid;
    msg.datalen = len;
    std::memcpy(msg.data, data, len);

    {
        std::lock_guard<std::mutex> scoped_lock(m_mutex);
        if (m_msg_queue.empty()) {
            m_packet_drop_counter = 0;
            m_is_dropping_packets = (++m_packet_good_counter > 3) ? false : m_is_dropping_packets;
        } else if (type == RoRnet::MSG2_STREAM_DATA_DISCARDABLE) {
            auto search = std::find_if(m_msg_queue.begin(), m_msg_queue.end(), [&](const QueueEntry& m)
                    { return m.type == RoRnet::MSG2_STREAM_DATA_DISCARDABLE && m.uid == uid && m.streamid == streamid; });
            if (search != m_msg_queue.end()) {
                // Found outdated discardable streamdata -> replace it
                (*search) = msg;
                m_packet_good_counter = 0;
                m_is_dropping_packets = (++m_packet_drop_counter > 3) ? true : m_is_dropping_packets;
                Messaging::StatsAddOutgoingDrop(sizeof(RoRnet::Header) + msg.datalen); // Statistics
                return;
            }
        }
        m_msg_queue.push_back(msg);
    }

    m_queue_cond.notify_one();
}

