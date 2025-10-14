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
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <limits.h>
#include "../aesd-char-driver/aesd_ioctl.h"

/* ---- Macros ---- */
/* Build switch to use /dev/aesdchar character device
 * instead of /var/tmp/aesdsocketdata and also disable timestamp printing */
#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#define SERVER_PORT 9000
#define BUFFER_SIZE 40000

#if USE_AESD_CHAR_DEVICE
#define PACKET_FILE "/dev/aesdchar"
#else
#define PACKET_FILE "/var/tmp/aesdsocketdata"
#endif

/* ---- Thread Data Structure ---- */
struct thread_data {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    bool thread_complete;
    struct thread_data *next;
};

/* ---- Global Variables ---- */
bool IntTermSignaled = false;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
struct thread_data *thread_list_head = NULL;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
#if !USE_AESD_CHAR_DEVICE
pthread_t timer_thread_id;
timer_t timer_id;
#endif

/* Function declarations */
static int send_file_to_client(int socketFd);
static int send_file_to_client_fd(int socketFd, int fileFd);
static bool parse_ioctl_seek_command(const char *buffer, size_t length, unsigned int *write_cmd, unsigned int *write_cmd_offset);


static void signal_handler(int signalNumber)
{
    if(signalNumber == SIGINT || signalNumber == SIGTERM)
    {
        syslog(LOG_INFO, "Caught signal, exiting");
        IntTermSignaled = true;
    }
}

#if !USE_AESD_CHAR_DEVICE
/* Timer signal handler for timestamp writing */
static void timer_handler(int sig, siginfo_t *si, void *uc)
{
    /* This handler is triggered by the timer */
    /* The actual work is done in timer_thread function */
    (void)sig;
    (void)si; 
    (void)uc;
}

/* Timer thread function */
static void* timer_thread(void* arg)
{
    (void)arg; /* Unused parameter */
    
    struct sigevent sev;
    struct itimerspec its;
    sigset_t mask;
    struct sigaction sa;
    
    /* Setup signal handler for timer */
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = timer_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGRTMIN, &sa, NULL) == -1) {
        syslog(LOG_ERR, "Error setting up timer signal handler: %s", strerror(errno));
        return NULL;
    }
    
    /* Block SIGRTMIN for all threads except this one */
    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN);
    
    /* Create timer */
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = &timer_id;
    if (timer_create(CLOCK_REALTIME, &sev, &timer_id) == -1) {
        syslog(LOG_ERR, "Error creating timer: %s", strerror(errno));
        return NULL;
    }
    
    /* Setup timer to fire every 10 seconds */
    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 10;
    its.it_interval.tv_nsec = 0;
    
    if (timer_settime(timer_id, 0, &its, NULL) == -1) {
        syslog(LOG_ERR, "Error setting timer: %s", strerror(errno));
        timer_delete(timer_id);
        return NULL;
    }
    
    /* Wait for timer signals and write timestamps */
    struct timespec timeout = {1, 0}; /* 1 second timeout */
    while (!IntTermSignaled) {
        int result = sigtimedwait(&mask, NULL, &timeout);
        if (result == SIGRTMIN) {
            /* Get current time */
            time_t current_time;
            struct tm *time_info;
            char timestamp_buffer[200];
            
            time(&current_time);
            time_info = localtime(&current_time);
            
            /* Format timestamp according to RFC 2822 */
            strftime(timestamp_buffer, sizeof(timestamp_buffer), 
                    "timestamp:%a, %d %b %Y %H:%M:%S %z\n", time_info);
            
            /* Lock mutex and write timestamp to file */
            pthread_mutex_lock(&file_mutex);
            FILE* timestamp_file = fopen(PACKET_FILE, "a");
            if (timestamp_file != NULL) {
                fwrite(timestamp_buffer, 1, strlen(timestamp_buffer), timestamp_file);
                fclose(timestamp_file);
            } else {
                syslog(LOG_ERR, "Error opening file for timestamp: %s", strerror(errno));
            }
            pthread_mutex_unlock(&file_mutex);
        } else if (result == -1 && errno == EAGAIN) {
            /* Timeout occurred - check IntTermSignaled and continue */
            continue;
        } else if (result == -1) {
            /* Other error occurred */
            syslog(LOG_ERR, "Error in sigtimedwait: %s", strerror(errno));
            break;
        }
    }
    
    /* Cleanup timer */
    timer_delete(timer_id);
    return NULL;
}
#endif /* !USE_AESD_CHAR_DEVICE */

