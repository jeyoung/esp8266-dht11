SDK_HOME = /home/pi/source/github/esp-open-sdk/sdk
CC = $(SDK_HOME)/../xtensa-lx106-elf/bin/xtensa-lx106-elf-gcc
CFLAGS = -I. -mlongcalls -I$(SDK_HOME)/include -I$(SDK_HOME)/driver_lib/driver -I$(SDK_HOME)/driver_lib/include/driver
LDLIBS = -nostdlib -Wl,--start-group -lmain -lnet80211 -lwpa -llwip -lpp -lphy -lc -Wl,--end-group -lgcc -ldriver
LDFLAGS = -T$(SDK_HOME)/ld/eagle.app.v6.ld -L$(SDK_HOME)/lib

dht11-0x00000.bin: dht11
	esptool.py elf2image $^

dht11: dht11.o

dht11.o: dht11.c

flash: dht11-0x00000.bin
	esptool.py write_flash 0 dht11-0x00000.bin 0x10000 dht11-0x10000.bin

clean:
	rm -f dht11 dht11.o dht11-0x00000.bin dht11-0x10000.bin
