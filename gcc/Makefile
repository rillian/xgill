
# set this to your GCC source directory.
GCCDIR= /path/to/gcc

# set this to your GCC build directory.
GCCBUILD= $(GCCDIR)/host-i686-pc-linux-gnu

GCC_INCLUDE= -I$(GCCBUILD)/gcc -I$(GCCDIR)/gcc -I$(GCCDIR)/include -I$(GCCDIR)/libcpp/include -I$(GCCDIR)/libcpp -I$(GCCDIR)/libdecnumber -I$(GCCDIR)/libdecnumber/bid

XGILL_OBJS= xgill.o type.o variable.o tree.o annot.o missing.o

all: xgill.so

xgill.so: $(XGILL_OBJS) ../bin/libxgill.a
	gcc -shared -o $@ $+ ../bin/libxgill.a -lstdc++ -lz

%.o: %.c xgill.h
	gcc -g -Werror -Wall -c -o $@ $< $(GCC_INCLUDE)

clean:
	rm -f *.o *.so
