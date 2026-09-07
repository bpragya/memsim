RM        := /bin/rm -rf
SIM       := ./sim
CC        := g++

RAMULATOR_DIR := ../third_party/ramulator2
RAMULATOR_INC := $(RAMULATOR_DIR)/src
RAMULATOR_LIB := $(RAMULATOR_DIR)/build

#CFLAGS    := -lz -O2 -lm  -W -Wall -Wno-deprecated -Wno-unknown-warning-option -Wno-self-assign -Wno-unused-parameter -Wno-unused-but-set-variable
CFLAGS    := -g -O0 -std=c++20 -I$(RAMULATOR_INC)  -W -Wall -Wno-deprecated -Wno-unknown-warning-option -Wno-self-assign -Wno-unused-parameter -Wno-unused-but-set-variable
LDFLAGS   := -L$(RAMULATOR_LIB) -lramulator -lz -lm
DFLAGS    :=  -fsanitize=address
PFLAGS    := -pg




all:
	${CC} ${CFLAGS} mcore.c os.c  mcache.c sim.c memsys.c   -o ${SIM} ${LDFLAGS}
	cp $(RAMULATOR_LIB)/libramulator.dll . 2>/dev/null || true

dbg:
	${CC} ${CFLAGS} ${DFLAGS} mcore.c os.c  mcache.c sim.c memsys.c  -o ${SIM} ${LDFLAGS}
	cp $(RAMULATOR_LIB)/libramulator.dll . 2>/dev/null || true


clean:
	$(RM) ${SIM} *.o libramulator.dll
