all: init
static: init-static
clean:
	rm -fr init init-static *.o

stripped: init init-static
	strip -s init
	strip -s init-static

init: init.o

init-static: init.o
	$(CC) $(CFLAGS) -static $< -o $@

.PHONY: all static clean
