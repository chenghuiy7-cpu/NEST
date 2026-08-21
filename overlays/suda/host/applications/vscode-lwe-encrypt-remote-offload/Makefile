ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(ROOT_DIR)/mk/common.mk

APP = vscode-lwe-encrypt-remote-offload
CXX_SRCS := vscode-lwe-encrypt-remote-offload.cpp lwe_remote_protocol.cpp

CXXFLAGS += -I$(ROOT_DIR)/api/libnvme/src

OBJS = $(CXX_SRCS:.cpp=.o)

LIBS = $(ROOT_DIR)/api/libnvme/.build/src/libnvme.a
LIBS += -lssl -lcrypto -lstdc++ -lm -lpthread

.PHONY: all clean test

all: $(APP)

$(APP): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

test_protocol: test_protocol.o lwe_remote_protocol.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lpthread

test: test_protocol
	./test_protocol

clean:
	rm -f $(APP) $(OBJS) test_protocol test_protocol.o
