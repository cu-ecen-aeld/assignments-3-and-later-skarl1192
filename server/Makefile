# Makefile for aesdsocket.c

# Generates the application for the native build platform when GNU
# make variable CROSS_COMPILE is not specified on the make command line.
# When CROSS_COMPILE is specified with aarch64-none-linux-gnu- (note the
# trailing -)the makefile compiles using the cross compiler.
CC = $(CROSS_COMPILE)gcc

# Compiler flags
CFLAGS = -Wall -g
LDFLAGS = -lpthread -lrt

# Target executable/binary name
TARGET = aesdsocket

# Source and object files
SRC = aesdsocket.c
OBJ = aesdsocket.o

# The default target is 'all', which depends on the target executable/binary
all: $(TARGET)

# The target executable/binary depends on the object files
# Rule to link the object file into the target executable/binary
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET) $(LDFLAGS)

# The object files depend on the source files
# Rule to compile the .c files into the .o files
$(OBJ): $(SRC)
	$(CC) $(CFLAGS) -c $(SRC) -o $(OBJ)

# Clean
# Usage: `make clean` or `make CROSS_COMPILE=aarch64-none-linux-gnu- clean`
clean:
	rm -f $(TARGET) $(OBJ)

# Declare 'all' and 'clean' as phony targets
.PHONY: all clean
