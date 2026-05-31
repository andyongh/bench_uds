CC      = gcc
CFLAGS  = -O3 -Wall -Wextra -I./include -D_GNU_SOURCE
LDFLAGS = -lpthread -lm

SRCDIR  = src
BINDIR  = bin

BINS = $(BINDIR)/uds_server \
       $(BINDIR)/uds_client \
       $(BINDIR)/uds_throughput

.PHONY: all clean

all: $(BINDIR) $(BINS)
	@echo ""
	@echo "✓ Build complete. Binaries in ./bin/"

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/uds_server: $(SRCDIR)/uds_server.c include/uds_bench.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BINDIR)/uds_client: $(SRCDIR)/uds_client.c include/uds_bench.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BINDIR)/uds_throughput: $(SRCDIR)/uds_throughput.c include/uds_bench.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -rf $(BINDIR) results/*.csv /tmp/uds_bench*
