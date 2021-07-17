EXEC = fifo_queue
.PHONY: all
all: $(EXEC)

CC ?= gcc
CFLAGS = -std=c11 -Wall -g
LDFLAGS = -lpthread -latomic

OBJS := fifo_queue.o

deps := $(OBJS:%.o=.%.o.d)

%.o: %.c
	$(CC) $(CFLAGS) -c -MMD -MF .$@.d -o $@ $<

$(EXEC): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

check: $(EXEC)
	@./$(EXEC)

clean:
	$(RM) $(EXEC) $(OBJS) $(deps)

-include $(deps)
