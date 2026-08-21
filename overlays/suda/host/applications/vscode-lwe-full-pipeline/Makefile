ROOT_DIR := $(abspath $(CURDIR)/../..)
REMOTE_PROTOCOL_DIR := ../vscode-lwe-encrypt-remote-offload
include $(ROOT_DIR)/mk/common.mk

APP = vscode-lwe-full-pipeline
OBJS = vscode-lwe-full-pipeline.o lwe_remote_protocol.o

CXXFLAGS += -I$(ROOT_DIR)/api/libnvme/src
CXXFLAGS += -I$(REMOTE_PROTOCOL_DIR)

LIBS = $(ROOT_DIR)/api/libnvme/.build/src/libnvme.a
LIBS += -lssl -lcrypto -lstdc++ -lm -lpthread

.PHONY: all clean

all: $(APP)

$(APP): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

lwe_remote_protocol.o: $(REMOTE_PROTOCOL_DIR)/lwe_remote_protocol.cpp \
	$(REMOTE_PROTOCOL_DIR)/lwe_remote_protocol.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(APP) $(OBJS)
