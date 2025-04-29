/* Define to include sigaction related functionality */
#define _POSIX_C_SOURCE 200809L

/* ---- Includes ---- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* ---- Macros ---- */
#define SERVER_PORT 9000
#define BUFFER_SIZE 40000
#define PACKET_FILE "/var/tmp/aesdsocketdata"

/* ---- Global Variables ---- */
bool IntTermSignaled = false;


static void signal_handler(int signalNumber)
{
    if(signalNumber == SIGINT || signalNumber == SIGTERM)
    {
        syslog(LOG_INFO, "Caught signal, exiting");
        IntTermSignaled = true;
    }
}

/* Send the packet file to the client */
static int sendfile(int socketFd)
{
    FILE *readFile = fopen(PACKET_FILE, "r");
    if(readFile == NULL)
    {
        syslog(LOG_ERR, "Error %d (%s) opening %s for reading", errno, strerror(errno), PACKET_FILE);
        return 1;
    }

    char buffer[BUFFER_SIZE];
    size_t bytesRead = 0;
    ssize_t bytesSent = 0;
    ssize_t totalSent = 0;

    /* Read file chunk by chunk and send */
    while ((bytesRead = fread(buffer, 1, BUFFER_SIZE, readFile)) > 0)
    {
        char *bufferPtr = buffer;
        size_t remainingToSend = bytesRead;
        while (remainingToSend > 0)
        {
            bytesSent = send(socketFd, bufferPtr, remainingToSend, 0);
            if(bytesSent == -1)
            {
                if(errno == EINTR) continue;
                syslog(LOG_ERR, "Error %d (%s) sending data to client", errno, strerror(errno));
                fclose(readFile);
                return 1;
            }
            remainingToSend -= bytesSent;
            bufferPtr += bytesSent;
            totalSent += bytesSent;
        }
    }

    fclose(readFile);
    return 0;
}


