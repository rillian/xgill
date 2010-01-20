
CC = g++

# enable Yices solver
USE_YICES = 1

# enable CVC3 solver
USE_CVC3 = 1

# run 'make debug'
ifdef DEBUG
  OPT = -O0 -DDEBUG
else
  OPT = -O2
endif

WARNINGS = -Wall -Wno-non-virtual-dtor -Wno-strict-aliasing -Werror
CFLAGS = -g ${OPT} -I${PWD} ${WARNINGS}
LDFLAGS = -lz -lgmp

# run 'make profile' to enable profiling in generated binaries
ifdef PROFILE
  CFLAGS += -pg
  LDFLAGS += -pg
endif

BACKEND_INC = \
	backend/action.h \
	backend/backend.h \
	backend/backend_compound.h \
	backend/backend_graph.h \
	backend/backend_hash.h \
	backend/backend_util.h \
	backend/backend_xdb.h \
	backend/merge_lookup.h \
	backend/merge_lookup_impl.h \
	backend/operand.h \
	backend/serial.h \
	backend/timestamp.h \
	backend/transaction.h

IMLANG_INC = \
	imlang/bit.h \
	imlang/block.h \
	imlang/exp.h \
	imlang/filename.h \
	imlang/interface.h \
	imlang/loopsplit.h \
	imlang/opcode.h \
	imlang/serial.h \
	imlang/storage.h \
	imlang/trace.h \
	imlang/type.h \
	imlang/variable.h \
	imlang/visitor.h

MEMORY_INC = \
	memory/alias.h \
	memory/baked.h \
	memory/block.h \
	memory/callgraph.h \
	memory/clobber.h \
	memory/escape.h \
	memory/modset.h \
	memory/serial.h \
	memory/simplify.h \
	memory/storage.h \
	memory/summary.h

INFER_INC = \
	infer/expand.h \
	infer/infer.h \
	infer/invariant.h \
	infer/nullterm.h

CHECK_INC = \
	check/checker.h \
	check/decision.h \
	check/frame.h \
	check/path.h \
	check/propagate.h \
	check/sufficient.h \
	check/where.h

SOLVE_INC = \
	solve/constraint.h \
	solve/solver.h \
	solve/solver_hash.h \
	solve/solver-mux.h \
	solve/union_find.h

UTIL_INC = \
	util/alloc.h \
	util/assert.h \
	util/buffer.h \
	util/config.h \
	util/hashcache.h \
	util/hashcache_impl.h \
	util/hashcons.h \
	util/hashcons_impl.h \
	util/hashtable.h \
	util/hashtable_impl.h \
	util/list.h \
	util/monitor.h \
	util/primitive.h \
	util/stream.h \
	util/istream.h \
	util/ostream.h \
	util/timer.h \
	util/vector.h \
	util/xml.h

XDB_INC = \
	xdb/layout.h \
	xdb/xdb.h

INCLUDE = \
	${BACKEND_INC} \
	${IMLANG_INC} \
	${MEMORY_INC} \
	${INFER_INC} \
	${CHECK_INC} \
	${SOLVE_INC} \
	${UTIL_INC} \
	${XDB_INC}

BACKEND_OBJS = \
	backend/action.o \
	backend/backend.o \
	backend/backend_compound.o \
	backend/backend_graph.o \
	backend/backend_hash.o \
	backend/backend_util.o \
	backend/backend_xdb.o \
	backend/operand.o \
	backend/timestamp.o \
	backend/transaction.o

IMLANG_OBJS = \
	imlang/bit.o \
	imlang/block.o \
	imlang/exp.o \
	imlang/filename.o \
	imlang/interface.o \
	imlang/loopsplit.o \
	imlang/opcode.o \
	imlang/storage.o \
	imlang/trace.o \
	imlang/type.o \
	imlang/variable.o

MEMORY_OBJS = \
	memory/alias.o \
	memory/baked.o \
	memory/block.o \
	memory/callgraph.o \
	memory/clobber.o \
	memory/escape.o \
	memory/modset.o \
	memory/simplify.o \
	memory/storage.o \
	memory/summary.o

INFER_OBJS = \
	infer/expand.o \
	infer/infer.o \
	infer/invariant.o \
	infer/nullterm.o

CHECK_OBJS = \
	check/checker.o \
	check/decision.o \
	check/frame.o \
	check/path.o \
	check/propagate.o \
	check/sufficient.o \
	check/where.o

SOLVE_OBJS = \
	solve/constraint.o \
	solve/solver.o \
	solve/solver-mux.o \
	solve/union_find.o

