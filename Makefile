all: init
clean:
	rm -fr init *.o

init: init.o

.PHONY: all clean