/**
 * parse_ioctl_seek_command() - Parse AESDCHAR_IOCSEEKTO command string
 * @buffer: Buffer containing the potential command string
 * @length: Length of the buffer (should include newline)
 * @write_cmd: Output parameter for the write command index (X value)
 * @write_cmd_offset: Output parameter for the write command offset (Y value)
 *
 * Parses strings in the format "AESDCHAR_IOCSEEKTO:X,Y\n" where X and Y are
 * unsigned decimal integers. The newline must be present as it marks the end
 * of the command packet.
 *
 * Return: true if the buffer contains a valid AESDCHAR_IOCSEEKTO command,
 *         false otherwise
 */
static bool parse_ioctl_seek_command(const char *buffer, size_t length, 
                                     unsigned int *write_cmd, unsigned int *write_cmd_offset)
{
    /* Check minimum length: "AESDCHAR_IOCSEEKTO:0,0\n" = 23 chars */
    if (length < 23) {
        return false;
    }
    
    /* Check if buffer ends with newline */
    if (buffer[length - 1] != '\n') {
        return false;
    }
    
    /* Check if buffer starts with "AESDCHAR_IOCSEEKTO:" */
    const char *prefix = "AESDCHAR_IOCSEEKTO:";
    size_t prefix_len = strlen(prefix);
    if (strncmp(buffer, prefix, prefix_len) != 0) {
        return false;
    }
    
    /* Parse X,Y values after the prefix */
    const char *values_start = buffer + prefix_len;
    char *comma_ptr = strchr(values_start, ',');
    if (comma_ptr == NULL) {
        return false;
    }
    
    /* Parse X value (write_cmd) */
    char *endptr;
    unsigned long x_val = strtoul(values_start, &endptr, 10);
    if (endptr != comma_ptr) {
        return false;
    }
    
    /* Parse Y value (write_cmd_offset) */
    unsigned long y_val = strtoul(comma_ptr + 1, &endptr, 10);
    if (endptr != (buffer + length - 1)) {
        return false;
    }
    
    *write_cmd = (unsigned int)x_val;
    *write_cmd_offset = (unsigned int)y_val;
    return true;
}

/**
 * send_file_to_client_fd() - Send file contents to client using file descriptor
 * @socketFd: Socket file descriptor to send data to
 * @fileFd: File descriptor to read from (maintains file position across call)
 *
 * This function reads from the file descriptor and sends all data to the client.
 * Unlike send_file_to_client(), this uses a file descriptor instead of FILE*,
 * which allows the caller to perform ioctl operations and maintain file position
 * across the read operation. The file position is honored and updated as data is read.
 *
 * Return: 0 on success, 1 on error
 */
static int send_file_to_client_fd(int socketFd, int fileFd)
{
    char buffer[BUFFER_SIZE];
    ssize_t bytesRead = 0;
    ssize_t bytesSent = 0;
    ssize_t totalSent = 0;

    /* Read file chunk by chunk from current file position and send */
    while ((bytesRead = read(fileFd, buffer, BUFFER_SIZE)) > 0)
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
                return 1;
            }
            remainingToSend -= bytesSent;
            bufferPtr += bytesSent;
            totalSent += bytesSent;
        }
    }
    
    if (bytesRead < 0) {
        syslog(LOG_ERR, "Error %d (%s) reading from file", errno, strerror(errno));
        return 1;
    }

    return 0;
}

