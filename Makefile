# Makefile - FJSP+ SAT solver (Order Encoding) voi CaDiCaL rel-1.9.5
#
# Muc tieu:
#   make cadical   -> clone + build CaDiCaL (rel-1.9.5) tu repo cua tac gia vao thu muc third_party/cadical
#   make           -> bien dich fjsp_sat (tu dong build CaDiCaL truoc neu chua co)
#   make clean     -> xoa file object/binary cua du an nay (khong dong cham third_party/cadical)
#   make distclean -> xoa ca third_party/cadical

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
LDFLAGS  ?= -pthread

CADICAL_DIR   := third_party/cadical
CADICAL_REPO  := https://github.com/arminbiere/cadical.git
CADICAL_TAG   := rel-1.9.5
CADICAL_LIB   := $(CADICAL_DIR)/build/libcadical.a
CADICAL_INC   := $(CADICAL_DIR)/src

TARGET  := fjsp_sat
SRCS    := main.cpp
HEADERS := json_min.hpp instance.hpp cnf_encoder.hpp cadical_worker.hpp

.PHONY: all cadical clean distclean

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS) $(CADICAL_LIB)
	$(CXX) $(CXXFLAGS) -I$(CADICAL_INC) $(SRCS) $(CADICAL_LIB) -o $(TARGET) $(LDFLAGS)

# Clone + build CaDiCaL dung phien ban rel-1.9.5 tu repo goc cua tac gia (Armin Biere).
cadical: $(CADICAL_LIB)

$(CADICAL_LIB):
	mkdir -p third_party
	if [ ! -d "$(CADICAL_DIR)" ]; then \
		git clone --branch $(CADICAL_TAG) --depth 1 $(CADICAL_REPO) $(CADICAL_DIR); \
	fi
	cd $(CADICAL_DIR) && ./configure && $(MAKE)

clean:
	rm -f $(TARGET) *.o

distclean: clean
	rm -rf third_party
