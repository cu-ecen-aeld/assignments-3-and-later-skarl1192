#!/bin/sh

# This script creates a new file `writefile` with content `writestr``

# Ensure the script is run with exactly two arguments
if [ $# -ne 2 ]
then
    echo "Error: Exactly two arguments are required: writefile and writestr."
    exit 1
fi

# Get command line arguments
# Note: Use quotes so that the shell doesn't misinterpret spaces
writefile="$1"
writestr="$2"

# Create the directory path if it doesn't exist
# This command doesn't create the file itself, just the directory
mkdir -p "$(dirname "$writefile")"

# Check exit status of mkdir to make sure it was successful
# If mkdir fails, it will return a non-zero exit status
if [ $? -ne 0 ]
then
    echo "Error: Could not create directory path for $writefile"
    exit 1
fi

# Write the content to the file, overwriting if it exists
echo "$writestr" > "$writefile"

# Check exit status of echo to make sure it was successful
if [ $? -ne 0 ]; then
    echo "Error: Could not create file $writefile"
    exit 1
fi

exit 0
