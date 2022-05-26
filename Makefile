OUT ?= build

EXEC := logger
EXEC := $(addprefix $(OUT)/,$(EXEC))

all: $(EXEC)

.PHONY: check clean

CC ?= gcc
CFLAGS = -Wall -std=gnu11 -g -O2
CFLAGS += -I. -D_GNU_SOURCE
LDFLAGS = -lpthread

OBJS := \
	logger.o \
	logger-colors.o \
	main.o

deps := $(OBJS:%.o=%.o.d)
OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(addprefix $(OUT)/,$(deps))

$(OUT)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ -MMD -MF $@.d $<

$(OUT)/logger: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): | $(OUT)

$(OUT):
	mkdir -p $@

.PRECIOUS: %.o

check: $(EXEC)
	@scripts/test.sh

clean:
	$(RM) $(EXEC) $(OBJS) $(deps)
	@rm -rf $(OUT)

-include $(deps)
