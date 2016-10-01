all: init
static: init-static
clean:
	rm -fr init init-static *.o

init: init.o

init-static: init.o
	$(CC) -static $< -o $@

.PHONY: all static clean
