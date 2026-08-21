# toolset.mk - 设置Vitis环境并运行HLS tcl脚本

# 确保MODE参数已设置，默认为prj_gen
MODE ?= prj_gen
VITIS_HLS_ROOT ?= /opt/Xilinx_2020.2/Vitis_HLS/2020.2
VIVADO_ROOT ?= /opt/Xilinx_2020.2/Vivado/2020.2

# 默认目标
.PHONY: run_hls

# 运行HLS脚本
run_hls:
	@echo "Setting up Vitis environment..."
	@bash -c 'source $(VITIS_HLS_ROOT)/settings64.sh && \
	source $(VIVADO_ROOT)/settings64.sh && \
	echo "Running HLS for $(TARGET) with mode $(MODE)..." && \
	vitis_hls -f run_hls.tcl -tclargs $(TARGET) $(MODE)'
