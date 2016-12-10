/*
 * Tiny TCP proxy server
 *
 * Author: Krzysztof Kliś <krzysztof.klis@gmail.com>
 * Fixes and improvements: Jérôme Poulin <jeromepoulin@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version with the following modification:
 *
 * As a special exception, the copyright holders of this library give you
 * permission to link this library with independent modules to produce an
 * executable, regardless of the license terms of these independent modules,
 * and to copy and distribute the resulting executable under terms of your choice,
 * provided that you also meet, for each linked independent module, the terms
 * and conditions of the license of that module. An independent module is a
 * module which is not derived from or based on this library. If you modify this
 * library, you may extend this exception to your version of the library, but
 * you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <resolv.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/wait.h>
#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#define BUF_SIZE 16384

#define READ  0
#define WRITE 1

#define SERVER_SOCKET_ERROR -1
#define SERVER_SETSOCKOPT_ERROR -2
#define SERVER_BIND_ERROR -3
#define SERVER_LISTEN_ERROR -4
#define CLIENT_SOCKET_ERROR -5
#define CLIENT_RESOLVE_ERROR -6
#define CLIENT_CONNECT_ERROR -7
#define CREATE_PIPE_ERROR -8
#define BROKEN_PIPE_ERROR -9
#define SYNTAX_ERROR -10

typedef enum {TRUE = 1, FALSE = 0} bool;

int create_socket(int port);
void sigchld_handler(int signal);
void sigterm_handler(int signal);
void server_loop();
void handle_client(int client_sock, struct sockaddr_in client_addr);
void forward_data(int source_sock, int destination_sock);
void forward_data_ext(int source_sock, int destination_sock, char *cmd);
int create_connection(char *conn_host, int conn_port);
int set_reuseaddr(int sock);
int parse_options(int argc, char *argv[]);
void plog(int priority, const char *format, ...);

int server_sock, client_sock, remote_sock, remote_port = 0;
int connections_processed = 0;
char *cmd_in, *cmd_out;
char remote_host[256];
char **argvp;
bool foreground = FALSE;
bool use_syslog = FALSE;

/* Program start */
int main(int argc, char *argv[]) {
    int local_port;
    pid_t pid;

    local_port = parse_options(argc, argv);
    argvp = argv;

    if (local_port < 0) {
        printf("Syntax: %s -l local_port -h remote_host -p remote_port [-i \"input parser\"] [-o \"output parser\"] [-f (stay in foreground)] [-s (use syslog)]\n", argv[0]);
        return local_port;
    }

    if (use_syslog)
        openlog("proxy", LOG_PID, LOG_DAEMON);

    if ((server_sock = create_socket(local_port)) < 0) { // start server
        plog(LOG_CRIT, "Cannot run server: %m");
        return server_sock;
    }

    signal(SIGCHLD, sigchld_handler); // prevent ended children from becoming zombies
    signal(SIGTERM, sigterm_handler); // handle KILL signal

    if (foreground) {
        server_loop();
    } else {
        switch(pid = fork()) {
            case 0: // deamonized child
                server_loop();
                break;
            case -1: // error
                plog(LOG_CRIT, "Cannot daemonize: %m");
                return pid;
            default: // parent
                close(server_sock);
        }
    }

    if (use_syslog)
        closelog();

    return EXIT_SUCCESS;
}

/* Parse command line options */
int parse_options(int argc, char *argv[]) {
    int c, local_port = 0;

    while ((c = getopt(argc, argv, "l:h:p:i:o:fs")) != -1) {
        switch(c) {
            case 'l':
                local_port = atoi(optarg);
                break;
            case 'h':
                strncpy(remote_host, optarg, sizeof(remote_host));
                break;
            case 'p':
                remote_port = atoi(optarg);
                break;
            case 'i':
                cmd_in = optarg;
                break;
            case 'o':
                cmd_out = optarg;
                break;
            case 'f':
                foreground = TRUE;
                break;
            case 's':
                use_syslog = TRUE;
                break;
        }
    }

    if (local_port && remote_port) {
        return local_port;
    } else {
        return SYNTAX_ERROR;
    }
}

/* Create server socket */
int create_socket(int port) {
    int server_sock;
    struct sockaddr_in server_addr;

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        return SERVER_SOCKET_ERROR;
    }

    if (set_reuseaddr(server_sock) < 0) {
        return SERVER_SETSOCKOPT_ERROR;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        return SERVER_BIND_ERROR;
    }

    if (listen(server_sock, 20) < 0) {
        return SERVER_LISTEN_ERROR;
    }

    return server_sock;
}

/* Send log message to stderr or syslog */
void plog(int priority, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);

    if (use_syslog)
        vsyslog(priority, format, ap);
    else {
        vfprintf(stderr, format, ap);
        fprintf(stderr, "\n");
    }

    va_end(ap);
}

/* Update systemd status with connection count */
void update_connection_count()
{
#ifdef USE_SYSTEMD
    sd_notifyf(0, "STATUS=Ready. %d connections processed.\n", connections_processed);
#endif
}

/* Handle finished child process */
void sigchld_handler(int signal) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* Handle term signal */
void sigterm_handler(int signal) {
    close(client_sock);
    close(server_sock);
    exit(0);
}

int set_reuseaddr(int sock)
{
    int optval = 1;

    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

/* Main server loop */
void server_loop() {
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    char new_addr[INET_ADDRSTRLEN];

#ifdef USE_SYSTEMD
    sd_notify(0, "READY=1\n");
#endif

    while (TRUE) {
        update_connection_count();
        client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addrlen);
        set_reuseaddr(client_sock);
        if (fork() == 0) { // handle client connection in a separate process
            close(server_sock);
            inet_ntop(AF_INET, &(client_addr.sin_addr), new_addr, addrlen);
            memset(argvp[0], 0, _POSIX_PATH_MAX);
            snprintf(argvp[0], _POSIX_PATH_MAX,
                " proxy-connection[%s]", new_addr);
            argvp[1] = 0;
            handle_client(client_sock, client_addr);
            exit(0);
        } else
            connections_processed++;
        close(client_sock);
    }

}

