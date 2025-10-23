/* sharkd_daemon.c
 *
 * Copyright (C) 2016 Jakub Zawadzki
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>
#define WS_LOG_DOMAIN LOG_DOMAIN_MAIN

#include <glib.h>

#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
#include <wsutil/unicode-utils.h>
#include <wsutil/win32-utils.h>
#endif

#include <wsutil/filesystem.h>
#include <wsutil/socket.h>
#include <wsutil/inet_addr.h>
#include <wsutil/please_report_bug.h>
#include <wsutil/wslog.h>
#include <wsutil/ws_getopt.h>

#ifndef _WIN32
#include <sys/un.h>
#include <netinet/tcp.h>
#endif

#include <wsutil/strtoi.h>
#include <wsutil/version_info.h>

#include "sharkd.h"

#ifdef _WIN32
/*
 * TCP sockets can work on Linux and other systems, but is disabled by default
 * because we do not want to encourage insecure setups. Unfiltered access to
 * sharkd could potentially result in arbitrary command execution.
 * On Windows, Unix sockets are not supported, so we enable it there.
 */
# define SHARKD_TCP_SUPPORT
#endif

#ifndef _WIN32
# define SHARKD_UNIX_SUPPORT
#endif

static int mode;
static socket_handle_t _server_fd = INVALID_SOCKET;

static socket_handle_t
socket_init(char *path)
{
    socket_handle_t fd = INVALID_SOCKET;
    char *err_msg;

    err_msg = ws_init_sockets();
    if (err_msg != NULL) {
        ws_warning("ERROR: %s", err_msg);
        g_free(err_msg);
        ws_warning("%s", please_report_bug());
        return fd;
    }

    if (!strncmp(path, "unix:", 5))
    {
#ifdef SHARKD_UNIX_SUPPORT
        struct sockaddr_un s_un;
        socklen_t s_un_len;

        path += 5;

        if (strlen(path) + 1 > sizeof(s_un.sun_path)) {
            fputs("Socket path too long.\n", stderr);
            return INVALID_SOCKET;
        }

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == INVALID_SOCKET) {
            fprintf(stderr, "Failed to create socket: %s\n", g_strerror(errno));
            return INVALID_SOCKET;
        }

        memset(&s_un, 0, sizeof(s_un));
        s_un.sun_family = AF_UNIX;
        (void) g_strlcpy(s_un.sun_path, path, sizeof(s_un.sun_path));

        s_un_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(s_un.sun_path));

        if (s_un.sun_path[0] == '@')
            s_un.sun_path[0] = '\0';

        if (bind(fd, (struct sockaddr *) &s_un, s_un_len))
        {
            fprintf(stderr, "Failed to bind socket: %s\n", g_strerror(errno));
            closesocket(fd);
            return INVALID_SOCKET;
        }
#else
        fputs("Unix sockets are not available on Windows, use tcp instead.\n", stderr);
        return INVALID_SOCKET;
#endif
    } else if (!strncmp(path, "tcp:", 4)) {
#ifdef SHARKD_TCP_SUPPORT
        struct sockaddr_in s_in;
        int one = 1;
        char *port_sep;
        uint16_t port;

        path += 4;

        port_sep = strchr(path, ':');
        if (!port_sep) {
            fputs("Missing port number in socket path.\n", stderr);
            return INVALID_SOCKET;
        }

        *port_sep = '\0';

        if (ws_strtou16(port_sep + 1, NULL, &port) == false) {
            fputs("Invalid port number.\n", stderr);
            return INVALID_SOCKET;
        }

#ifdef _WIN32
        /* Need to use WSASocket() to disable overlapped I/O operations,
            this way on windows SOCKET can be used as HANDLE for stdin/stdout */
        fd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
#else
        fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
        if (fd == INVALID_SOCKET) {
            fprintf(stderr, "Failed to open socket: %s\n",
#ifdef _WIN32
                    win32strerror(WSAGetLastError())
#else
                    g_strerror(errno)
#endif
                   );
            return INVALID_SOCKET;
        }

        s_in.sin_family = AF_INET;
        ws_inet_pton4(path, (ws_in4_addr *)&(s_in.sin_addr.s_addr));
        s_in.sin_port = g_htons(port);
        *port_sep = ':';

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *) &one, sizeof(one));

        if (bind(fd, (struct sockaddr *) &s_in, sizeof(struct sockaddr_in)))
        {
            fprintf(stderr, "Failed to bind socket: %s\n", g_strerror(errno));
            closesocket(fd);
            return INVALID_SOCKET;
        }
#else
        fputs("TCP sockets are not available for security reasons, use Unix sockets instead.\n", stderr);
        return INVALID_SOCKET;
#endif
    }
    else
    {
        fprintf(stderr, "Unsupported socket path '%s', try unix:... for Unix sockets\n", path);
        return INVALID_SOCKET;
    }

    if (listen(fd, SOMAXCONN))
    {
        fprintf(stderr, "Failed to listen on socket: %s\n", g_strerror(errno));
        closesocket(fd);
        return INVALID_SOCKET;
    }

    return fd;
}

