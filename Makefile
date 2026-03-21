CC = gcc
CFLAGS = -Wall -g
TARGET = group16_scheduler
SRCS = group16_scheduler.c queue.c
OBJS = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c queue.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJS) $(TARGET)