UTIL_OBJS = \
	util/alloc.o \
	util/assert.o \
	util/buffer.o \
	util/config.o \
	util/hashcons.o \
	util/monitor.o \
	util/primitive.o \
	util/stream.o \
	util/timer.o \
	util/xml.o

XDB_OBJS = \
	xdb/layout.o \
	xdb/xdb.o

LIB_OBJS = \
	${BACKEND_OBJS} \
	${IMLANG_OBJS} \
	${MEMORY_OBJS} \
	${UTIL_OBJS} \
	${XDB_OBJS}

CHK_OBJS = \
	${INFER_OBJS} \
	${CHECK_OBJS} \
	${SOLVE_OBJS}

ALL_LIBS = \
	bin/libxcheck.a \
	bin/libxgill.a

ALL_BINS = \
	bin/xcheck \
	bin/xinfer \
	bin/xmemlocal \
	bin/xsource \
	bin/xdbfind \
	bin/xdbkeys \
	bin/xmanager

# additional settings for Yices.
ifdef USE_YICES
CFLAGS += -DSOLVER_YICES=1
INCLUDE += solve/solver-yices.h solve/wrapyices.h
CHK_OBJS += solve/solver-yices.o solve/wrapyices.o
ALL_LIBS += yices/lib/libyices.a
.have_yices: yices/lib/libyices.a yices/include/yices_c.h
else
.have_yices:
endif

# additional settings for CVC3.
ifdef USE_CVC3
CFLAGS += -DSOLVER_CVC3=1
INCLUDE += solve/solver-cvc3.h solve/cvc3_interface.h
CHK_OBJS += solve/solver-cvc3.o solve/cvc3_interface.o
ALL_LIBS += cvc3/lib/libcvc3.a
.have_cvc3: cvc3/lib/libcvc3.a cvc3/src/include/vc.h
else
.have_cvc3:
endif

%.o: %.cpp ${INCLUDE}
	${CC} ${CFLAGS} -c $< -o $@

all: build-libevent .have_yices .have_cvc3 ${ALL_LIBS} ${ALL_BINS} # build-elsa

debug:
	$(MAKE) all "DEBUG=1"

debug_xcheck:
	$(MAKE) bin/xcheck "DEBUG=1"

profile:
	$(MAKE) all "PROFILE=1"

bin/libxgill.a: ${LIB_OBJS}
	rm -f $@
	ar -r $@ ${LIB_OBJS}

bin/libxcheck.a: ${CHK_OBJS}
	rm -f $@
	ar -r $@ ${CHK_OBJS}

bin/xdbfind: main/xdbfind.o ${ALL_LIBS}
	${CC} $< -o $@ ${LDFLAGS} ${ALL_LIBS}

bin/xdbkeys: main/xdbkeys.o ${ALL_LIBS}
	${CC} $< -o $@ ${LDFLAGS} ${ALL_LIBS}

bin/xsource: main/xsource.o ${ALL_LIBS}
	${CC} $< -o $@ ${LDFLAGS} ${ALL_LIBS}

bin/xmemlocal: main/xmemlocal.o ${ALL_LIBS}
	${CC} $< -o $@ ${LDFLAGS} ${ALL_LIBS}

bin/xbrowse: main/xbrowse.o ${ALL_LIBS}
	${CC} $< -o $@ ${LDFLAGS} ${ALL_LIBS}

bin/xinfer: main/xinfer.o ${ALL_LIBS}
	${CC} $< -o $@ ${LDFLAGS} ${ALL_LIBS}

bin/xcheck: main/xcheck.o ${ALL_LIBS}
	${CC} $< -o $@ ${LDFLAGS} ${ALL_LIBS}

bin/xmanager: main/xmanager.o ${ALL_LIBS}
	${CC} libevent/*.o $< -o $@ ${LDFLAGS} ${ALL_LIBS}

# libevent stuff

build-libevent:
	make -C libevent

# Elsa frontend stuff

ifdef DEBUG

build-elsa:
	make -C elsa debug

else # DEBUG

build-elsa:
	make -C elsa

endif # DEBUG

# other

clean:
	rm -f ${LIB_OBJS} ${CHK_OBJS} bin/libmemory.a bin/libimlang.a ${ALL_BINS} main/*.o

complete_clean:
	$(MAKE) clean
	make -C elsa clean

complete:
	$(MAKE)
	make -C elsa

complete_debug:
	$(MAKE) debug
	make -C elsa debug
