CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

cmd: cmd.c
	$(CC) $(CFLAGS) -o $@ $<

test: cmd
	./cmd tictactoe.cmd /test

play: cmd
	./cmd tictactoe.cmd

clean:
	rm -f cmd
	rm -rf bin/tmp

.PHONY: test play clean
