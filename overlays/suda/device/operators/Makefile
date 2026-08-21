# 定义目标名称
SWOP_TARGETS := grep
HWOP_TARGETS := blowfish lwe_encrypt lwe_decrypt

# 顶层目录路径
TOP_DIR := $(shell pwd)
SCRIPTS_DIR := $(TOP_DIR)/scripts
SHARED_INCLUDE_DIR := $(TOP_DIR)/../shared_components/hls

# Vitis和编译器路径
VITIS_INSTALL_DIR := /opt/Xilinx_2020.2/Vitis_HLS/2020.2
VITIS_HLS_INCLUDE := $(VITIS_INSTALL_DIR)/include


# ARM编译器路径
ARM_COMPILER_BASE := /opt/Xilinx_2020.2/Vitis/2020.2/gnu/aarch64/lin/aarch64-linux/bin
ARM_CC := $(ARM_COMPILER_BASE)/aarch64-linux-gnu-gcc
ARM_CXX := $(ARM_COMPILER_BASE)/aarch64-linux-gnu-g++

# X86编译器路径
X86_CC := /usr/bin/gcc
X86_CXX := /usr/bin/g++

# 共享的编译选项
COMMON_CFLAGS := -fpermissive -Wno-deprecated-declarations -Wno-attributes -Wno-strict-aliasing

# ARM特定编译选项
ARM_CFLAGS := $(COMMON_CFLAGS) -mcpu=cortex-a53 -mtune=cortex-a53 -ffast-math -fPIC -flto -g0 -O3

# X86特定编译选项
X86_CFLAGS := $(COMMON_CFLAGS) -fPIC -Os -O3

# 链接选项
LDFLAGS := -fvisibility=hidden

# 默认目标
.PHONY: all clean prj_gen hwop_gen sw_test swop_gen 

all: hwop_gen swop_gen

# 清理所有项目
clean:
	@echo "Cleaning all projects..."
	@echo "Cleaning Hardware Operators..."
	@for target in $(HWOP_TARGETS); do \
		if [ -d hls/$$target ]; then \
			rm -rf hls/$$target/solution1; \
			echo "Cleaned hls/$$target"; \
		fi; \
	@echo "Cleaning Software Operators..."
	@for target in $(SWOP_TARGETS); do \
		if [ -d hls/$$target ]; then \
			rm -rf hls/$$target/build; \
			rm -f hls/$$target/lib*.so; \
			rm -f hls/$$target/dynamiclib_test; \
			echo "Cleaned hls/$$target"; \
		fi; \
	done

# 为所有目标生成HLS项目
prj_gen: $(addsuffix _prj, $(HWOP_TARGETS))
	@echo "All HLS projects created."

# 为所有目标生成RTL (硬件操作)
hwop_gen: $(addsuffix _hwop, $(HWOP_TARGETS))
	@echo "All RTL generation completed."

# 为所有目标编译X86测试库并测试
sw_test: $(addsuffix _sw_test, $(SWOP_TARGETS))
	@echo "All SW tests completed."

# 为所有目标生成ARM库
swop_gen: $(addsuffix _swop, $(SWOP_TARGETS))
	@echo "All ARM libraries generated."

# 为每个目标创建项目的规则
%_prj:
	@target=$$(echo $@ | sed 's/_prj//'); \
	if [ -d hls/$$target ]; then \
		echo "Creating project for $$target..."; \
		if [ -f hls/$$target/run_hls.tcl ]; then \
			cd hls/$$target && \
			$(MAKE) -f $(SCRIPTS_DIR)/toolset.mk TARGET=$$target MODE=prj_gen run_hls; \
		else \
			echo "Warning: run_hls.tcl not found in hls/$$target, skipping..."; \
		fi; \
	else \
		echo "Warning: Directory hls/$$target does not exist, skipping..."; \
	fi

# 为每个目标生成RTL的规则
%_hwop:
	@target=$$(echo $@ | sed 's/_hwop//'); \
	if [ -d hls/$$target ]; then \
		echo "Generating RTL for $$target..."; \
		if [ -f hls/$$target/run_hls.tcl ]; then \
			cd hls/$$target && \
			$(MAKE) -f $(SCRIPTS_DIR)/toolset.mk TARGET=$$target MODE=rtl_gen run_hls; \
		else \
			echo "Warning: run_hls.tcl not found in hls/$$target, skipping..."; \
		fi; \
	else \
		echo "Warning: Directory hls/$$target does not exist, skipping..."; \
	fi
