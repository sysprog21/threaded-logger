CFLAGS := -Wall -O2 -g -std=c11
CC ?= gcc

logger: logger.h logger.c main.c logger-colors.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o logger logger.c main.c logger-colors.c -lpthread

clean:
	rm -f logger out*.log

check: logger
	./test.sh
