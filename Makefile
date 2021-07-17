EXEC = test_prog
.PHONY: all
all: $(EXEC)

CC ?= gcc
CFLAGS = -std=c11 -Wall -g
LDFLAGS = -lpthread -latomic

OBJS := hzp_rec_mgr.o fifo_queue.o test_prog.o

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