# 为每个目标编译X86库并运行测试的规则
%_sw_test:
	@target=$$(echo $@ | sed 's/_sw_test//'); \
	if [ -d hls/$$target ]; then \
		echo "Building X86 library and test for $$target..."; \
		if [ -f hls/$$target/sw_wrapper.cpp ] && [ -f hls/$$target/$$target.cpp ] && [ -f hls/$$target/dynamiclib_test.c ]; then \
			mkdir -p hls/$$target/build && \
			cd hls/$$target && \
			$(X86_CXX) $(X86_CFLAGS) -DEXPORT_SYMBOLS -shared \
				-I$(VITIS_HLS_INCLUDE)  -I$(SHARED_INCLUDE_DIR) \
				-Wl,--version-script,sw_wrapper.map \
				sw_wrapper.cpp $$target.cpp \
				-o lib$$target\_x86.so $(LDFLAGS) && \
			$(X86_CC) dynamiclib_test.c -Wl,-rpath,. -L. -l$$target\_x86 -o dynamiclib_test && \
			echo "Running test for $$target..." && \
			./dynamiclib_test; \
		else \
			echo "Warning: Required files not found in hls/$$target, skipping..."; \
		fi; \
	else \
		echo "Warning: Directory hls/$$target does not exist, skipping..."; \
	fi

# 为每个目标编译ARM库的规则
%_swop:
	@target=$$(echo $@ | sed 's/_swop//'); \
	if [ -d hls/$$target ]; then \
		echo "Building ARM library for $$target..."; \
		if [ -f hls/$$target/sw_wrapper.cpp ] && [ -f hls/$$target/$$target.cpp ]; then \
			mkdir -p hls/$$target/build && \
			cd hls/$$target && \
			$(ARM_CXX) $(ARM_CFLAGS) -DEXPORT_SYMBOLS -shared \
				-I$(VITIS_HLS_INCLUDE) -I$(SHARED_INCLUDE_DIR) \
				-Wl,--version-script,sw_wrapper.map \
				sw_wrapper.cpp $$target.cpp \
				-o lib$$target.so $(LDFLAGS) && \
			echo "ARM library built for $$target"; \
		else \
			echo "Warning: Required files not found in hls/$$target, skipping..."; \
		fi; \
	else \
		echo "Warning: Directory hls/$$target does not exist, skipping..."; \
	fi

# 为所有硬件目标拷贝Verilog文件
hwop_copy:
	@echo "Copying Verilog files for targets: $(HW_TARGETS)"
	@for target in $(HW_TARGETS); do \
		if [ -d hls/$$target ]; then \
			echo "Processing $$target..."; \
			if [ -f hls/$$target/$${target}_ip.zip ]; then \
				echo "Extracting IP zip file for $$target..."; \
				rm -rf hls/$$target/tmp_zip_extract; \
				mkdir -p hls/$$target/tmp_zip_extract; \
				cd hls/$$target/tmp_zip_extract && unzip -q ../$${target}_ip.zip; \
				\
				# 确定IP核的名称 (通常与目标名相同，但以防万一可以检查解压后的目录) \
				IP_NAME=$$(find . -type d -name "*.srcs" | head -1 | awk -F'/' '{print $$2}'); \
				if [ -z "$$IP_NAME" ]; then IP_NAME=$$target; fi; \
				echo "IP name determined as: $$IP_NAME"; \
				\
				# 检查Verilog目录是否存在 \
				VERILOG_PATH=$$(find . -type d -name "hdl" -o -name "verilog" | head -1); \
				if [ -z "$$VERILOG_PATH" ]; then \
					echo "Warning: Verilog directory not found in extracted IP for $$target"; \
				else \
					echo "Found Verilog directory: $$VERILOG_PATH"; \
					mkdir -p $(VERILOG_DEST_PATH)/$$IP_NAME; \
					cp -r $$VERILOG_PATH/* $(VERILOG_DEST_PATH)/$$IP_NAME/; \
					echo "Copied Verilog files to $(VERILOG_DEST_PATH)/$$IP_NAME/"; \
				fi; \
				\
				# 清理临时目录 \
				cd .. && rm -rf tmp_zip_extract; \
			else \
				echo "Warning: IP zip file not found for $$target, run hwop_gen first"; \
			fi; \
		else \
			echo "Warning: Directory hls/$$target does not exist, skipping..."; \
		fi; \
	done
