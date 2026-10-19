# minivmm — Cloud Computing HW1
#
#   make                 build the VMM core and the three guests
#   make guests/cpu_burn_hb8.bin   build cpu_burn with one heartbeat exit every 2^8 iterations
#   make sweep           build cpu_burn for HB_SHIFT in $(SWEEP)
#   make test            run the hello guest directly (no panel)
#   make clean

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
LDLIBS  += -pthread
NASM    ?= nasm
SWEEP   ?= 0 4 8 12 16 20 24

GUESTS  := guests/hello.bin guests/cpu_burn.bin guests/mem_touch.bin

.PHONY: all guests sweep test clean

all: core/minivmm guests

guests: $(GUESTS)

core/minivmm: core/minivmm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

guests/%.bin: guests/%.asm
	$(NASM) -f bin -o $@ $<

guests/cpu_burn_hb%.bin: guests/cpu_burn.asm
	$(NASM) -f bin -DHB_SHIFT=$* -o $@ $<

sweep: $(foreach s,$(SWEEP),guests/cpu_burn_hb$(s).bin)

test: all
	./core/minivmm --mem 16 --vcpus 1 guests/hello.bin

clean:
	rm -f core/minivmm guests/*.bin
