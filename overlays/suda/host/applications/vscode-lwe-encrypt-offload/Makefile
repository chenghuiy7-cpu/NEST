ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(ROOT_DIR)/mk/common.mk

APP = vscode-lwe-encrypt-offload
CXX_SRCS := vscode-lwe-encrypt-offload.cpp

CXXFLAGS += -I$(ROOT_DIR)/api/libnvme/src

OBJS = $(CXX_SRCS:.cpp=.o)

LIBS = $(ROOT_DIR)/api/libnvme/.build/src/libnvme.a
LIBS += -lssl -lcrypto -lstdc++ -lm -lpthread

.PHONY: all clean

all: $(APP)

$(APP): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(APP) $(OBJS)
