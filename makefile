CC = gcc
CFLAGS = -Wvla -Wextra -Werror -Wall -D_GNU_SOURCE -g -I./
SRCDIR = src
BIN_DIR = bin
TARGETS = $(BIN_DIR)/responsabile_mensa $(BIN_DIR)/operatore $(BIN_DIR)/operatore_cassa $(BIN_DIR)/utente

all: $(TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/responsabile_mensa: $(SRCDIR)/responsabile_mensa.c $(SRCDIR)/helpers.c common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(SRCDIR)/responsabile_mensa.c $(SRCDIR)/helpers.c -o $@

$(BIN_DIR)/operatore: $(SRCDIR)/operatore.c $(SRCDIR)/helpers.c common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(SRCDIR)/operatore.c $(SRCDIR)/helpers.c -o $@

$(BIN_DIR)/operatore_cassa: $(SRCDIR)/operatore_cassa.c $(SRCDIR)/helpers.c common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(SRCDIR)/operatore_cassa.c $(SRCDIR)/helpers.c -o $@

$(BIN_DIR)/utente: $(SRCDIR)/utente.c $(SRCDIR)/helpers.c common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(SRCDIR)/utente.c $(SRCDIR)/helpers.c -o $@

.PHONY: all start clean

start: $(BIN_DIR)/responsabile_mensa
	./$(BIN_DIR)/responsabile_mensa

clean:
	rm -f $(TARGETS)
