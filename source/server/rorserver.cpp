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

// RoRserver.cpp : Defines the entry point for the console application.

#include "rornet.h"
#include "sequencer.h"
#include "logger.h"
#include "config.h"
#include "messaging.h"
#include "listener.h"
#include "master-server.h"
#include "utils.h"
#include "http.h"

#include "sha1_util.h"
#include "sha1.h"

#include <iostream>
#include <cstdlib>
#include <csignal>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <atomic>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
# include "windows.h"
# include "resource.h"
#else // _WIN32

# include <fcntl.h>
# include <signal.h>
# include <unistd.h>
# include <sys/types.h>
# include <pwd.h>
# include <sys/types.h>
# include <sys/stat.h>

#endif // _WIN32


static Sequencer s_sequencer;
static MasterServer::Client s_master_server;
static bool s_exit_requested = false;
#ifndef _WIN32
static volatile sig_atomic_t s_shutdown_requested = 0;
#else
static std::atomic<bool> s_windows_shutdown_requested(false);
#endif

#ifndef _WIN32

void handler(int) {
    s_shutdown_requested = 1;
}

#endif // ! _WIN32

static bool ShutdownRequested()
{
#ifndef _WIN32
    return s_shutdown_requested != 0 || s_exit_requested;
#else
    return s_windows_shutdown_requested.load(std::memory_order_acquire) || s_exit_requested;
#endif
}

static bool SleepUntilShutdown(unsigned int seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!ShutdownRequested() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return ShutdownRequested();
}

#ifdef _WIN32
// Reference: https://msdn.microsoft.com/en-us/library/ms686016.aspx
BOOL WINAPI WindowsConsoleHandlerRoutine(DWORD ctrl_type)
{
    switch (ctrl_type)
    {
    case CTRL_C_EVENT:
        break;
    case CTRL_BREAK_EVENT:
        break;
    case CTRL_CLOSE_EVENT:
        break;
    case CTRL_SHUTDOWN_EVENT:
        break;
    default:
        return TRUE; // Means 'event handled'
    }

    s_windows_shutdown_requested.store(true, std::memory_order_release);
    return TRUE;
}
#endif

#ifndef WITHOUTMAIN

#ifndef _WIN32
// from http://www.enderunix.org/docs/eng/daemon.php
// also http://www-theorie.physik.unizh.ch/~dpotter/howto/daemonize

#define LOCK_FILE "rorserver.lock"

void daemonize() {
    if (getppid() == 1) {
        /* already a daemon */
        return;
    }

    /* Drop user if there is one, and we were run as root */
    const char *username = "rorserver";
    // TODO: add flexibility to change the username via cmdline
    if (getuid() == 0 || geteuid() == 0) {
        Logger::Log(LOG_VERBOSE, "changing user to %s", username);
        struct passwd *pw = getpwnam(username);
        if (pw) {
            int i = setuid(pw->pw_uid);
            if (i) {
                perror("unable to change user");
                exit(1);
            }
        } else {
            //perror("error getting user");
            Logger::Log(LOG_ERROR, "unable to get user %s, Is it existing?", username);
            printf("unable to get user %s, Is it existing?\n", username);
            exit(1);
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        Logger::Log(LOG_ERROR, "error forking into background");
        perror("error forking into background");
        exit(1); /* fork error */
    }
    if (pid > 0) {
        // need both here
        printf("forked into background as pid %d\n", pid);
        Logger::Log(LOG_INFO, "forked into background as pid %d", pid);
        exit(0); /* parent exits */
    }

    /* child (daemon) continues */

    /* Change the file mode mask */
    umask(0);

    /* obtain a new process group */
    pid_t sid = setsid();
    if (sid < 0) {
        perror("unable to get a new session");
        exit(1);
    }

    /* Redirect standard files to /dev/null */
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);

    /* Change the current working directory.  This prevents the current
       directory from being locked; hence not being able to remove it. */
    if ((chdir("/")) < 0) {
        perror("unable to change working directory to /");
        exit(1);
    }

    /*
    // TODO: add config option for lockfile name
    {
        int lfp=open(LOCK_FILE,O_RDWR|O_CREAT,0640);
        if (lfp<0)
        {
            //cannot open
            perror("could not open lock file");
            exit(1);
        }
        if (lockf(lfp,F_TLOCK,0)<0)
        {
            // cannot lock
            perror("could not lock");
            exit(0); 
        }

        // record pid to lockfile
        char str[10];
        sprintf(str,"%d\n",getpid());
        write(lfp,str,strlen(str));
    }
    */

    // ignore some signals
    signal(SIGCHLD, SIG_IGN); /* ignore child */
    signal(SIGTSTP, SIG_IGN); /* ignore tty signals */
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
}

#endif // ! _WIN32