static void
print_usage(FILE* output)
{
    fprintf(output, "\n");
    fprintf(output, "Usage: sharkd [options]\n");
    fprintf(output, "  or   sharkd -\n");

    fprintf(output, "\n");
    fprintf(output, "Options:\n");
    fprintf(output, "  -a <socket>, --api <socket>\n");
    fprintf(output, "                           listen on this socket instead of the console\n");
    fprintf(output, "  --foreground             do not detach from console\n");
    fprintf(output, "  -h, --help               show this help information\n");
    fprintf(output, "  -v, --version            show version information\n");
    fprintf(output, "  -C <config profile>, --config-profile <config profile>\n");
    fprintf(output, "                           start with specified configuration profile\n");

    fprintf(output, "\n");
    fprintf(output, "Supported socket types:\n");
#ifdef SHARKD_UNIX_SUPPORT
    fprintf(output, "    unix:/tmp/sharkd.sock - listen on Unix domain socket file /tmp/sharkd.sock\n");
    fprintf(output, "    unix:@sharkd          - listen on abstract Unix socket 'sharkd' (Linux-only)\n");
#else
    fprintf(output, "    (Unix domain sockets are unavailable on this platform.)\n");
#endif
#ifdef SHARKD_TCP_SUPPORT
    fprintf(output, "    tcp:127.0.0.1:4446    - listen on TCP port 4446\n");
#else
    fprintf(output, "    (TCP sockets are disabled in this build)\n");
#endif

    fprintf(output, "\n");
    fprintf(output, "If no socket option is provided, or if 'sharkd -' is used,\n");
    fprintf(output, "sharkd will accept commands via console (standard input).\n");

    fprintf(output, "\n");
    fprintf(output, "Examples:\n");
    fprintf(output, "    sharkd -\n");
    fprintf(output, "    sharkd -C myprofile\n");
#ifdef SHARKD_UNIX_SUPPORT
    fprintf(output, "    sharkd -a unix:/tmp/sharkd.sock -C myprofile\n");
#elif defined(SHARKD_TCP_SUPPORT)
    fprintf(output, "    sharkd -a tcp:127.0.0.1:4446 -C myprofile\n");
#endif

    fprintf(output, "\n");
    fprintf(output, "For security reasons, do not directly expose sharkd to the public Internet.\n");
    fprintf(output, "Instead, have a separate backend service to interact with sharkd.\n");
    fprintf(output, "\n");
    fprintf(output, "For full details, see https://wiki.wireshark.org/Development/sharkd\n");
    fprintf(output, "\n");
}

int
sharkd_init(int argc, char **argv)
{
    int opt;

#ifndef _WIN32
    pid_t pid;
#endif
    socket_handle_t fd;
    bool foreground = false;

    if (argc < 2)
    {
        print_usage(stderr);
        return -1;
    }

    // check for classic command line
    if (!strcmp(argv[1], "-") || argv[1][0] == 't' || argv[1][0] == 'u')
    {
        mode = SHARKD_MODE_CLASSIC_CONSOLE;

#ifndef _WIN32
        signal(SIGCHLD, SIG_IGN);
#endif

        if (!strcmp(argv[1], "-"))
        {
            mode = SHARKD_MODE_CLASSIC_CONSOLE;
        }
        else
        {
            fd = socket_init(argv[1]);
            if (fd == INVALID_SOCKET)
                return -1;
            _server_fd = fd;
            mode = SHARKD_MODE_CLASSIC_DAEMON;
        }
    }
    else
        mode = SHARKD_MODE_GOLD_CONSOLE;  // assume we are running as gold console

    if (mode >= SHARKD_MODE_GOLD_CONSOLE)
    {
        /*
           In Daemon Mode, we will come through here twice; once when we start the Daemon and
           once again after we have forked the session process.  The second time through, the
           session process has already had its stdin and stdout wired up to the TCP or UNIX
           socket and so in the original version of sharkd the session process is invoked with
           the command line: sharkd -

           When not using the classic command line, we want to spawn the session process with
           the complete command line with all the new options but with the -a option and
           parameter removed.  Invoking a second time with the -a option will cause a loop
           where we repeatedly spawn a new session process.
           */

        /*
         * To reset the options parser, set ws_optreset to 1 and set ws_optind to 1.
         *
         * Also reset ws_opterr to 1, so that error messages are printed by
         * getopt_long().
         */
        ws_optreset = 1;
        ws_optind = 1;
        ws_opterr = 1;

        do {
            if (ws_optind > (argc - 1))
                break;

            opt = ws_getopt_long(argc, argv, sharkd_optstring(), sharkd_long_options(), NULL);

            switch (opt) {
                case 'C':        /* Configuration Profile */
                    if (profile_exists(ws_optarg, false)) {
                        set_profile_name(ws_optarg);  // In Daemon Mode, we may need to do this again in the child process
                    }
                    else {
                        fprintf(stderr, "Configuration Profile \"%s\" does not exist\n", ws_optarg);
                        return -1;
                    }
                    break;

                case 'a':
                    fd = socket_init(ws_optarg);
                    if (fd == INVALID_SOCKET)
                        return -1;
                    _server_fd = fd;

                    fprintf(stderr, "Sharkd listening on: %s\n", ws_optarg);

                    mode = SHARKD_MODE_GOLD_DAEMON;
                    break;

                case 'h':
                    show_help_header("Daemon variant of Wireshark");
                    print_usage(stdout);
                    exit(0);
                    break;

                case 'm':
                    // m is an internal-only option used when the daemon session process is created
                    mode = SHARKD_MODE_GOLD_CONSOLE;
                    break;

                case 'v':         /* Show version and exit */
                    show_version();
                    exit(0);
                    break;

                case LONGOPT_FOREGROUND:
                    foreground = true;
                    break;

                default:
                    /* wslog arguments are okay */
                    if (ws_log_is_wslog_arg(opt))
                        break;

                    if (!ws_optopt)
                        fprintf(stderr, "This option isn't supported: %s\n", argv[ws_optind]);
                    fprintf(stderr, "Use sharkd -h for details of supported options\n");
                    exit(0);
                    break;
            }
        } while (opt != -1);
    }

    if (!foreground && (mode == SHARKD_MODE_CLASSIC_DAEMON || mode == SHARKD_MODE_GOLD_DAEMON))
    {
        /* all good - try to daemonize */
#ifndef _WIN32
        pid = fork();
        if (pid == -1)
            fprintf(stderr, "cannot go to background fork() failed: %s\n", g_strerror(errno));

        if (pid != 0)
        {
            /* parent */
            exit(0);
        }
#endif
    }

    return 0;
}

