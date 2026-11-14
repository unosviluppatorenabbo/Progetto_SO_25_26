CC = gcc
CFLAGS = -Wvla -Wextra -Werror -Wall -D_GNU_SOURCE -g -I./
SRCDIR = src
BINDIR = bin
TARGETS = responsabile_mensa operatore operatore_cassa utente
BIN_TARGETS = $(addprefix $(BINDIR)/, $(TARGETS))

.PHONY: all clean start

all: $(BIN_TARGETS)

$(BINDIR):
	mkdir -p $@

$(BINDIR)/responsabile_mensa: $(SRCDIR)/responsabile_mensa.c $(SRCDIR)/helpers.c common.h | $(BINDIR)
	$(CC) $(CFLAGS) $(SRCDIR)/responsabile_mensa.c $(SRCDIR)/helpers.c -o $@

$(BINDIR)/operatore: $(SRCDIR)/operatore.c $(SRCDIR)/helpers.c common.h | $(BINDIR)
	$(CC) $(CFLAGS) $(SRCDIR)/operatore.c $(SRCDIR)/helpers.c -o $@

$(BINDIR)/operatore_cassa: $(SRCDIR)/operatore_cassa.c $(SRCDIR)/helpers.c common.h | $(BINDIR)
	$(CC) $(CFLAGS) $(SRCDIR)/operatore_cassa.c $(SRCDIR)/helpers.c -o $@

$(BINDIR)/utente: $(SRCDIR)/utente.c $(SRCDIR)/helpers.c common.h | $(BINDIR)
	$(CC) $(CFLAGS) $(SRCDIR)/utente.c $(SRCDIR)/helpers.c -o $@

start: $(BINDIR)/responsabile_mensa
	./$(BINDIR)/responsabile_mensa

clean:
	rm -rf $(BINDIR)
