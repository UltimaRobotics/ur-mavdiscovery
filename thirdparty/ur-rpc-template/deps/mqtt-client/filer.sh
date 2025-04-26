#!/bin/bash

# Output file
OUTPUT_FILE="general.txt"
EXPORT_DIR="export"
LINES_PER_FILE=1100

# Create/Clear output file
> "$OUTPUT_FILE"

# Function to process each file and append to OUTPUT_FILE
process_file() {
    local file="$1"
    {
        echo "--------------------------------------------------------------------------------"
        echo "File: $file"
        echo "Path: $(realpath "$file")"
        echo "--------------------------------------------------------------------------------"
        cat "$file"
        echo -e "\n\n"
    } >> "$OUTPUT_FILE"
}

# Recursive directory traversal
traverse() {
    local dir="$1"
    for file in "$dir"/*; do
        if [ -f "$file" ]; then
            process_file "$file"
        elif [ -d "$file" ]; then
            traverse "$file"
        fi
    done
}

# Check for valid input
if [ $# -eq 0 ]; then
    echo "Usage: $0 <directory>"
    exit 1
fi

if [ ! -d "$1" ]; then
    echo "Error: Directory '$1' does not exist."
    exit 1
fi

# Process files
traverse "$1"

# Create export directory if it doesn't exist
mkdir -p "$EXPORT_DIR"

# Split general.txt into chunks of 150 lines each in the export directory
split -l "$LINES_PER_FILE" -d -a 3 "$OUTPUT_FILE" "$EXPORT_DIR/text"

# Rename split files to have .txt extension
for file in "$EXPORT_DIR"/text*; do
    mv "$file" "$file.txt"
done

echo "Done. Output split files are in the '$EXPORT_DIR' directory."