int
#ifndef _WIN32
sharkd_loop(int argc _U_, char* argv[] _U_)
#else
sharkd_loop(int argc _U_, char* argv[])
#endif
{
    if (mode == SHARKD_MODE_CLASSIC_CONSOLE || mode == SHARKD_MODE_GOLD_CONSOLE)
    {
        return sharkd_session_main(mode);
    }

    while (1)
    {
#ifndef _WIN32
        pid_t pid;
#else
        size_t i_handles;
        HANDLE handles[2];
        PROCESS_INFORMATION pi;
        STARTUPINFO si;
        char *exename;
        char command_line[2048];
#endif
        socket_handle_t fd;

        fd = accept(_server_fd, NULL, NULL);
        if (fd == INVALID_SOCKET)
        {
            fprintf(stderr, "cannot accept(): %s\n", g_strerror(errno));
            continue;
        }

        /* wireshark is not ready for handling multiple capture files in single process, so fork(), and handle it in separate process */
#ifndef _WIN32
        pid = fork();
        if (pid == 0)
        {
            closesocket(_server_fd);
            /* redirect stdin, stdout to socket */
            dup2(fd, 0);
            dup2(fd, 1);
            close(fd);

            exit(sharkd_session_main(mode));
        }

        if (pid == -1)
        {
            fprintf(stderr, "cannot fork(): %s\n", g_strerror(errno));
        }

#else
        memset(&pi, 0, sizeof(pi));
        memset(&si, 0, sizeof(si));

        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdInput = (HANDLE) fd;
        si.hStdOutput = (HANDLE) fd;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        i_handles = 0;
        handles[i_handles++] = (HANDLE)fd;
        if (si.hStdError != NULL) {
            handles[i_handles++] = si.hStdError;
        }

        exename = get_executable_path("sharkd");

        // we need to pass in all of the command line parameters except the -a parameter
        // passing in -a at this point would could a loop, each iteration of which would generate a new session process
        memset(&command_line, 0, sizeof(command_line));

        if (mode <= SHARKD_MODE_CLASSIC_DAEMON)
        {
            (void) g_strlcat(command_line, "sharkd.exe -", sizeof(command_line));
        }
        else
        {
            // The -m option used here is an internal-only option that notifies the child process that it should
            // run in Gold Console mode
            (void) g_strlcat(command_line, "sharkd.exe -m", sizeof(command_line));

            for (int i = 1; i < argc; i++)
            {
                if (
                        !g_ascii_strncasecmp(argv[i], "-a", strlen(argv[i]))
                        || !g_ascii_strncasecmp(argv[i], "--api", strlen(argv[i]))
                   )
                {
                    i++;  // skip the socket details
                }
                else
                {
                    (void) g_strlcat(command_line, " ", sizeof(command_line));
                    (void) g_strlcat(command_line, argv[i], sizeof(command_line));
                }
            }
        }

        if (!win32_create_process(exename, command_line, NULL, NULL, i_handles, handles, 0, NULL, NULL, &si, &pi))
        {
            fprintf(stderr, "win32_create_process(%s) failed\n", exename);
        }
        else
        {
            CloseHandle(pi.hThread);
        }

        g_free(exename);
#endif

        closesocket(fd);
    }
    return 0;
}
