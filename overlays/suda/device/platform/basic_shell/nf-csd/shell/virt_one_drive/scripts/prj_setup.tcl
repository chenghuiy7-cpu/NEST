# add custom_cpu source HDL files
add_files -norecurse -fileset sources_1 ${design_dir}/../fpga/sources/hdl/
# 添加源文件
add_files -fileset sources_1 [glob -nocomplain ${design_dir}/../fpga/sources/hlsaccframework/hlsacc_rspreceiver/*.v]
add_files -fileset sources_1 [glob -nocomplain ${design_dir}/../fpga/sources/hlsaccframework/hlsacc_scheduler/*.v]
add_files -fileset sources_1 [glob -nocomplain ${design_dir}/../fpga/sources/hlsaccframework/OperatorController/*.v]
add_files -fileset sources_1 [glob -nocomplain ${design_dir}/../fpga/sources/hlsaccframework/SimpleOperator/*.v]
add_files -fileset sources_1 [glob -nocomplain ${design_dir}/../fpga/sources/hlsaccframework/SoftResetMCDMA/*.v]
add_files -fileset sources_1 [glob -nocomplain ${design_dir}/../fpga/sources/hlsaccframework/blowfish_en/*.v]
add_files -fileset sources_1 [glob -nocomplain ${design_dir}/../fpga/sources/hlsaccframework/lwe_encrypt/*.v]
add_files -fileset sources_1 [glob -nocomplain ${design_dir}/../fpga/sources/hlsaccframework/lwe_decrypt/*.v]


# 更新编译顺序
update_compile_order -fileset sources_1
#set_property ip_repo_paths ${design_dir}/../fpga/sources/hlsaccframework [current_fileset]
#set_property ip_repo_paths ${design_dir}/../fpga/sources/vscode-op [current_fileset]
update_ip_catalog -rebuild

# setup block design
set bd_design mpsoc
source ${design_dir}/../fpga/scripts/${board}/${bd_design}.tcl

set bd_file_gen_loc \
    ./${vivado_prj_name}/${vivado_prj_name}.srcs/sources_1/bd
set bd_file_gen \
    ./${bd_file_gen_loc}/${bd_design}/${bd_design}.bd
	
set_property synth_checkpoint_mode None [get_files ${bd_file_gen}] 
generate_target all [get_files ${bd_file_gen}] 
	
make_wrapper -files [get_files ${bd_file_gen}] -top
exec cp -r ./${vivado_prj_name}/${vivado_prj_name}.gen/sources_1/bd/${bd_design} \
    ${bd_file_gen_loc}/
import_files -force -norecurse -fileset sources_1 \
    ./${bd_file_gen_loc}/${bd_design}/hdl/${bd_design}_wrapper.v
validate_bd_design
save_bd_design
close_bd_design ${bd_design}

# setup top module
set_property top mpsoc_wrapper [get_filesets sources_1]

# add constraints files
set main_constraints ${design_dir}/../fpga/constraints/${board}/top.xdc
add_files -fileset constrs_1 -norecurse ${main_constraints}

set ddr_constraints ${design_dir}/../fpga/constraints/${board}/ddr4_mig_phy_loc.xdc
add_files -fileset constrs_1 -norecurse ${ddr_constraints}

# Copy PCIe EP DCP file generated in dcp_gen stage
exec cp ${dcp_dir}/../../${prj_loc}_${prj_name}_pcie_ep_${board}/dcp/pcie_ep.dcp \
    ${dcp_dir}/

# Copy AccFramework DCP file generated in dcp_gen state
exec cp ${dcp_dir}/../../${prj_loc}_${prj_name}_accframework_${board}/dcp/accframework.dcp \
    ${dcp_dir}/
