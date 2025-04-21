#!/bin/sh

# This script counts the number of files in directory `filesdir` and
# the number of lines that match a given search string `searchstr` in those files

# Ensure the script is run with exactly two arguments
if [ $# -ne 2 ]
then
    echo "Error: Exactly two arguments are required: filesdir and searchstr."
    exit 1
fi

# Get command line arguments
# Note: Use quotes so that the shell doesn't misinterpret spaces
filesdir="$1"
searchstr="$2"

# Check if filesdir is a directory
if [ ! -d "$filesdir" ]
then
    echo "Error: ${filesdir} is not a directory."
    exit 1
fi

# Count the number of files and matching lines
numfiles=$(find "$filesdir" -type f | wc -l)
numlines=$(grep -r "$searchstr" "$filesdir" | wc -l)

# Print the message
echo "The number of files are ${numfiles} and the number of matching lines are ${numlines}"

exit 0