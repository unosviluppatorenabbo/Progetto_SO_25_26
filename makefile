CC = gcc
CFLAGS = -Wvla -Wextra -Werror -Wall -D_GNU_SOURCE -g -I./
SRCDIR = src
TARGETS = responsabile_mensa operatore operatore_cassa utente

all: $(TARGETS)

responsabile_mensa: $(SRCDIR)/responsabile_mensa.c $(SRCDIR)/helpers.c common.h
	$(CC) $(CFLAGS) $(SRCDIR)/responsabile_mensa.c $(SRCDIR)/helpers.c -o $@

operatore: $(SRCDIR)/operatore.c $(SRCDIR)/helpers.c common.h
	$(CC) $(CFLAGS) $(SRCDIR)/operatore.c $(SRCDIR)/helpers.c -o $@

operatore_cassa: $(SRCDIR)/operatore_cassa.c $(SRCDIR)/helpers.c common.h
	$(CC) $(CFLAGS) $(SRCDIR)/operatore_cassa.c $(SRCDIR)/helpers.c -o $@

utente: $(SRCDIR)/utente.c $(SRCDIR)/helpers.c common.h
	$(CC) $(CFLAGS) $(SRCDIR)/utente.c $(SRCDIR)/helpers.c -o $@

clean:
	rm -f $(TARGETS)