/* Thread function to handle client connections */
static void* handle_client(void* arg)
{
    struct thread_data* thread_info = (struct thread_data*)arg;
    int clientFd = thread_info->client_fd;
    char clientIpStr[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &(thread_info->client_addr.sin_addr), clientIpStr, INET_ADDRSTRLEN);
    
    /* Set socket timeout to allow periodic checking of IntTermSignaled */
    struct timeval timeout;
    timeout.tv_sec = 1;  /* 1 second timeout */
    timeout.tv_usec = 0;
    if (setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        syslog(LOG_ERR, "Error setting socket timeout: %s", strerror(errno));
    }
    
    /* Receive and process data on the accepted client connection */
    char receiveBuffer[BUFFER_SIZE];
    size_t receiveBufferLen = 0;
    ssize_t bytesReceived = 0;
    bool clientConnected = true;

    /* While client is connected and SIGINT/SIGTERM not received */
    while (clientConnected && !IntTermSignaled)
    {
        bytesReceived = recv(clientFd, receiveBuffer, sizeof(receiveBuffer), 0);

        if(bytesReceived == -1)
        {
            /* Error receiving data */
            if(errno == EINTR) continue; /* Interrupted by signal */
            if(errno == EAGAIN || errno == EWOULDBLOCK) continue; /* Timeout - check IntTermSignaled */
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

                /* Check if this is an AESDCHAR_IOCSEEKTO command */
                unsigned int write_cmd, write_cmd_offset;
                bool is_ioctl_cmd = parse_ioctl_seek_command(receiveBuffer, packetLen, 
                                                             &write_cmd, &write_cmd_offset);

                /* Lock mutex before file operations */
                pthread_mutex_lock(&file_mutex);
                
                if (is_ioctl_cmd) {                    
                    /* Open the device file with read/write access using file descriptor */
                    int fileFd = open(PACKET_FILE, O_RDWR);
                    if (fileFd < 0) {
                        syslog(LOG_ERR, "Error %d (%s) opening %s for ioctl", errno, strerror(errno), PACKET_FILE);
                        pthread_mutex_unlock(&file_mutex);
                        clientConnected = false;
                        break;
                    }
                    
                    /* Prepare ioctl structure */
                    struct aesd_seekto seekto;
                    seekto.write_cmd = write_cmd;
                    seekto.write_cmd_offset = write_cmd_offset;
                    
                    /* Send AESDCHAR_IOCSEEKTO ioctl to the driver
                     * The ioctl returns the new file position on success (>= 0)
                     * or a negative error code on failure */
                    long ioctl_result = ioctl(fileFd, AESDCHAR_IOCSEEKTO, &seekto);
                    if (ioctl_result < 0) {
                        syslog(LOG_ERR, "Error %d (%s) ioctl AESDCHAR_IOCSEEKTO failed (cmd=%u, offset=%u)", 
                               errno, strerror(errno), write_cmd, write_cmd_offset);
                        close(fileFd);
                        pthread_mutex_unlock(&file_mutex);
                        clientConnected = false;
                        break;
                    }
                    
                    /* Send file contents to client starting from the seeked position */
                    /* Use the same file descriptor to honor the file position set by ioctl */
                    if (send_file_to_client_fd(clientFd, fileFd) != 0) {
                        /* Error sending file content, assume client disconnected */
                        close(fileFd);
                        pthread_mutex_unlock(&file_mutex);
                        clientConnected = false;
                        break;
                    }
                    
                    /* Close the file descriptor */
                    close(fileFd);
                    
                } else {
                    /* Normal packet - write to file and send back contents */
                    
                    /* Create/Open the data file to write/append to */
                    FILE* outputFilePtr = fopen(PACKET_FILE, "a");
                    if(outputFilePtr == NULL)
                    {
                        syslog(LOG_ERR, "Error %d (%s) opening %s for appending", errno, strerror(errno), PACKET_FILE);
                        pthread_mutex_unlock(&file_mutex);
                        clientConnected = false;
                        break; // break from newline processing loop
                    }

                    /* Append to file, including the newline character */
                    fwrite(receiveBuffer, 1, packetLen, outputFilePtr);
                    fclose(outputFilePtr);

                    /* Send file to client */
                    if(send_file_to_client(clientFd) != 0)
                    {
                        /* Error sending file content, assume client disconnected */
                        pthread_mutex_unlock(&file_mutex);
                        clientConnected = false;
                        break; // break from newline processing loop
                    }
                }

                /* Unlock mutex after file operations */
                pthread_mutex_unlock(&file_mutex);

                /* Remove processed packet from buffer */
                size_t remainingLen = receiveBufferLen - packetLen;
                if(remainingLen > 0)
                {
                    memmove(receiveBuffer, receiveBuffer + packetLen, remainingLen);
                }
                receiveBufferLen = remainingLen;
            }

            /* If there are remaining bytes in the buffer, write them to file
             * Don't send the file to the connected client until full packet */
            if(receiveBufferLen > 0)
            {
                /* Lock mutex before writing to file */
                pthread_mutex_lock(&file_mutex);
                
                FILE* outputFilePtr = fopen(PACKET_FILE, "a");
                if(outputFilePtr == NULL)
                {
                    syslog(LOG_ERR, "Error %d (%s) opening %s for appending", errno, strerror(errno), PACKET_FILE);
                    pthread_mutex_unlock(&file_mutex);
                    clientConnected = false;
                }
                else
                {
                    fwrite(receiveBuffer, 1, receiveBufferLen, outputFilePtr);
                    fclose(outputFilePtr);
                    pthread_mutex_unlock(&file_mutex);
                    receiveBufferLen = 0;
                }
            }
        }
    }

    /* Cleanup after client disconnection */
    if(clientFd != -1)
    {
        close(clientFd);
        syslog(LOG_INFO, "Closed connection from %s", clientIpStr);
    }

    /* Mark thread as complete */
    thread_info->thread_complete = true;
    
    return NULL;
}