/* Handle client connection */
void handle_client(int client_sock, struct sockaddr_in client_addr)
{
    const char to_find[] = "support.test.ca";
    char buffer[BUF_SIZE];
    char *conn_addr;
    int n;

    n = recv(client_sock, buffer, BUF_SIZE, MSG_PEEK);

    if (n > 0) {
        if (memmem(buffer, sizeof(buffer), to_find, sizeof(to_find)) == NULL) {
            plog(LOG_INFO, "New connection from console.");
            conn_addr = remote_host;
        } else {
            if (buffer[0x2b] == 0x20) {
                conn_addr = remote_host;
                plog(LOG_INFO, "New connection from mobile device.");
            } else {
                conn_addr = "127.0.0.1";
                plog(LOG_INFO, "New connection from browser.");
            }
        }
    } else {
        //perror("Connection aborted.");
        goto cleanup;
    }

    if ((remote_sock = create_connection(conn_addr, remote_port)) < 0) {
        plog(LOG_WARNING, "Cannot connect to host %s on port %u. %s",
            conn_addr, remote_port, strerror(errno));
        goto cleanup;
    }

    if (fork() == 0) { // a process forwarding data from client to remote socket
        if (cmd_out) {
            forward_data_ext(client_sock, remote_sock, cmd_out);
        } else {
            forward_data(client_sock, remote_sock);
        }
        exit(0);
    }

    if (fork() == 0) { // a process forwarding data from remote socket to client
        if (cmd_in) {
            forward_data_ext(remote_sock, client_sock, cmd_in);
        } else {
            forward_data(remote_sock, client_sock);
        }
        exit(0);
    }

cleanup:
    close(remote_sock);
    close(client_sock);
}

/* Forward data between sockets */
void forward_data(int source_sock, int destination_sock) {
    ssize_t n;

#ifdef USE_SPLICE
    int buf_pipe[2];

    if (pipe(buf_pipe) == -1) {
        plog(LOG_ERR, "pipe: %m");
        exit(CREATE_PIPE_ERROR);
    }

    while ((n = splice(source_sock, NULL, buf_pipe[WRITE], NULL, SSIZE_MAX, SPLICE_F_NONBLOCK|SPLICE_F_MOVE)) > 0) {
        if (splice(buf_pipe[READ], NULL, destination_sock, NULL, SSIZE_MAX, SPLICE_F_MOVE) < 0) {
            plog(LOG_ERR, "write: %m");
            exit(BROKEN_PIPE_ERROR);
        }
    }
#else
    char buffer[BUF_SIZE];

    while ((n = recv(source_sock, buffer, BUF_SIZE, 0)) > 0) { // read data from input socket
        send(destination_sock, buffer, n, 0); // send data to output socket
    }
#endif

    if (n < 0) {
        plog(LOG_ERR, "read: %m");
        exit(BROKEN_PIPE_ERROR);
    }

#ifdef USE_SPLICE
    close(buf_pipe[0]);
    close(buf_pipe[1]);
#endif

    shutdown(destination_sock, SHUT_RDWR); // stop other processes from using socket
    close(destination_sock);

    shutdown(source_sock, SHUT_RDWR); // stop other processes from using socket
    close(source_sock);
}

/* Forward data between sockets through external command */
void forward_data_ext(int source_sock, int destination_sock, char *cmd) {
    char buffer[BUF_SIZE];
    int n, i, pipe_in[2], pipe_out[2];

    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) { // create command input and output pipes
        plog(LOG_CRIT, "Cannot create pipe: %m");
        exit(CREATE_PIPE_ERROR);
    }

    if (fork() == 0) {
        dup2(pipe_in[READ], STDIN_FILENO); // replace standard input with input part of pipe_in
        dup2(pipe_out[WRITE], STDOUT_FILENO); // replace standard output with output part of pipe_out
        close(pipe_in[WRITE]); // close unused end of pipe_in
        close(pipe_out[READ]); // close unused end of pipe_out
        n = system(cmd); // execute command
        exit(n);
    } else {
        close(pipe_in[READ]); // no need to read from input pipe here
        close(pipe_out[WRITE]); // no need to write to output pipe here

        while ((n = recv(source_sock, buffer, BUF_SIZE, 0)) > 0) { // read data from input socket
            if (write(pipe_in[WRITE], buffer, n) < 0) { // write data to input pipe of external command
                plog(LOG_ERR, "Cannot write to pipe: %m");
                exit(BROKEN_PIPE_ERROR);
            }
            if ((i = read(pipe_out[READ], buffer, BUF_SIZE)) > 0) { // read command output
                send(destination_sock, buffer, i, 0); // send data to output socket
            }
        }

        shutdown(destination_sock, SHUT_RDWR); // stop other processes from using socket
        close(destination_sock);

        shutdown(source_sock, SHUT_RDWR); // stop other processes from using socket
        close(source_sock);
    }
}

/* Create client connection */
int create_connection(char *conn_host, int conn_port) {
    struct sockaddr_in server_addr;
    struct hostent *server;
    int sock;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        return CLIENT_SOCKET_ERROR;
    }

    if ((server = gethostbyname(conn_host)) == NULL) {
        errno = EFAULT;
        return CLIENT_RESOLVE_ERROR;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(conn_port);

    if (connect(sock, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        return CLIENT_CONNECT_ERROR;
    }

    return sock;
}
/* vim: set et ts=4 sw=4: */
