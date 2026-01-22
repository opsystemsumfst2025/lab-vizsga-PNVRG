CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
TARGET = wallstreet

all: $(TARGET)

$(TARGET): wallstreet.c
	$(CC) $(CFLAGS) -o $(TARGET) wallstreet.c

run: $(TARGET)
	./$(TARGET)

valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET)

clean:
	rm -f $(TARGET)