int main(int argc, char *argv[])
{
    /* Declare Local Variables */
    struct sockaddr_in serverAddr;
    int serverFd = -1;
    bool runAsDaemon = false;

    /* Setup logging to syslog */
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    /* Get command line arguments and check for flag `-d` to run as daemon */
    if(argc > 1)
    {
        if(strcmp(argv[1], "-d") == 0)
        {
            runAsDaemon = true;
        }
        else
        {
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            return 1;
        }
    }

    /* Setup and register signal handler for SIGINT and SIGTERM */
    struct sigaction sigAction;
    memset(&sigAction, 0, sizeof(sigAction));
    sigAction.sa_handler = signal_handler;
    if(sigaction(SIGINT, &sigAction, NULL) != 0)
    {
        syslog(LOG_ERR, "Error %d (%s) sigaction for SIGINT failed", errno, strerror(errno));
        closelog();
        return 1;
    }
    if(sigaction(SIGTERM, &sigAction, NULL) == -1)
    {
        syslog(LOG_ERR, "Error %d (%s) sigaction for SIGTERM failed", errno, strerror(errno));
        closelog();
        return 1;
    }

    /* Create and configure Server TCP socket */
    serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if(serverFd < 0)
    {
        syslog(LOG_ERR, "Error %d (%s) socket creation failed", errno, strerror(errno));
        closelog();
        return -1; // Return -1 if any socket connection steps fail
    }

    /* Allow reusing the address so we don't face any issues */
    int reuseOption = 1;
    if(setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuseOption, sizeof(reuseOption)) < 0)
    {
        syslog(LOG_ERR, "Error %d (%s) setsockopt SO_REUSEADDR failed", errno, strerror(errno));
        close(serverFd);
        serverFd = -1;
        closelog();
        return -1; // Return -1 if any socket connection steps fail
    }

    /* Bind socket to the port and any local address */
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(SERVER_PORT);

    if(bind(serverFd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1)
    {
        syslog(LOG_ERR, "Error %d (%s) socket bind failed", errno, strerror(errno));
        close(serverFd);
        serverFd = -1;
        closelog();
        return -1; // Return -1 if any socket connection steps fail
    }

    /* Run as daemon if configured to do so */
    if(runAsDaemon)
    {
        pid_t processId = fork();

        /* Check if fork failed */
        if(processId < 0)
        {   /* Fork failed */
            syslog(LOG_ERR, "Error %d (%s) fork failed", errno, strerror(errno));
            close(serverFd);
            serverFd = -1;
            closelog();
            return 1;
        }

        /* Check if this is the parent process */
        if(processId > 0)
        {
            /* If parent, return and let child run as daemon */
            return 0;
        }

        /* If we get here, this is the child process - continue as daemon */
        if(setsid() < 0)
        {   /* Create new session, detach from terminal */
            syslog(LOG_ERR, "Error %d (%s) setsid failed", errno, strerror(errno));
            close(serverFd);
            serverFd = -1;
            closelog();
            return 1;
        }

        /* Redirect standard file descriptors to /dev/null */
        int devnull = open("/dev/null", O_RDWR);
        if(devnull < 0)
        {
            syslog(LOG_ERR, "Error %d (%s) opening /dev/null", errno, strerror(errno));
        }
        else
        {
            if(dup2(devnull, STDIN_FILENO) < 0)  syslog(LOG_ERR, "Error %d (%s) dup2 stdin failed",  errno, strerror(errno));
            if(dup2(devnull, STDOUT_FILENO) < 0) syslog(LOG_ERR, "Error %d (%s) dup2 stdout failed", errno, strerror(errno));
            if(dup2(devnull, STDERR_FILENO) < 0) syslog(LOG_ERR, "Error %d (%s) dup2 stderr failed", errno, strerror(errno));
            
            /* Once dup2 has duplicated the fd onto 0,1,2, we can close the original */
            close(devnull);
        }

        syslog(LOG_INFO, "aesdsocket started as a daemon.");
    }


    /* Listen for incoming connections */
    if(listen(serverFd, 1) == -1)
    {
        syslog(LOG_ERR, "Error %d (%s) socket listen failed", errno, strerror(errno));
        close(serverFd);
        serverFd = -1;
        closelog();
        return -1; // Return -1 if any socket connection steps fail
    }

    syslog(LOG_INFO, "Server listening on port %d", SERVER_PORT);

    /* Loop until SIGINT/SIGTERM is received */
    while (!IntTermSignaled)
    {
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);

        /* Block waiting to accept an incoming connection */
        int clientFd = accept(serverFd, (struct sockaddr *)&clientAddr, &clientAddrLen);
        if(clientFd == -1)
        {
            /* If accept() fails, try again until SIGINT/SIGTERM is received */
            continue;
        }

        /* Log that a connection was accepted */
        char clientIpStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIpStr, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", clientIpStr);

        /* Receive and process data on the accpeted client connection */
        char receiveBuffer[BUFFER_SIZE];
        size_t receiveBufferLen = 0;
        ssize_t bytesReceived = 0;
        bool clientConnected = true;

        /* Create/Open the data file to write/append to */
        FILE* outputFilePtr = fopen(PACKET_FILE, "a");
        if(outputFilePtr == NULL)
        {
            syslog(LOG_ERR, "Error %d (%s) opening %s for appending", errno, strerror(errno), PACKET_FILE);
            clientConnected = false;
        }

        /* While client is connected and SIGINT/SIGTERM not received */
        while (clientConnected && !IntTermSignaled)
        {
            bytesReceived = recv(clientFd, receiveBuffer, sizeof(receiveBuffer), 0);

            if(bytesReceived == -1)
            {
                /* Error receiving data */
                if(errno == EINTR) continue; // Interrupted by signal
                clientConnected = false;
            }
            else if(bytesReceived == 0)
            {
                /* Client closed the connection */
                clientConnected = false;
            }
            else
            {
                /* Data received */
                receiveBufferLen += bytesReceived;

                /* Check for newline character(s) in the buffer */
                char *newlineCharPtr = NULL;
                while ((newlineCharPtr = memchr(receiveBuffer, '\n', receiveBufferLen)) != NULL)
                {
                    /* Found end of packet, signified by the newline character */
                    size_t packetLen = (newlineCharPtr - receiveBuffer) + 1;

                    /* Append to file, including the newline character */
                    fwrite(receiveBuffer, 1, packetLen, outputFilePtr);

                    /* Close file so the send function can open it and read from it */
                    fclose(outputFilePtr);
                    outputFilePtr = NULL;

                    /* Send file to client */
                    if(sendfile(clientFd) == -1)
                    {
                        /* Error sending file content, assume client disconnected */
                        clientConnected = false;
                        break; // break from newline processing loop
                    }

                    /* Reopen the file for appending */
                    outputFilePtr = fopen(PACKET_FILE, "a");
                    if(outputFilePtr == NULL)
                    {
                        syslog(LOG_ERR, "Error %d (%s) opening %s for appending", errno, strerror(errno), PACKET_FILE);
                        clientConnected = false;
                        break; // break from newline processing loop
                    }

                    /* Remove processed packet from buffer */
                    size_t remainingLen = receiveBufferLen - packetLen;
                    if(remainingLen > 0)
                    {
                        memmove(receiveBuffer, receiveBuffer + packetLen, remainingLen);
                    }
                    receiveBufferLen = remainingLen;
                }

                /* If there are remaining bytes in the buffer, append them to the file 
                 * Don't send the file to the connected client until full*/
                if(receiveBufferLen > 0)
                {
                    fwrite(receiveBuffer, 1, receiveBufferLen, outputFilePtr);
                    receiveBufferLen = 0;
                }
            }
        }

        /* Cleanup after client disconnection */
        if(outputFilePtr != NULL)
        {
            fclose(outputFilePtr);
            outputFilePtr = NULL;
        }

        if(clientFd != -1)
        {
            close(clientFd);
            clientFd = -1;
            syslog(LOG_INFO, "Closed connection from %s", clientIpStr);
        }
    }

    /* Close server socket */
    syslog(LOG_INFO, "Shutting down server.");
    if(serverFd != -1)
    {
        close(serverFd);
        serverFd = -1;
    }

    /* Delete the data file if it exists */
    remove(PACKET_FILE);

    /* Close syslog connection */
    closelog();

    return 0;
}