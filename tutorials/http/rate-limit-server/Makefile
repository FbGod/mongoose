ROOT ?= ../../..
all build example:
	$(CC) main.c $(ROOT)/mongoose.c -I$(ROOT) -W -Wall -Werror -O2 -o server

clean:
	rm -f server
