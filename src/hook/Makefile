# In-game hook blob (freestanding, position independent)
CC      = clang
LD      = ld.lld
CFLAGS  = -target x86_64-unknown-none-elf -Os -fPIE -ffreestanding -fno-builtin -nostdlib \
          -fno-stack-protector -fno-jump-tables -fno-asynchronous-unwind-tables -fvisibility=hidden \
          -mstackrealign -Wall -Wextra -Wno-unused-parameter -I../core
OBJS    = hooks.o diproto.o
all: ../../build/dihook.bin
hooks.o: hooks.c dihook.h grid_input.h ../core/diproto.h
	$(CC) $(CFLAGS) -c $< -o $@
diproto.o: ../core/diproto.c ../core/diproto.h
	$(CC) $(CFLAGS) -c $< -o $@
dihook.elf: $(OBJS) link.ld
	$(LD) -T link.ld -pie --no-dynamic-linker -z norelro -o $@ $(OBJS)
../../build/dihook.bin: dihook.elf mkblob.py dihook.h
	mkdir -p ../../build
	python3 mkblob.py $< $@
clean:
	rm -f *.o dihook.elf ../../build/dihook.bin
