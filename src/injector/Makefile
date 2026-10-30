# PS5 injector payload. Needs ps5-payload-sdk (https://github.com/ps5-payload-dev/sdk).
PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF      := ../../build/diinject.elf
BLOB     := ../../build/dihook.bin
OBJ      := ../../build/obj
OVL      := ../overlay
CFLAGS   := -Wall -O2 -I../core -DDI_INJECTOR=1
OVLFLAGS := -O2 -Wall -Wno-unused-function -I$(OVL)/common -I$(OVL)/controller -I$(OVL)/shsrv
CXXFLAGS := -std=c++20 -fno-exceptions $(OVLFLAGS)

C_SRC   := main.c got.c pt.c game.c tags.c catalog.c favs.c webremote.c cui.c ../core/diproto.c ../core/ditag.c ovl_renderer.c
OVL_C   := $(OVL)/controller/ovl_pt.c $(OVL)/controller/stable_elfldr_bridge.c
OVL_CXX := $(OVL)/controller/ovl_controller.cpp $(OVL)/controller/remote_call.cpp \
           $(OVL)/controller/shellui_hook_controller.cpp $(OVL)/controller/stable_shellui_injector.cpp
OBJS    := $(addprefix $(OBJ)/,$(notdir $(C_SRC:.c=.o) $(OVL_C:.c=.o) $(OVL_CXX:.cpp=.o)))

vpath %.c . ../core $(OVL)/controller
vpath %.cpp $(OVL)/controller

all: $(ELF)

blob.h: $(BLOB)
	cd ../../build && xxd -i -n DIHOOK_BIN dihook.bin | sed 's/^unsigned/static const unsigned/' > ../src/injector/blob.h

imports.h: ../hook/dihook.h ../../tools/gen_imports.py
	python3 ../../tools/gen_imports.py

ovl_renderer.c:
	$(MAKE) -C $(OVL)

$(OBJ)/got.o: got.c got.h imports.h ../hook/dihook.h
$(OBJ)/main.o: main.c build_id.h got.h blob.h imports.h ../hook/dihook.h game.h tags.h catalog.h webremote.h cui.h
$(OBJ)/game.o: game.c game.h catalog.h ../hook/dihook.h ../core/diproto.h
$(OBJ)/cui.o: cui.c build_id.h cui.h game.h catalog.h $(OVL)/common/ovl_proto.h

$(OBJ)/webremote.o: webremote.c build_id.h webremote.h game.h catalog.h cui.h

$(OBJ)/%.o: %.c
	@mkdir -p $(OBJ)
	$(CC) $(CFLAGS) $(if $(filter $(OVL)/%,$<),$(OVLFLAGS)) -c -o $@ $<

$(OBJ)/ovl_pt.o $(OBJ)/stable_elfldr_bridge.o: CFLAGS += $(OVLFLAGS)

$(OBJ)/%.o: %.cpp
	@mkdir -p $(OBJ)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(ELF): $(OBJS)
	$(CXX) -o $@ $(OBJS) -lkernel_sys -lpthread
	@size=$$(stat -c %s $@); echo "diinject.elf: $$size bytes"

clean:
	rm -rf $(ELF) blob.h $(OBJ)
