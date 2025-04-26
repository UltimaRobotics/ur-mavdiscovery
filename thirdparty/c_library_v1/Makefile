# Compiler
CC = gcc

# Compiler Flags
CFLAGS = -Wall -Wextra -std=c99

# Directories
SRC_DIR = .
OUT_DIR = build
LIB_DIR = /path/to/mavlink/library

# Source files
SRC = $(wildcard $(SRC_DIR)/*.c)
LIB_SRC = $(wildcard $(LIB_DIR)/**/*.c)

# Object files
OBJ = $(patsubst $(SRC_DIR)/%.c,$(OUT_DIR)/%.o,$(SRC))
LIB_OBJ = $(patsubst $(LIB_DIR)/%.c,$(OUT_DIR)/%.o,$(LIB_SRC))

# Executable name
TARGET = your_program

# Include directories
INC = -I$(SRC_DIR) -I$(LIB_DIR)/common

$(TARGET): $(OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(OUT_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(OUT_DIR)/%.o: $(LIB_DIR)/%.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

.PHONY: clean

clean:
	rm -f $(OUT_DIR)/*.o
	rm -f $(TARGET)

