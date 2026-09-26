# ---------------------------------------------------------------------------
# Out of This World SMS cutscene viewer - build
# code -> code.sms (32 KiB, banks 0-1) -> awstitch -> ootw-sms.sms (4 MiB)
# ---------------------------------------------------------------------------
DEVKIT   ?= $(HOME)/.devkitsms
PROG     := code

CC       := sdcc
IHX2SMS  := ihx2sms

CFLAGS   := -mz80 -I$(DEVKIT)/include --peep-file $(DEVKIT)/include/peep-rules.txt -Isrc
LDFLAGS  := -mz80 --no-std-crt0 --data-loc 0xC000

OBJS     := main.rel

all: ootw-sms.sms

src/fmv_data.h:
	python3 ../tools/awstitch.py --header-only

%.rel: src/%.c src/fmv_data.h src/font.h
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: $(PROG).ihx
$(PROG).ihx: $(OBJS)
	$(CC) -o $@ $(LDFLAGS) $(DEVKIT)/lib/crt0_sms.rel $(OBJS) \
	      $(DEVKIT)/lib/SMSlib.lib

$(PROG).sms: $(PROG).ihx
	$(IHX2SMS) $< $@

ootw-sms.sms: $(PROG).sms
	python3 ../tools/awstitch.py $(PROG).sms $@

clean:
	rm -f *.rel *.ihx *.sms *.lst *.map *.sym *.noi src/fmv_data.h

.PHONY: all clean
