CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11

ublk_churn_repro: src/ublk_churn_repro.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ublk_churn_repro

.PHONY: clean