/* Send the packet file to the client */
static int send_file_to_client(int socketFd)
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

/* Cleanup completed threads */
static void cleanup_completed_threads(void)
{
    struct thread_data *thread_entry, *prev_entry = NULL;
    
    pthread_mutex_lock(&thread_list_mutex);
    thread_entry = thread_list_head;
    
    while(thread_entry != NULL)
    {
        if(thread_entry->thread_complete)
        {
            pthread_join(thread_entry->thread_id, NULL);
            
            /* Remove from list */
            if(prev_entry == NULL)
            {
                thread_list_head = thread_entry->next;
            }
            else
            {
                prev_entry->next = thread_entry->next;
            }
            
            struct thread_data *temp = thread_entry;
            thread_entry = thread_entry->next;
            free(temp);
        }
        else
        {
            prev_entry = thread_entry;
            thread_entry = thread_entry->next;
        }
    }
    pthread_mutex_unlock(&thread_list_mutex);
}

/* Cleanup all threads and wait for them to complete */
static void cleanup_all_threads(void)
{
    struct thread_data *thread_entry, *temp_entry;
    
    pthread_mutex_lock(&thread_list_mutex);
    thread_entry = thread_list_head;
    
    while(thread_entry != NULL)
    {
        pthread_join(thread_entry->thread_id, NULL);
        temp_entry = thread_entry->next;
        free(thread_entry);
        thread_entry = temp_entry;
    }
    
    thread_list_head = NULL;
    pthread_mutex_unlock(&thread_list_mutex);
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

#if !USE_AESD_CHAR_DEVICE
    /* Block SIGRTMIN signal for all threads. In the timer thread we will explicitly wait for it.
     * Note: The signal is delivered to the process, not to a specific thread.
     * The kernel picks ONE thread to handle the signal so we must block it on all
     * threads and explicity wait for it in the timer thread. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN);
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        syslog(LOG_ERR, "Error blocking SIGRTMIN: %s", strerror(errno));
        close(serverFd);
        closelog();
        return -1;
    }

    /* Create timer thread */
    if (pthread_create(&timer_thread_id, NULL, timer_thread, NULL) != 0) {
        syslog(LOG_ERR, "Error creating timer thread: %s", strerror(errno));
        close(serverFd);
        closelog();
        return -1;
    }
#endif

    /* Listen for incoming connections */
    if(listen(serverFd, 100) == -1)
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
            /* If accept() fails, check if it's due to signal interruption */
            if(errno == EINTR) continue; /* Interrupted by signal, check IntTermSignaled */
            /* Other errors, try again until SIGINT/SIGTERM is received */
            continue;
        }

        /* Log that a connection was accepted */
        char clientIpStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIpStr, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", clientIpStr);

        /* Create thread data structure */
        struct thread_data *new_thread = malloc(sizeof(struct thread_data));
        if(new_thread == NULL)
        {
            syslog(LOG_ERR, "Error %d (%s) malloc failed for thread data", errno, strerror(errno));
            close(clientFd);
            continue;
        }

        new_thread->client_fd = clientFd;
        new_thread->client_addr = clientAddr;
        new_thread->thread_complete = false;
        new_thread->next = NULL;

        /* Create thread to handle client connection */
        int thread_result = pthread_create(&new_thread->thread_id, NULL, handle_client, new_thread);
        if(thread_result != 0)
        {
            syslog(LOG_ERR, "Error %d (%s) pthread_create failed", thread_result, strerror(thread_result));
            close(clientFd);
            free(new_thread);
            continue;
        }

        /* Add thread to list */
        pthread_mutex_lock(&thread_list_mutex);
        new_thread->next = thread_list_head;
        thread_list_head = new_thread;
        pthread_mutex_unlock(&thread_list_mutex);

        /* Cleanup completed threads periodically */
        cleanup_completed_threads();
    }

    /* Wait for all threads to complete */
    cleanup_all_threads();

#if !USE_AESD_CHAR_DEVICE
    /* Wait for timer thread to complete */
    pthread_join(timer_thread_id, NULL);
#endif

    /* Close server socket */
    syslog(LOG_INFO, "Shutting down server.");
    if(serverFd != -1)
    {
        close(serverFd);
        serverFd = -1;
    }

#if !USE_AESD_CHAR_DEVICE
    /* Delete the data file if it exists */
    remove(PACKET_FILE);
#endif

    /* Close syslog connection */
    closelog();

    return 0;
}