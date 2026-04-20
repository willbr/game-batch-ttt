CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

# miniaudio backends on macOS: CoreAudio + AudioToolbox.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
AUDIO_LIBS := -framework CoreFoundation -framework CoreAudio -framework AudioToolbox -pthread
else
AUDIO_LIBS := -lm -ldl -lpthread
endif

# Windows-media sounds mapped to macOS system sounds. afconvert ships with macOS.
MEDIA_MAP := \
	tada=Hero \
	recycle=Pop \
	start=Ping \
	chimes=Glass \
	notify=Tink \
	ding=Tink \
	chord=Glass \
	ringout=Funk

MEDIA_DIR := bin/media
MEDIA_WAVS := $(foreach m,$(MEDIA_MAP),$(MEDIA_DIR)/$(firstword $(subst =, ,$(m))).wav)

cmd: cmd.o audio.o
	$(CC) $(CFLAGS) -o $@ cmd.o audio.o $(AUDIO_LIBS)

cmd.o: cmd.c audio.h
	$(CC) $(CFLAGS) -c -o $@ cmd.c

# miniaudio.h is 4MB; compile with fewer flags so a warm rebuild of cmd.c stays fast.
audio.o: audio.c audio.h miniaudio.h
	$(CC) -O2 -c -o $@ audio.c

$(MEDIA_DIR)/%.wav: /System/Library/Sounds
	@mkdir -p $(MEDIA_DIR)
	@src=$$(awk -v n=$* 'BEGIN{for(i=1;i<=split("$(MEDIA_MAP)",a," ");i++){split(a[i],kv,"=");if(kv[1]==n){print kv[2];exit}}}'); \
	if [ -z "$$src" ]; then echo "no mapping for $*"; exit 1; fi; \
	echo "afconvert /System/Library/Sounds/$$src.aiff -> $@"; \
	afconvert -f WAVE -d LEI16 /System/Library/Sounds/$$src.aiff $@

media: $(MEDIA_WAVS)

test: cmd media
	./cmd tictactoe.cmd /test

play: cmd media
	./cmd tictactoe.cmd

clean:
	rm -f cmd cmd.o audio.o
	rm -rf bin/tmp $(MEDIA_DIR)

.PHONY: test play clean media
