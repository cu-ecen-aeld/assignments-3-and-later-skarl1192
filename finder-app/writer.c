#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>

/**
 * @brief Creates a file and writes a string to it.
 *
 * This program takes two command-line arguments: a full path to a file (including filename)
 * and a text string to write to that file. It creates the file if it doesn't exist or
 * overwrites it if it does exist. Logging is done using syslog.
 *
 * @param[in] argc The number of command-line arguments. Expected value is 3.
 * @param[in] argv An array of command-line argument strings.
 *                  argv[0] is the program name.
 *                  argv[1] is the path to the file (writeFile).
 *                  argv[2] is the string to write (writeStr).
 * @return 0 on success, 1 on failure.
 */
int main(int argc, char *argv[])
{
    openlog(NULL, LOG_PID | LOG_CONS, LOG_USER); 

    /* Check for correct number of arguments */
    if (argc != 3)
    {
        syslog(LOG_ERR, "Error: Exactly two arguments are required: writeFile and writeStr.");
        fprintf(stderr, "Error: Exactly two arguments are required: writeFile and writeStr.\n");
        closelog();
        return 1;
    }

    const char *writeFile = argv[1];
    const char *writeStr = argv[2];

    /* Open the file for writing */
    FILE *filePtr = fopen(writeFile, "w");
    if (filePtr == NULL)
    {
        int savedErrno = errno; // Store errno before other system calls
        syslog(LOG_ERR, "Failed to open file %s for writing: %s", writeFile, strerror(savedErrno));
        fprintf(stderr, "Error opening file %s: %s\n", writeFile, strerror(savedErrno));
        closelog();
        return 1;
    }

    /* Write the string to the file */
    if (fprintf(filePtr, "%s", writeStr) < 0)
    {
        int savedErrno = errno; // Store errno before other calls
        syslog(LOG_ERR, "Failed to write string to file %s: %s", writeFile, strerror(savedErrno));
        fprintf(stderr, "Error writing to file %s: %s\n", writeFile, strerror(savedErrno));
        fclose(filePtr);
        closelog();
        return 1;
    }

    /* Use the syslog capability to write a message “Writing <string> to <file> */
    syslog(LOG_DEBUG, "Writing %s to %s", writeStr, writeFile);

    /* Close the file */
    if (fclose(filePtr) != 0)
    {
        int savedErrno = errno; // Store errno before other calls
        syslog(LOG_ERR, "Failed to close file %s: %s", writeFile, strerror(savedErrno));
        fprintf(stderr, "Error closing file %s: %s\n", writeFile, strerror(savedErrno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}