int main(int argc, char *argv[]) {
    int return_code = 0;
    bool listener_initialized = false;
    bool sequencer_initialized = false;

    // set default verbose levels
    Logger::SetLogLevel(LOGTYPE_DISPLAY, LOG_INFO);
    Logger::SetLogLevel(LOGTYPE_FILE, LOG_VERBOSE);
    Logger::SetOutputFile("server.log");

    if (!Config::ProcessArgs(argc, argv)) {
        return -1;
    }
    if (Config::GetShowHelp()) {
        Config::ShowHelp();
        return 0;
    }
    if (Config::GetShowVersion()) {
        Config::ShowVersion();
        return 0;
    }

    if (!Http::Initialize()) {
        return -1;
    }

    // Check configuration
    ServerType server_mode = Config::getServerMode();
    if (server_mode != SERVER_LAN) {
        Logger::Log(LOG_INFO, "Starting server in INET mode");
        std::string ip_addr = Config::getIPAddr();
        if (ip_addr.empty() || (ip_addr == "0.0.0.0")) {
            Logger::Log(LOG_WARN, "No IP given, detecting...");
            if (!MasterServer::RetrievePublicIp()) {
                Logger::Log(LOG_ERROR, "Failed to auto-detect public IP, exit.");
                return -1;
            }
        }
        Logger::Log(LOG_INFO, "IP address: %s", Config::getIPAddr().c_str());

        unsigned int max_clients = Config::getMaxClients();
        Logger::Log(LOG_INFO, "Maximum required upload: %ikbit/s", max_clients * (max_clients - 1) * 64);
        Logger::Log(LOG_INFO, "Maximum required download: %ikbit/s", max_clients * 64);

        if (Config::getServerName().empty()) {
            Logger::Log(LOG_ERROR, "Server name not specified, exit.");
            return -1;
        }
        Logger::Log(LOG_INFO, "Server name: %s", Config::getServerName().c_str());
    }

    if (!Config::checkConfig()) {
        return 1;
    }

    if (!sha1check()) {
        Logger::Log(LOG_ERROR, "sha1 malfunction!");
        return -1;
    }

#ifndef _WIN32
    if (!Config::getForeground()) {
        // no output because of background mode
        Logger::SetLogLevel(LOGTYPE_DISPLAY, LOG_NONE);
        daemonize();
    }
#endif // ! _WIN32

    // so ready to run, then set up signal handling
#ifndef _WIN32
    struct sigaction shutdown_action;
    memset(&shutdown_action, 0, sizeof(shutdown_action));
    shutdown_action.sa_handler = handler;
    sigemptyset(&shutdown_action.sa_mask);
    if (sigaction(SIGHUP, &shutdown_action, nullptr) != 0 ||
        sigaction(SIGINT, &shutdown_action, nullptr) != 0 ||
        sigaction(SIGTERM, &shutdown_action, nullptr) != 0) {
        Logger::Log(LOG_ERROR, "Failed to install shutdown signal handlers");
        return -1;
    }
#else // _WIN32
    SetConsoleCtrlHandler(WindowsConsoleHandlerRoutine, TRUE);
#endif // ! _WIN32


    Listener listener(&s_sequencer);
    if (!listener.Initialize()) {
        return_code = -1;
        goto cleanup;
    }
    listener_initialized = true;
    s_sequencer.Initialize();
    sequencer_initialized = true;

    // Listener is ready, let's register ourselves on serverlist (which will contact us back to check).
    if (server_mode != SERVER_LAN) {
        bool registered = s_master_server.Register();
        if (!registered && (server_mode == SERVER_INET)) {
            Logger::Log(LOG_ERROR, "Failed to register on serverlist. Exit");
            return_code = -1;
            goto cleanup;
        } else if (!registered) // server_mode == SERVER_AUTO
        {
            Logger::Log(LOG_WARN, "Failed to register on serverlist, continuing in LAN mode");
            server_mode = SERVER_LAN;
        } else {
            Logger::Log(LOG_INFO, "Registration successful");
        }
    }

    // start the main program loop
    // if we need to communiate to the master user the notifier routine
    if (server_mode != SERVER_LAN) {
        //heartbeat
        while (!ShutdownRequested()) {
            Messaging::UpdateMinuteStats();
            s_sequencer.UpdateMinuteStats();

            //every minute
            if (SleepUntilShutdown(Config::GetHeartbeatIntervalSec())) {
                break;
            }

            Logger::Log(LOG_VERBOSE, "Sending heartbeat...");
            Json::Value user_list(Json::arrayValue);
            s_sequencer.GetHeartbeatUserList(user_list);
            if (!s_master_server.SendHeatbeat(user_list)) {
                unsigned int timeout = Config::GetHeartbeatRetrySeconds();
                unsigned int max_retries = Config::GetHeartbeatRetryCount();
                Logger::Log(LOG_WARN, "A heartbeat failed! Retry in %d seconds.", timeout);
                bool success = false;
                for (unsigned int i = 0; i < max_retries; ++i) {
                    if (SleepUntilShutdown(timeout)) {
                        break;
                    }
                    success = s_master_server.SendHeatbeat(user_list);

                    LogLevel log_level = (success ? LOG_INFO : LOG_ERROR);
                    const char *log_result = (success ? "successful." : "failed.");
                    Logger::Log(log_level, "Heartbeat retry %d/%d %s", i + 1, max_retries, log_result);
                    if (success) {
                        break;
                    }
                }
                if (!success) {
                    if (ShutdownRequested()) {
                        break;
                    }
                    Logger::Log(LOG_ERROR, "Unable to send heartbeats, exit");
                    s_exit_requested = true;
                    break;
                }
            } else {
                Logger::Log(LOG_VERBOSE, "Heartbeat sent OK");
            }
        }

    } else {
        while (!ShutdownRequested()) {
            Messaging::UpdateMinuteStats();
            s_sequencer.UpdateMinuteStats();

            // broadcast our "i'm here" signal
            Messaging::broadcastLAN();

            // sleep a minute
            if (SleepUntilShutdown(60)) {
                break;
            }
        }

    }

cleanup:
    s_exit_requested = true;
    if (listener_initialized) {
        listener.Shutdown();
    }

    if (s_master_server.IsRegistered()) {
        s_master_server.UnRegister();
    }

    if (sequencer_initialized) {
        s_sequencer.Close();
    }

    return return_code;
}

#endif //WITHOUTMAIN

