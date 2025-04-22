#include "systemcalls.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h> 
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{

    /* Call the system() function with the command set in the cmd
     * and return a boolean true if the system() call completed with success
     * or false() if it returned a failure*/
    int rtn = system(cmd);
    if(rtn == -1)
    {
        /* Call to system() itself failed */
        fprintf(stderr, "Error on system call\n");
        return false;
    }
    else
    {

        /* The system call spawns the child process and waits for it -
         * The return value of the system call is the exit status of the child process */

        /* Check if the child process terminated normally */
        if(WIFEXITED(rtn))
        {
            /* Get the exit status returned by the child process */
            if(WEXITSTATUS(rtn) == 0)
            {
                /* Child process terminated successfully */
                return true;
            }
            else
            {
                /* Child process terminated with an error */
                fprintf(stderr, "Error child process terminated with status: %d\n", WEXITSTATUS(rtn));
                return false;
            }
        }
        else
        {
            /* Child process terminated abnormally */
            fprintf(stderr, "Error child process terminated abnormally\n");
            return false;
        }
    }

    return true;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

    /* Execute a system command by calling fork, execv(),
     * and wait instead of system (see LSP page 161).
     * Use the command[0] as the full path to the command to execute
     * (first argument to execv), and use the remaining arguments
     * as second argument to the execv() command. */
    pid_t pid = fork();
    
    if(pid < 0)
    {
        /* Fork failed */
        fprintf(stderr, "Error on fork\n");
        va_end(args);
        return false;
    }
    else if(pid == 0)
    {
        /* Fork succeeded, this is the child process */
        execv(command[0], command);

        /* If execv() returns, an error occurred */
        fprintf(stderr, "Error on execv\n");
        exit(EXIT_FAILURE); // Exit child process with failure, don't use return
    }
    else
    {
        /* Fork succeeded, this is the parent process */
        va_end(args);

        /* Wait for the child process pid to complete */
        int wstatus = 0;
        pid_t wpid = waitpid(pid, &wstatus, 0);

        if(wpid == -1)
        {
            /* Waiting failed */
            fprintf(stderr, "Error on waitpid\n");
            return false;
        }

        /* Check if the child process terminated normally */
        if(WIFEXITED(wstatus))
        {
            /* Get the exit status returned by the child process */
            if(WEXITSTATUS(wstatus) == 0)
            {
                /* Child process terminated successfully */
                return true;
            }
            else
            {
                /* Child process terminated with an error */
                fprintf(stderr, "Error child process terminated with status: %d\n", WEXITSTATUS(wstatus));
                return false;
            }
        }
        else
        {
            /* Child process terminated abnormally */
            fprintf(stderr, "Error child process terminated abnormally\n");
            return false;
        }
    }
    return true;
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

    /*
    *   Call execv, but first using https://stackoverflow.com/a/13784315/1446624 as a refernce,
    *   redirect standard out to a file specified by outputfile.
    *   The rest of the behaviour is same as do_exec()
    */

    /* Open the file for writing (O_WRONLY), create if it doesn't exist (O_CREAT), truncate (O_TRUNC) if it does */
    int fd = open(outputfile, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd == -1)
    {
        fprintf(stderr, "Error opening file for redirection\n");
        return false;
    }

    pid_t pid = fork();
    
    if(pid < 0)
    {
        /* Fork failed */
        fprintf(stderr, "Error on fork\n");
        va_end(args);
        return false;
    }
    else if(pid == 0)
    {

        /* Redirect the standard output to the file descriptor 'fd' */
        if(dup2(fd, STDOUT_FILENO) < 0)
        {
            fprintf(stderr, "Error on dup2\n");
            close(fd);
            exit(EXIT_FAILURE); // Exit child process with failure
        }

        /* Close the file descriptor before calling execv */
        close(fd);

        /* Fork succeeded, this is the child process */
        execv(command[0], command);

        /* If execv() returns, an error occurred */
        fprintf(stderr, "Error on execv\n");
        exit(EXIT_FAILURE); // Exit child process with failure
    }
    else
    {
        /* Fork succeeded, this is the parent process */
        va_end(args);

        /* Wait for the child process pid to complete */
        int wstatus = 0;
        pid_t wpid = waitpid(pid, &wstatus, 0);

        if(wpid == -1)
        {
            /* Waiting failed */
            fprintf(stderr, "Error on waitpid\n");
            return false;
        }

        /* Check if the child process terminated normally */
        if(WIFEXITED(wstatus))
        {
            /* Get the exit status returned by the child process */
            if(WEXITSTATUS(wstatus) == 0)
            {
                /* Child process terminated successfully */
                return true;
            }
            else
            {
                /* Child process terminated with an error */
                fprintf(stderr, "Error child process terminated with status: %d\n", WEXITSTATUS(wstatus));
                return false;
            }
        }
        else
        {
            /* Child process terminated abnormally */
            fprintf(stderr, "Error child process terminated abnormally\n");
            return false;
        }
    }
    return true;
}
