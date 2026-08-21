
################################################################
# This is a generated script based on design: accframework
#
# Though there are limitations about the generated script,
# the main purpose of this utility is to make learning
# IP Integrator Tcl commands easier.
################################################################

namespace eval _tcl {
proc get_script_folder {} {
   set script_path [file normalize [info script]]
   set script_folder [file dirname $script_path]
   return $script_folder
}
}
variable script_folder
set script_folder [_tcl::get_script_folder]

################################################################
# Check if script is running in correct Vivado version.
################################################################
set scripts_vivado_version 2020.2
set current_vivado_version [version -short]

if { [string first $scripts_vivado_version $current_vivado_version] == -1 } {
   puts ""
   catch {common::send_gid_msg -ssname BD::TCL -id 2041 -severity "ERROR" "This script was generated using Vivado <$scripts_vivado_version> and is being run in <$current_vivado_version> of Vivado. Please run the script in Vivado <$scripts_vivado_version> then open the design in Vivado <$current_vivado_version>. Upgrade the design by running \"Tools => Report => Report IP Status...\", then run write_bd_tcl to create an updated script."}

   return 1
}

################################################################
# START
################################################################

# To test this script, run the following commands from Vivado Tcl console:
# source accframework_script.tcl


# The design that will be created by this Tcl script contains the following 
# module references:
# AssScheduler, CtrlRspReceiver, OperatorController, OperatorController, accExamplePlusOperator, encrypt, two_user_wr_mem_access_throttler

# Please add the sources of those modules before sourcing this Tcl script.

# If there is no project opened, this script will create a
# project, but make sure you do not have an existing project
# <./myproj/project_1.xpr> in the current working folder.

set list_projs [get_projects -quiet]
if { $list_projs eq "" } {
   create_project project_1 myproj -part xczu19eg-ffvc1760-2-i
   set_property BOARD_PART fidus:none:part0:2.0 [current_project]
}


# CHANGE DESIGN NAME HERE
variable design_name
set design_name accframework

# If you do not already have an existing IP Integrator design open,
# you can create a design using the following command:
#    create_bd_design $design_name

# Creating design if needed
set errMsg ""
set nRet 0

set cur_design [current_bd_design -quiet]
set list_cells [get_bd_cells -quiet]

if { ${design_name} eq "" } {
   # USE CASES:
   #    1) Design_name not set

   set errMsg "Please set the variable <design_name> to a non-empty value."
   set nRet 1

} elseif { ${cur_design} ne "" && ${list_cells} eq "" } {
   # USE CASES:
   #    2): Current design opened AND is empty AND names same.
   #    3): Current design opened AND is empty AND names diff; design_name NOT in project.
   #    4): Current design opened AND is empty AND names diff; design_name exists in project.

   if { $cur_design ne $design_name } {
      common::send_gid_msg -ssname BD::TCL -id 2001 -severity "INFO" "Changing value of <design_name> from <$design_name> to <$cur_design> since current design is empty."
      set design_name [get_property NAME $cur_design]
   }
   common::send_gid_msg -ssname BD::TCL -id 2002 -severity "INFO" "Constructing design in IPI design <$cur_design>..."

} elseif { ${cur_design} ne "" && $list_cells ne "" && $cur_design eq $design_name } {
   # USE CASES:
   #    5) Current design opened AND has components AND same names.

   set errMsg "Design <$design_name> already exists in your project, please set the variable <design_name> to another value."
   set nRet 1
} elseif { [get_files -quiet ${design_name}.bd] ne "" } {
   # USE CASES: 
   #    6) Current opened design, has components, but diff names, design_name exists in project.
   #    7) No opened design, design_name exists in project.

   set errMsg "Design <$design_name> already exists in your project, please set the variable <design_name> to another value."
   set nRet 2

} else {
   # USE CASES:
   #    8) No opened design, design_name not in project.
   #    9) Current opened design, has components, but diff names, design_name not in project.

   common::send_gid_msg -ssname BD::TCL -id 2003 -severity "INFO" "Currently there is no design <$design_name> in project, so creating one..."

   create_bd_design $design_name

   common::send_gid_msg -ssname BD::TCL -id 2004 -severity "INFO" "Making design <$design_name> as current_bd_design."
   current_bd_design $design_name

}

common::send_gid_msg -ssname BD::TCL -id 2005 -severity "INFO" "Currently the variable <design_name> is equal to \"$design_name\"."

if { $nRet != 0 } {
   catch {common::send_gid_msg -ssname BD::TCL -id 2006 -severity "ERROR" $errMsg}
   return $nRet
}

set bCheckIPsPassed 1
##################################################################
# CHECK IPs
##################################################################
set bCheckIPs 1
if { $bCheckIPs == 1 } {
   set list_check_ips "\ 
xilinx.com:ip:axi_bram_ctrl:4.1\
xilinx.com:ip:axi_dwidth_converter:2.1\
xilinx.com:ip:axi_gpio:2.0\
xilinx.com:ip:axis_switch:1.1\
xilinx.com:ip:blk_mem_gen:8.4\
xilinx.com:ip:util_vector_logic:2.0\
xilinx.com:ip:xlconcat:2.1\
xilinx.com:ip:xlconstant:1.1\
xilinx.com:ip:xlslice:1.0\
"

   set list_ips_missing ""
   common::send_gid_msg -ssname BD::TCL -id 2011 -severity "INFO" "Checking if the following IPs exist in the project's IP catalog: $list_check_ips ."

   foreach ip_vlnv $list_check_ips {
      set ip_obj [get_ipdefs -all $ip_vlnv]
      if { $ip_obj eq "" } {
         lappend list_ips_missing $ip_vlnv
      }
   }

   if { $list_ips_missing ne "" } {
      catch {common::send_gid_msg -ssname BD::TCL -id 2012 -severity "ERROR" "The following IPs are not found in the IP Catalog:\n  $list_ips_missing\n\nResolution: Please add the repository containing the IP(s) to the project." }
      set bCheckIPsPassed 0
   }

}

##################################################################
# CHECK Modules
##################################################################
set bCheckModules 1
if { $bCheckModules == 1 } {
   set list_check_mods "\ 
AssScheduler\
CtrlRspReceiver\
OperatorController\
OperatorController\
accExamplePlusOperator\
encrypt\
lwe_encrypt\
lwe_decrypt\
two_user_wr_mem_access_throttler\
"

   set list_mods_missing ""
   common::send_gid_msg -ssname BD::TCL -id 2020 -severity "INFO" "Checking if the following modules exist in the project's sources: $list_check_mods ."

   foreach mod_vlnv $list_check_mods {
      if { [can_resolve_reference $mod_vlnv] == 0 } {
         lappend list_mods_missing $mod_vlnv
      }
   }

   if { $list_mods_missing ne "" } {
      catch {common::send_gid_msg -ssname BD::TCL -id 2021 -severity "ERROR" "The following module(s) are not found in the project: $list_mods_missing" }
      common::send_gid_msg -ssname BD::TCL -id 2022 -severity "INFO" "Please add source files for the missing module(s) above."
      set bCheckIPsPassed 0
   }
}

if { $bCheckIPsPassed != 1 } {
  common::send_gid_msg -ssname BD::TCL -id 2023 -severity "WARNING" "Will not continue with creation of design due to the error(s) above."
  return 3
}

##################################################################
# DESIGN PROCs
##################################################################



# Procedure to create entire design; Provide argument to make
# procedure reusable. If parentCell is "", will use root.
proc create_root_design { parentCell } {

  variable script_folder
  variable design_name

  if { $parentCell eq "" } {
     set parentCell [get_bd_cells /]
  }

  # Get object for parentCell
  set parentObj [get_bd_cells $parentCell]
  if { $parentObj == "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2090 -severity "ERROR" "Unable to find parent cell <$parentCell>!"}
     return
  }

  # Make sure parentObj is hier blk
  set parentType [get_property TYPE $parentObj]
  if { $parentType ne "hier" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2091 -severity "ERROR" "Parent <$parentObj> has TYPE = <$parentType>. Expected to be <hier>."}
     return
  }

  # Save current instance; Restore later
  set oldCurInst [current_bd_instance .]

  # Set parent object as current
  current_bd_instance $parentObj


  # Create interface ports
  set axi_mem_access [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 axi_mem_access ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {32} \
   CONFIG.NUM_READ_OUTSTANDING {2} \
   CONFIG.NUM_WRITE_OUTSTANDING {2} \
   CONFIG.PROTOCOL {AXI4} \
   ] $axi_mem_access

  set c2h_data [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 c2h_data ]

  set m_ext_axis [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 m_ext_axis ]

  set prp_out [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 prp_out ]

  set read_mem_cmd [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 read_mem_cmd ]

  set read_mem_data [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 read_mem_data ]
  set_property -dict [ list \
   CONFIG.HAS_TKEEP {1} \
   CONFIG.HAS_TLAST {1} \
   CONFIG.HAS_TREADY {1} \
   CONFIG.HAS_TSTRB {1} \
   CONFIG.LAYERED_METADATA {undef} \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {1} \
   CONFIG.TUSER_WIDTH {1} \
   ] $read_mem_data

  set s_axi_Manager [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 s_axi_Manager ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {16} \
   CONFIG.ARUSER_WIDTH {0} \
   CONFIG.AWUSER_WIDTH {0} \
   CONFIG.BUSER_WIDTH {0} \
   CONFIG.DATA_WIDTH {32} \
   CONFIG.HAS_BRESP {1} \
   CONFIG.HAS_BURST {0} \
   CONFIG.HAS_CACHE {0} \
   CONFIG.HAS_LOCK {0} \
   CONFIG.HAS_PROT {1} \
   CONFIG.HAS_QOS {0} \
   CONFIG.HAS_REGION {0} \
   CONFIG.HAS_RRESP {1} \
   CONFIG.HAS_WSTRB {1} \
   CONFIG.ID_WIDTH {0} \
   CONFIG.MAX_BURST_LENGTH {1} \
   CONFIG.NUM_READ_OUTSTANDING {1} \
   CONFIG.NUM_READ_THREADS {1} \
   CONFIG.NUM_WRITE_OUTSTANDING {1} \
   CONFIG.NUM_WRITE_THREADS {1} \
   CONFIG.PROTOCOL {AXI4LITE} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   CONFIG.RUSER_BITS_PER_BYTE {0} \
   CONFIG.RUSER_WIDTH {0} \
   CONFIG.SUPPORTS_NARROW_BURST {0} \
   CONFIG.WUSER_BITS_PER_BYTE {0} \
   CONFIG.WUSER_WIDTH {0} \
   ] $s_axi_Manager

  set s_ext_axis [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 s_ext_axis ]
  set_property -dict [ list \
   CONFIG.HAS_TKEEP {1} \
   CONFIG.HAS_TLAST {1} \
   CONFIG.HAS_TREADY {1} \
   CONFIG.HAS_TSTRB {1} \
   CONFIG.LAYERED_METADATA {undef} \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {8} \
   CONFIG.TID_WIDTH {4} \
   CONFIG.TUSER_WIDTH {8} \
   ] $s_ext_axis

  set write_mem_cmd [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 write_mem_cmd ]

  set write_mem_data [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 write_mem_data ]


  # Create ports
  set clk [ create_bd_port -dir I -type clk clk ]
  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {axi_mem_access:c2h_data:prp_out:read_mem_cmd:read_mem_data:write_mem_cmd:write_mem_data:s_axi_Manager:m_ext_axis:s_ext_axis} \
 ] $clk
  set resetn [ create_bd_port -dir I -type rst resetn ]

  # Create instance: AssScheduler_0, and set properties
  set block_name AssScheduler
  set block_cell_name AssScheduler_0
  if { [catch {set AssScheduler_0 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $AssScheduler_0 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }
  
  # Create instance: CtrlRspReceiver_0, and set properties
  set block_name CtrlRspReceiver
  set block_cell_name CtrlRspReceiver_0
  if { [catch {set CtrlRspReceiver_0 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $CtrlRspReceiver_0 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }
  
  # Create instance: OperatorController_0, and set properties
  set block_name OperatorController
  set block_cell_name OperatorController_0
  if { [catch {set OperatorController_0 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $OperatorController_0 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }
  
  # Create instance: OperatorController_1, and set properties
  set block_name OperatorController
  set block_cell_name OperatorController_1
  if { [catch {set OperatorController_1 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $OperatorController_1 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }

  # Create instance: OperatorController_2, and set properties
  set block_name OperatorController
  set block_cell_name OperatorController_2
  if { [catch {set OperatorController_2 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $OperatorController_2 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }

  # Create instance: OperatorController_3, and set properties
  set block_name OperatorController
  set block_cell_name OperatorController_3
  if { [catch {set OperatorController_3 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $OperatorController_3 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }
  
  # Create instance: accExamplePlusOperat_0, and set properties
  set block_name accExamplePlusOperator
  set block_cell_name accExamplePlusOperat_0
  if { [catch {set accExamplePlusOperat_0 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $accExamplePlusOperat_0 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }
  
  # Create instance: axi_bram_ctrl_0, and set properties
  set axi_bram_ctrl_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 axi_bram_ctrl_0 ]
  set_property -dict [ list \
   CONFIG.DATA_WIDTH {64} \
   CONFIG.ECC_TYPE {0} \
   CONFIG.SINGLE_PORT_BRAM {1} \
 ] $axi_bram_ctrl_0

  # Create instance: axi_bram_ctrl_1, and set properties
  set axi_bram_ctrl_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 axi_bram_ctrl_1 ]
  set_property -dict [ list \
   CONFIG.DATA_WIDTH {32} \
   CONFIG.ECC_TYPE {0} \
   CONFIG.PROTOCOL {AXI4LITE} \
   CONFIG.SINGLE_PORT_BRAM {1} \
   CONFIG.SUPPORTS_NARROW_BURST {0} \
 ] $axi_bram_ctrl_1

  # Create instance: axi_dwidth_converter_0, and set properties
  set axi_dwidth_converter_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dwidth_converter:2.1 axi_dwidth_converter_0 ]
  set_property -dict [ list \
   CONFIG.MI_DATA_WIDTH {32} \
   CONFIG.SI_DATA_WIDTH {64} \
 ] $axi_dwidth_converter_0

  # Create instance: axi_gpio_0, and set properties
  set axi_gpio_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 axi_gpio_0 ]
  set_property -dict [ list \
   CONFIG.C_ALL_INPUTS {1} \
   CONFIG.C_ALL_OUTPUTS {0} \
   CONFIG.C_ALL_OUTPUTS_2 {1} \
   CONFIG.C_DOUT_DEFAULT_2 {0x00000001} \
   CONFIG.C_GPIO2_WIDTH {1} \
   CONFIG.C_GPIO_WIDTH {22} \
   CONFIG.C_IS_DUAL {1} \
 ] $axi_gpio_0

  # Create instance: axi_interconnect_1, and set properties
  set axi_interconnect_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_interconnect_1 ]
  set_property -dict [ list \
   CONFIG.M00_HAS_REGSLICE {3} \
   CONFIG.M01_HAS_REGSLICE {3} \
   CONFIG.NUM_MI {4} \
   CONFIG.NUM_SI {1} \
   CONFIG.S00_HAS_REGSLICE {3} \
   CONFIG.STRATEGY {1} \
 ] $axi_interconnect_1

  # Create instance: axis_switch_0, and set properties
  set axis_switch_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_switch:1.1 axis_switch_0 ]
  set_property -dict [ list \
   CONFIG.DECODER_REG {1} \
   CONFIG.NUM_MI {4} \
   CONFIG.NUM_SI {1} \
 ] $axis_switch_0

  # Create instance: axis_switch_1, and set properties
  set axis_switch_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_switch:1.1 axis_switch_1 ]
  set_property -dict [ list \
   CONFIG.DECODER_REG {0} \
   CONFIG.NUM_MI {1} \
   CONFIG.NUM_SI {4} \
 ] $axis_switch_1

  # Create instance: axis_switch_2, and set properties
  set axis_switch_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_switch:1.1 axis_switch_2 ]
  set_property -dict [ list \
   CONFIG.DECODER_REG {1} \
   CONFIG.HAS_TLAST {1} \
   CONFIG.NUM_MI {4} \
   CONFIG.NUM_SI {1} \
 ] $axis_switch_2

  # Create instance: axis_switch_4, and set properties
  set axis_switch_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_switch:1.1 axis_switch_4 ]
  set_property -dict [ list \
   CONFIG.DECODER_REG {1} \
   CONFIG.M00_S00_CONNECTIVITY {0} \
   CONFIG.M00_S01_CONNECTIVITY {1} \
   CONFIG.M01_AXIS_HIGHTDEST {0x0000000f} \
   CONFIG.M01_S01_CONNECTIVITY {1} \
   CONFIG.M02_AXIS_BASETDEST {0x00000010} \
   CONFIG.M02_AXIS_HIGHTDEST {0x0000001f} \
   CONFIG.M02_S02_CONNECTIVITY {0} \
   CONFIG.M03_AXIS_BASETDEST {0x00000020} \
   CONFIG.M03_AXIS_HIGHTDEST {0x0000002f} \
   CONFIG.M03_S03_CONNECTIVITY {0} \
   CONFIG.M04_AXIS_BASETDEST {0x00000030} \
   CONFIG.M04_AXIS_HIGHTDEST {0x0000003f} \
   CONFIG.M04_S04_CONNECTIVITY {0} \
   CONFIG.NUM_MI {5} \
   CONFIG.NUM_SI {5} \
 ] $axis_switch_4

  # Create instance: cq, and set properties
  set cq [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 cq ]
  set_property -dict [ list \
   CONFIG.Byte_Size {9} \
   CONFIG.EN_SAFETY_CKT {false} \
   CONFIG.Enable_32bit_Address {false} \
   CONFIG.Enable_B {Use_ENB_Pin} \
   CONFIG.Memory_Type {Simple_Dual_Port_RAM} \
   CONFIG.Operating_Mode_A {NO_CHANGE} \
   CONFIG.Port_B_Clock {100} \
   CONFIG.Port_B_Enable_Rate {100} \
   CONFIG.Register_PortA_Output_of_Memory_Primitives {false} \
   CONFIG.Register_PortB_Output_of_Memory_Primitives {false} \
   CONFIG.Use_Byte_Write_Enable {false} \
   CONFIG.Use_RSTA_Pin {false} \
   CONFIG.Write_Depth_A {256} \
   CONFIG.use_bram_block {Stand_Alone} \
 ] $cq

  # Create instance: encrypt_0, and set properties
  set block_name encrypt
  set block_cell_name encrypt_0
  if { [catch {set encrypt_0 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $encrypt_0 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }

  # Create instance: lwe_encrypt_0, and set properties
  set block_name lwe_encrypt
  set block_cell_name lwe_encrypt_0
  if { [catch {set lwe_encrypt_0 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $lwe_encrypt_0 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }

  # Create instance: lwe_decrypt_0, and set properties
  set block_name lwe_decrypt
  set block_cell_name lwe_decrypt_0
  if { [catch {set lwe_decrypt_0 [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $lwe_decrypt_0 eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }
  
  # Create instance: host_write_mem_ctrl, and set properties
  set block_name two_user_wr_mem_access_throttler
  set block_cell_name host_write_mem_ctrl
  if { [catch {set host_write_mem_ctrl [create_bd_cell -type module -reference $block_name $block_cell_name] } errmsg] } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2095 -severity "ERROR" "Unable to add referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   } elseif { $host_write_mem_ctrl eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2096 -severity "ERROR" "Unable to referenced block <$block_name>. Please add the files for ${block_name}'s definition into the project."}
     return 1
   }
    set_property -dict [ list \
   CONFIG.MEM_TYPE {"1"} \
 ] $host_write_mem_ctrl

  # Create instance: sq, and set properties
  set sq [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 sq ]
  set_property -dict [ list \
   CONFIG.Byte_Size {8} \
   CONFIG.EN_SAFETY_CKT {false} \
   CONFIG.Enable_32bit_Address {false} \
   CONFIG.Enable_B {Use_ENB_Pin} \
   CONFIG.Memory_Type {Simple_Dual_Port_RAM} \
   CONFIG.Operating_Mode_A {NO_CHANGE} \
   CONFIG.Port_B_Clock {100} \
   CONFIG.Port_B_Enable_Rate {100} \
   CONFIG.Read_Width_A {64} \
   CONFIG.Read_Width_B {64} \
   CONFIG.Register_PortA_Output_of_Memory_Primitives {false} \
   CONFIG.Register_PortB_Output_of_Memory_Primitives {false} \
   CONFIG.Use_Byte_Write_Enable {true} \
   CONFIG.Use_RSTA_Pin {false} \
   CONFIG.Write_Depth_A {256} \
   CONFIG.Write_Width_A {64} \
   CONFIG.Write_Width_B {64} \
   CONFIG.use_bram_block {Stand_Alone} \
 ] $sq

  # Create instance: static_var_bram, and set properties
  set static_var_bram [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 static_var_bram ]
  set_property -dict [ list \
   CONFIG.Byte_Size {8} \
   CONFIG.EN_SAFETY_CKT {true} \
   CONFIG.Enable_32bit_Address {true} \
   CONFIG.Operating_Mode_A {WRITE_FIRST} \
   CONFIG.Read_Width_A {512} \
   CONFIG.Read_Width_B {512} \
   CONFIG.Register_PortA_Output_of_Memory_Primitives {false} \
   CONFIG.Use_Byte_Write_Enable {true} \
   CONFIG.Use_RSTA_Pin {true} \
   CONFIG.Write_Depth_A {512} \
   CONFIG.Write_Width_A {512} \
   CONFIG.Write_Width_B {512} \
   CONFIG.use_bram_block {Stand_Alone} \
 ] $static_var_bram

  # Create instance: static_var_bram1, and set properties
  set static_var_bram1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 static_var_bram1 ]
  set_property -dict [ list \
   CONFIG.Byte_Size {8} \
   CONFIG.EN_SAFETY_CKT {true} \
   CONFIG.Enable_32bit_Address {true} \
   CONFIG.Operating_Mode_A {WRITE_FIRST} \
   CONFIG.Read_Width_A {512} \
   CONFIG.Read_Width_B {512} \
   CONFIG.Register_PortA_Output_of_Memory_Primitives {false} \
   CONFIG.Use_Byte_Write_Enable {true} \
   CONFIG.Use_RSTA_Pin {true} \
   CONFIG.Write_Depth_A {512} \
   CONFIG.Write_Width_A {512} \
   CONFIG.Write_Width_B {512} \
   CONFIG.use_bram_block {Stand_Alone} \
 ] $static_var_bram1

  # Create instance: static_var_bram2, and set properties
  set static_var_bram2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 static_var_bram2 ]
  set_property -dict [ list \
   CONFIG.Byte_Size {8} \
   CONFIG.EN_SAFETY_CKT {true} \
   CONFIG.Enable_32bit_Address {true} \
   CONFIG.Operating_Mode_A {WRITE_FIRST} \
   CONFIG.Read_Width_A {512} \
   CONFIG.Read_Width_B {512} \
   CONFIG.Register_PortA_Output_of_Memory_Primitives {false} \
   CONFIG.Use_Byte_Write_Enable {true} \
   CONFIG.Use_RSTA_Pin {true} \
   CONFIG.Write_Depth_A {512} \
   CONFIG.Write_Width_A {512} \
   CONFIG.Write_Width_B {512} \
   CONFIG.use_bram_block {Stand_Alone} \
 ] $static_var_bram2

  # Create instance: static_var_bram3, and set properties
  set static_var_bram3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 static_var_bram3 ]
  set_property -dict [ list \
   CONFIG.Byte_Size {8} \
   CONFIG.EN_SAFETY_CKT {true} \
   CONFIG.Enable_32bit_Address {true} \
   CONFIG.Operating_Mode_A {WRITE_FIRST} \
   CONFIG.Read_Width_A {512} \
   CONFIG.Read_Width_B {512} \
   CONFIG.Register_PortA_Output_of_Memory_Primitives {false} \
   CONFIG.Use_Byte_Write_Enable {true} \
   CONFIG.Use_RSTA_Pin {true} \
   CONFIG.Write_Depth_A {512} \
   CONFIG.Write_Width_A {512} \
   CONFIG.Write_Width_B {512} \
   CONFIG.use_bram_block {Stand_Alone} \
 ] $static_var_bram3

  # Create instance: util_vector_logic_0, and set properties
  set util_vector_logic_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic:2.0 util_vector_logic_0 ]
  set_property -dict [ list \
   CONFIG.C_SIZE {1} \
 ] $util_vector_logic_0

  # Create instance: xlconcat_0, and set properties
  set xlconcat_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 xlconcat_0 ]
  set_property -dict [ list \
   CONFIG.IN2_WIDTH {10} \
   CONFIG.IN3_WIDTH {10} \
   CONFIG.NUM_PORTS {4} \
 ] $xlconcat_0

  # Create instance: xlconcat_1, and set properties
  set xlconcat_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 xlconcat_1 ]
  set_property -dict [ list \
   CONFIG.IN0_WIDTH {3} \
   CONFIG.IN1_WIDTH {5} \
 ] $xlconcat_1

  # Create instance: xlconstant_0, and set properties
  set xlconstant_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_0 ]
  set_property -dict [ list \
   CONFIG.CONST_VAL {1} \
   CONFIG.CONST_WIDTH {1} \
 ] $xlconstant_0

  # Create instance: xlconstant_1, and set properties
  set xlconstant_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_1 ]
  set_property -dict [ list \
   CONFIG.CONST_VAL {0} \
 ] $xlconstant_1

  # Create instance: xlconstant_2, and set properties
  set xlconstant_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_2 ]
  set_property -dict [ list \
   CONFIG.CONST_VAL {0} \
   CONFIG.CONST_WIDTH {64} \
 ] $xlconstant_2

  # Create instance: xlconstant_3, and set properties
  set xlconstant_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_3 ]
  set_property -dict [ list \
   CONFIG.CONST_VAL {1} \
 ] $xlconstant_3

  # Create instance: xlconstant_4, and set properties
  set xlconstant_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_4 ]
  set_property -dict [ list \
   CONFIG.CONST_VAL {0} \
   CONFIG.CONST_WIDTH {512} \
 ] $xlconstant_4

  # Create instance: xlconstant_5, and set properties
  set xlconstant_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_5 ]
  set_property -dict [ list \
   CONFIG.CONST_VAL {0} \
 ] $xlconstant_5

  # Create instance: xlconstant_6, and set properties
  set xlconstant_6 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_6 ]
  set_property -dict [ list \
   CONFIG.CONST_VAL {2} \
   CONFIG.CONST_WIDTH {4} \
 ] $xlconstant_6

  # Create instance: xlconstant_7, and set properties
  set xlconstant_7 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_7 ]
  set_property -dict [ list \
   CONFIG.CONST_VAL {3} \
   CONFIG.CONST_WIDTH {4} \
 ] $xlconstant_7

  # Create instance: xlslice_1, and set properties
  set xlslice_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 xlslice_1 ]
  set_property -dict [ list \
   CONFIG.DIN_FROM {11} \
   CONFIG.DIN_TO {2} \
   CONFIG.DIN_WIDTH {12} \
   CONFIG.DOUT_WIDTH {10} \
 ] $xlslice_1

  # Create interface connections
  connect_bd_intf_net -intf_net AssScheduler_0_acc_context_data_send_to_acc [get_bd_intf_pins AssScheduler_0/acc_context_data_send_to_acc] [get_bd_intf_pins axis_switch_2/S00_AXIS]
  connect_bd_intf_net -intf_net AssScheduler_0_acc_context_data_send_to_datamover [get_bd_intf_ports write_mem_data] [get_bd_intf_pins AssScheduler_0/acc_context_data_send_to_datamover]
  connect_bd_intf_net -intf_net AssScheduler_0_acc_context_recovery [get_bd_intf_ports read_mem_cmd] [get_bd_intf_pins AssScheduler_0/acc_context_recovery]
  connect_bd_intf_net -intf_net AssScheduler_0_acc_context_save [get_bd_intf_ports write_mem_cmd] [get_bd_intf_pins AssScheduler_0/acc_context_save]
  connect_bd_intf_net -intf_net AssScheduler_0_ctrl_req_to_acc [get_bd_intf_pins AssScheduler_0/ctrl_req_to_acc] [get_bd_intf_pins axis_switch_0/S00_AXIS]
  connect_bd_intf_net -intf_net CtrlRspReceiver_0_operator_done_signal [get_bd_intf_pins AssScheduler_0/operator_done_signal] [get_bd_intf_pins CtrlRspReceiver_0/operator_done_signal]
  connect_bd_intf_net -intf_net OperatorController_0_ctrl_rsp_to_ctrl [get_bd_intf_pins AssScheduler_0/ctrl_rsp_from_acc] [get_bd_intf_pins CtrlRspReceiver_0/ctrl_rsp_from_acc_out]
  connect_bd_intf_net -intf_net OperatorController_0_ctrl_rsp_to_ctrl1 [get_bd_intf_pins OperatorController_0/ctrl_rsp_to_ctrl] [get_bd_intf_pins axis_switch_1/S00_AXIS]
  connect_bd_intf_net -intf_net OperatorController_0_m_axis_inside1 [get_bd_intf_pins OperatorController_0/m_axis_inside] [get_bd_intf_pins accExamplePlusOperat_0/data_in]
  connect_bd_intf_net -intf_net OperatorController_0_m_axis_outside [get_bd_intf_pins OperatorController_0/m_axis_outside] [get_bd_intf_pins axis_switch_4/S00_AXIS]
  connect_bd_intf_net -intf_net OperatorController_1_ctrl_rsp_to_ctrl [get_bd_intf_pins OperatorController_1/ctrl_rsp_to_ctrl] [get_bd_intf_pins axis_switch_1/S01_AXIS]
  connect_bd_intf_net -intf_net OperatorController_1_m_axis_inside [get_bd_intf_pins OperatorController_1/m_axis_inside] [get_bd_intf_pins encrypt_0/stream_in]
  connect_bd_intf_net -intf_net OperatorController_1_m_axis_outside [get_bd_intf_pins OperatorController_1/m_axis_outside] [get_bd_intf_pins axis_switch_4/S02_AXIS]
  connect_bd_intf_net -intf_net OperatorController_2_ctrl_rsp_to_ctrl [get_bd_intf_pins OperatorController_2/ctrl_rsp_to_ctrl] [get_bd_intf_pins axis_switch_1/S02_AXIS]
  connect_bd_intf_net -intf_net OperatorController_2_m_axis_inside [get_bd_intf_pins OperatorController_2/m_axis_inside] [get_bd_intf_pins lwe_encrypt_0/data_in]
  connect_bd_intf_net -intf_net OperatorController_2_m_axis_outside [get_bd_intf_pins OperatorController_2/m_axis_outside] [get_bd_intf_pins axis_switch_4/S03_AXIS]
  connect_bd_intf_net -intf_net OperatorController_3_ctrl_rsp_to_ctrl [get_bd_intf_pins OperatorController_3/ctrl_rsp_to_ctrl] [get_bd_intf_pins axis_switch_1/S03_AXIS]
  connect_bd_intf_net -intf_net OperatorController_3_m_axis_inside [get_bd_intf_pins OperatorController_3/m_axis_inside] [get_bd_intf_pins lwe_decrypt_0/data_in]
  connect_bd_intf_net -intf_net OperatorController_3_m_axis_outside [get_bd_intf_pins OperatorController_3/m_axis_outside] [get_bd_intf_pins axis_switch_4/S04_AXIS]
  connect_bd_intf_net -intf_net S00_AXI_0_1 [get_bd_intf_ports s_axi_Manager] [get_bd_intf_pins axi_interconnect_1/S00_AXI]
  connect_bd_intf_net -intf_net accExamplePlusOperat_0_data_out [get_bd_intf_pins OperatorController_0/s_axis_inside] [get_bd_intf_pins accExamplePlusOperat_0/data_out]
  connect_bd_intf_net -intf_net accExamplePlusOperat_0_operator_done_signal [get_bd_intf_pins OperatorController_0/done_stream] [get_bd_intf_pins accExamplePlusOperat_0/operator_done_signal]
  connect_bd_intf_net -intf_net axi_dwidth_converter_0_M_AXI [get_bd_intf_ports axi_mem_access] [get_bd_intf_pins axi_dwidth_converter_0/M_AXI]
  connect_bd_intf_net -intf_net axi_interconnect_1_M00_AXI [get_bd_intf_pins axi_gpio_0/S_AXI] [get_bd_intf_pins axi_interconnect_1/M00_AXI]
  connect_bd_intf_net -intf_net axi_interconnect_1_M01_AXI [get_bd_intf_pins axi_bram_ctrl_1/S_AXI] [get_bd_intf_pins axi_interconnect_1/M01_AXI]
  connect_bd_intf_net -intf_net axi_interconnect_1_M02_AXI [get_bd_intf_pins axi_bram_ctrl_0/S_AXI] [get_bd_intf_pins axi_interconnect_1/M02_AXI]
  connect_bd_intf_net -intf_net axi_interconnect_1_M03_AXI [get_bd_intf_pins AssScheduler_0/s_axi_Manager] [get_bd_intf_pins axi_interconnect_1/M03_AXI]
  connect_bd_intf_net -intf_net axis_interconnect_1_M00_AXIS [get_bd_intf_pins OperatorController_0/s_axis_outside] [get_bd_intf_pins axis_switch_4/M00_AXIS]
  connect_bd_intf_net -intf_net axis_switch_0_M00_AXIS [get_bd_intf_pins OperatorController_0/ctrl_req_from_ctrl] [get_bd_intf_pins axis_switch_0/M00_AXIS]
  connect_bd_intf_net -intf_net axis_switch_0_M01_AXIS [get_bd_intf_pins OperatorController_1/ctrl_req_from_ctrl] [get_bd_intf_pins axis_switch_0/M01_AXIS]
  connect_bd_intf_net -intf_net axis_switch_0_M02_AXIS [get_bd_intf_pins OperatorController_2/ctrl_req_from_ctrl] [get_bd_intf_pins axis_switch_0/M02_AXIS]
  connect_bd_intf_net -intf_net axis_switch_0_M03_AXIS [get_bd_intf_pins OperatorController_3/ctrl_req_from_ctrl] [get_bd_intf_pins axis_switch_0/M03_AXIS]
  connect_bd_intf_net -intf_net axis_switch_1_M00_AXIS [get_bd_intf_pins CtrlRspReceiver_0/ctrl_rsp_from_acc_in] [get_bd_intf_pins axis_switch_1/M00_AXIS]
  connect_bd_intf_net -intf_net axis_switch_2_M00_AXIS [get_bd_intf_pins OperatorController_0/recovery_context_from_ctrl] [get_bd_intf_pins axis_switch_2/M00_AXIS]
  connect_bd_intf_net -intf_net axis_switch_2_M01_AXIS [get_bd_intf_pins OperatorController_1/recovery_context_from_ctrl] [get_bd_intf_pins axis_switch_2/M01_AXIS]
  connect_bd_intf_net -intf_net axis_switch_2_M02_AXIS [get_bd_intf_pins OperatorController_2/recovery_context_from_ctrl] [get_bd_intf_pins axis_switch_2/M02_AXIS]
  connect_bd_intf_net -intf_net axis_switch_2_M03_AXIS [get_bd_intf_pins OperatorController_3/recovery_context_from_ctrl] [get_bd_intf_pins axis_switch_2/M03_AXIS]
  connect_bd_intf_net -intf_net axis_switch_4_M01_AXIS [get_bd_intf_ports m_ext_axis] [get_bd_intf_pins axis_switch_4/M01_AXIS]
  connect_bd_intf_net -intf_net axis_switch_4_M02_AXIS [get_bd_intf_pins OperatorController_1/s_axis_outside] [get_bd_intf_pins axis_switch_4/M02_AXIS]
  connect_bd_intf_net -intf_net axis_switch_4_M03_AXIS [get_bd_intf_pins OperatorController_2/s_axis_outside] [get_bd_intf_pins axis_switch_4/M03_AXIS]
  connect_bd_intf_net -intf_net axis_switch_4_M04_AXIS [get_bd_intf_pins OperatorController_3/s_axis_outside] [get_bd_intf_pins axis_switch_4/M04_AXIS]
  connect_bd_intf_net -intf_net encrypt_0_stream_out [get_bd_intf_pins OperatorController_1/s_axis_inside] [get_bd_intf_pins encrypt_0/stream_out]
  connect_bd_intf_net -intf_net lwe_encrypt_0_data_out [get_bd_intf_pins OperatorController_2/s_axis_inside] [get_bd_intf_pins lwe_encrypt_0/data_out]
  connect_bd_intf_net -intf_net lwe_decrypt_0_data_out [get_bd_intf_pins OperatorController_3/s_axis_inside] [get_bd_intf_pins lwe_decrypt_0/data_out]
  connect_bd_intf_net -intf_net host_write_mem_ctrl_m_axis_cmd_channel [get_bd_intf_ports prp_out] [get_bd_intf_pins host_write_mem_ctrl/m_axis_cmd_channel]
  connect_bd_intf_net -intf_net host_write_mem_ctrl_s_axis_data_channel [get_bd_intf_ports c2h_data] [get_bd_intf_pins host_write_mem_ctrl/s_axis_data_channel]
  connect_bd_intf_net -intf_net read_mem_data_1 [get_bd_intf_ports read_mem_data] [get_bd_intf_pins AssScheduler_0/acc_context_data_recv_from_datamover]
  connect_bd_intf_net -intf_net s_ext_axis_1 [get_bd_intf_ports s_ext_axis] [get_bd_intf_pins axis_switch_4/S01_AXIS]

  # Create port connections
  connect_bd_net -net AssScheduler_0_cq_address0 [get_bd_pins AssScheduler_0/cq_address0] [get_bd_pins cq/addra]
  connect_bd_net -net AssScheduler_0_cq_ce0 [get_bd_pins AssScheduler_0/cq_ce0] [get_bd_pins cq/ena]
  connect_bd_net -net AssScheduler_0_cq_d0 [get_bd_pins AssScheduler_0/cq_d0] [get_bd_pins cq/dina]
  connect_bd_net -net AssScheduler_0_cq_we0 [get_bd_pins AssScheduler_0/cq_we0] [get_bd_pins cq/wea]
  connect_bd_net -net AssScheduler_0_sq_address0 [get_bd_pins AssScheduler_0/sq_address0] [get_bd_pins xlconcat_1/In1]
  connect_bd_net -net AssScheduler_0_sq_ce0 [get_bd_pins AssScheduler_0/sq_ce0] [get_bd_pins sq/enb]
  connect_bd_net -net OperatorController_0_ap_rst_n [get_bd_pins OperatorController_0/ap_rst_n] [get_bd_pins accExamplePlusOperat_0/ap_rst_n]
  connect_bd_net -net OperatorController_0_ap_start [get_bd_pins OperatorController_0/ap_start] [get_bd_pins accExamplePlusOperat_0/ap_start]
  connect_bd_net -net OperatorController_0_bram_address_out [get_bd_pins OperatorController_0/bram_address_out] [get_bd_pins static_var_bram/addra]
  connect_bd_net -net OperatorController_0_bram_ce_out [get_bd_pins OperatorController_0/bram_ce_out] [get_bd_pins static_var_bram/ena]
  connect_bd_net -net OperatorController_0_bram_d_out [get_bd_pins OperatorController_0/bram_d_out] [get_bd_pins static_var_bram/dina]
  connect_bd_net -net OperatorController_0_bram_q_in [get_bd_pins OperatorController_0/bram_q_in] [get_bd_pins accExamplePlusOperat_0/context_r_Dout_A]
  connect_bd_net -net OperatorController_0_bram_we_out [get_bd_pins OperatorController_0/bram_we_out] [get_bd_pins static_var_bram/wea]
  connect_bd_net -net OperatorController_1_ap_rst_n [get_bd_pins OperatorController_1/ap_rst_n] [get_bd_pins encrypt_0/ap_rst_n]
  connect_bd_net -net OperatorController_1_ap_start [get_bd_pins OperatorController_1/ap_start] [get_bd_pins encrypt_0/ap_start]
  connect_bd_net -net OperatorController_1_bram_address_out [get_bd_pins OperatorController_1/bram_address_out] [get_bd_pins static_var_bram1/addra]
  connect_bd_net -net OperatorController_1_bram_ce_out [get_bd_pins OperatorController_1/bram_ce_out] [get_bd_pins static_var_bram1/ena]
  connect_bd_net -net OperatorController_1_bram_d_out [get_bd_pins OperatorController_1/bram_d_out] [get_bd_pins static_var_bram1/dina]
  connect_bd_net -net OperatorController_1_bram_we_out [get_bd_pins OperatorController_1/bram_we_out] [get_bd_pins static_var_bram1/wea]
  connect_bd_net -net OperatorController_2_ap_rst_n [get_bd_pins OperatorController_2/ap_rst_n] [get_bd_pins lwe_encrypt_0/ap_rst_n]
  connect_bd_net -net OperatorController_2_ap_start [get_bd_pins OperatorController_2/ap_start] [get_bd_pins lwe_encrypt_0/ap_start]
  connect_bd_net -net OperatorController_2_bram_address_out [get_bd_pins OperatorController_2/bram_address_out] [get_bd_pins static_var_bram2/addra]
  connect_bd_net -net OperatorController_2_bram_ce_out [get_bd_pins OperatorController_2/bram_ce_out] [get_bd_pins static_var_bram2/ena]
  connect_bd_net -net OperatorController_2_bram_d_out [get_bd_pins OperatorController_2/bram_d_out] [get_bd_pins static_var_bram2/dina]
  connect_bd_net -net OperatorController_2_bram_we_out [get_bd_pins OperatorController_2/bram_we_out] [get_bd_pins static_var_bram2/wea]
  connect_bd_net -net OperatorController_3_ap_rst_n [get_bd_pins OperatorController_3/ap_rst_n] [get_bd_pins lwe_decrypt_0/ap_rst_n]
  connect_bd_net -net OperatorController_3_ap_start [get_bd_pins OperatorController_3/ap_start] [get_bd_pins lwe_decrypt_0/ap_start]
  connect_bd_net -net OperatorController_3_bram_address_out [get_bd_pins OperatorController_3/bram_address_out] [get_bd_pins static_var_bram3/addra]
  connect_bd_net -net OperatorController_3_bram_ce_out [get_bd_pins OperatorController_3/bram_ce_out] [get_bd_pins static_var_bram3/ena]
  connect_bd_net -net OperatorController_3_bram_d_out [get_bd_pins OperatorController_3/bram_d_out] [get_bd_pins static_var_bram3/dina]
  connect_bd_net -net OperatorController_3_bram_we_out [get_bd_pins OperatorController_3/bram_we_out] [get_bd_pins static_var_bram3/wea]
  connect_bd_net -net accExamplePlusOperat_0_context_r_Addr_A [get_bd_pins OperatorController_0/bram_address_in] [get_bd_pins accExamplePlusOperat_0/context_r_Addr_A]
  connect_bd_net -net accExamplePlusOperat_0_context_r_Din_A [get_bd_pins OperatorController_0/bram_d_in] [get_bd_pins accExamplePlusOperat_0/context_r_Din_A]
  connect_bd_net -net accExamplePlusOperat_0_context_r_EN_A [get_bd_pins OperatorController_0/bram_ce_in] [get_bd_pins accExamplePlusOperat_0/context_r_EN_A]
  connect_bd_net -net accExamplePlusOperat_0_context_r_WEN_A [get_bd_pins OperatorController_0/bram_we_in] [get_bd_pins accExamplePlusOperat_0/context_r_WEN_A]
  connect_bd_net -net lwe_encrypt_0_context_Addr_A [get_bd_pins OperatorController_2/bram_address_in] [get_bd_pins lwe_encrypt_0/context_Addr_A]
  connect_bd_net -net lwe_encrypt_0_context_Din_A [get_bd_pins OperatorController_2/bram_d_in] [get_bd_pins lwe_encrypt_0/context_Din_A]
  connect_bd_net -net lwe_encrypt_0_context_Dout_A [get_bd_pins OperatorController_2/bram_q_in] [get_bd_pins lwe_encrypt_0/context_Dout_A]
  connect_bd_net -net lwe_encrypt_0_context_EN_A [get_bd_pins OperatorController_2/bram_ce_in] [get_bd_pins lwe_encrypt_0/context_EN_A]
  connect_bd_net -net lwe_encrypt_0_context_WEN_A [get_bd_pins OperatorController_2/bram_we_in] [get_bd_pins lwe_encrypt_0/context_WEN_A]
  connect_bd_net -net lwe_decrypt_0_context_Addr_A [get_bd_pins OperatorController_3/bram_address_in] [get_bd_pins lwe_decrypt_0/context_Addr_A]
  connect_bd_net -net lwe_decrypt_0_context_Din_A [get_bd_pins OperatorController_3/bram_d_in] [get_bd_pins lwe_decrypt_0/context_Din_A]
  connect_bd_net -net lwe_decrypt_0_context_Dout_A [get_bd_pins OperatorController_3/bram_q_in] [get_bd_pins lwe_decrypt_0/context_Dout_A]
  connect_bd_net -net lwe_decrypt_0_context_EN_A [get_bd_pins OperatorController_3/bram_ce_in] [get_bd_pins lwe_decrypt_0/context_EN_A]
  connect_bd_net -net lwe_decrypt_0_context_WEN_A [get_bd_pins OperatorController_3/bram_we_in] [get_bd_pins lwe_decrypt_0/context_WEN_A]
  connect_bd_net -net axi_bram_ctrl_0_bram_addr_a [get_bd_pins axi_bram_ctrl_0/bram_addr_a] [get_bd_pins sq/addra]
  connect_bd_net -net axi_bram_ctrl_0_bram_clk_a [get_bd_pins axi_bram_ctrl_0/bram_clk_a] [get_bd_pins sq/clka]
  connect_bd_net -net axi_bram_ctrl_0_bram_en_a [get_bd_pins axi_bram_ctrl_0/bram_en_a] [get_bd_pins sq/ena]
  connect_bd_net -net axi_bram_ctrl_0_bram_we_a [get_bd_pins axi_bram_ctrl_0/bram_we_a] [get_bd_pins sq/wea]
  connect_bd_net -net axi_bram_ctrl_0_bram_wrdata_a [get_bd_pins axi_bram_ctrl_0/bram_wrdata_a] [get_bd_pins sq/dina]
  connect_bd_net -net axi_bram_ctrl_1_bram_addr_a [get_bd_pins axi_bram_ctrl_1/bram_addr_a] [get_bd_pins xlslice_1/Din]
  connect_bd_net -net axi_bram_ctrl_1_bram_clk_a [get_bd_pins axi_bram_ctrl_1/bram_clk_a] [get_bd_pins cq/clka] [get_bd_pins cq/clkb]
  connect_bd_net -net axi_bram_ctrl_1_bram_en_a [get_bd_pins axi_bram_ctrl_1/bram_en_a] [get_bd_pins cq/enb]
  connect_bd_net -net axi_gpio_0_gpio2_io_o [get_bd_pins axi_gpio_0/gpio2_io_o] [get_bd_pins util_vector_logic_0/Op1]
  connect_bd_net -net axis_switch_0_s_decode_err [get_bd_pins axis_switch_0/s_decode_err] [get_bd_pins xlconcat_0/In3]
  connect_bd_net -net axis_switch_1_s_decode_err [get_bd_pins axis_switch_1/s_decode_err] [get_bd_pins xlconcat_0/In1]
  connect_bd_net -net axis_switch_2_s_decode_err [get_bd_pins axis_switch_2/s_decode_err] [get_bd_pins xlconcat_0/In0]
  connect_bd_net -net axis_switch_4_s_decode_err [get_bd_pins axis_switch_4/s_decode_err] [get_bd_pins xlconcat_0/In2]
  connect_bd_net -net clk [get_bd_ports clk] [get_bd_pins AssScheduler_0/ap_clk] [get_bd_pins CtrlRspReceiver_0/ap_clk] [get_bd_pins OperatorController_0/clk] [get_bd_pins OperatorController_1/clk] [get_bd_pins OperatorController_2/clk] [get_bd_pins OperatorController_3/clk] [get_bd_pins accExamplePlusOperat_0/ap_clk] [get_bd_pins axi_bram_ctrl_0/s_axi_aclk] [get_bd_pins axi_bram_ctrl_1/s_axi_aclk] [get_bd_pins axi_dwidth_converter_0/s_axi_aclk] [get_bd_pins axi_gpio_0/s_axi_aclk] [get_bd_pins axi_interconnect_1/ACLK] [get_bd_pins axi_interconnect_1/M00_ACLK] [get_bd_pins axi_interconnect_1/M01_ACLK] [get_bd_pins axi_interconnect_1/M02_ACLK] [get_bd_pins axi_interconnect_1/M03_ACLK] [get_bd_pins axi_interconnect_1/S00_ACLK] [get_bd_pins axis_switch_0/aclk] [get_bd_pins axis_switch_1/aclk] [get_bd_pins axis_switch_2/aclk] [get_bd_pins axis_switch_4/aclk] [get_bd_pins encrypt_0/ap_clk] [get_bd_pins host_write_mem_ctrl/clk] [get_bd_pins lwe_encrypt_0/ap_clk] [get_bd_pins lwe_decrypt_0/ap_clk] [get_bd_pins sq/clkb] [get_bd_pins static_var_bram/clka] [get_bd_pins static_var_bram1/clka] [get_bd_pins static_var_bram2/clka] [get_bd_pins static_var_bram3/clka]
  connect_bd_net -net cq_doutb [get_bd_pins axi_bram_ctrl_1/bram_rddata_a] [get_bd_pins cq/doutb]
  connect_bd_net -net encrypt_0_ap_done [get_bd_pins OperatorController_1/ap_done] [get_bd_pins encrypt_0/ap_done]
  connect_bd_net -net encrypt_0_ap_idle [get_bd_pins OperatorController_1/ap_idle] [get_bd_pins encrypt_0/ap_idle]
  connect_bd_net -net encrypt_0_ap_ready [get_bd_pins OperatorController_1/ap_ready] [get_bd_pins encrypt_0/ap_ready]
  connect_bd_net -net encrypt_1_ap_done [get_bd_pins OperatorController_0/ap_done] [get_bd_pins accExamplePlusOperat_0/ap_done]
  connect_bd_net -net encrypt_1_ap_idle [get_bd_pins OperatorController_0/ap_idle] [get_bd_pins accExamplePlusOperat_0/ap_idle]
  connect_bd_net -net encrypt_1_ap_ready [get_bd_pins OperatorController_0/ap_ready] [get_bd_pins accExamplePlusOperat_0/ap_ready]
  connect_bd_net -net lwe_encrypt_0_ap_done [get_bd_pins OperatorController_2/ap_done] [get_bd_pins lwe_encrypt_0/ap_done]
  connect_bd_net -net lwe_encrypt_0_ap_idle [get_bd_pins OperatorController_2/ap_idle] [get_bd_pins lwe_encrypt_0/ap_idle]
  connect_bd_net -net lwe_encrypt_0_ap_ready [get_bd_pins OperatorController_2/ap_ready] [get_bd_pins lwe_encrypt_0/ap_ready]
  connect_bd_net -net lwe_decrypt_0_ap_done [get_bd_pins OperatorController_3/ap_done] [get_bd_pins lwe_decrypt_0/ap_done]
  connect_bd_net -net lwe_decrypt_0_ap_idle [get_bd_pins OperatorController_3/ap_idle] [get_bd_pins lwe_decrypt_0/ap_idle]
  connect_bd_net -net lwe_decrypt_0_ap_ready [get_bd_pins OperatorController_3/ap_ready] [get_bd_pins lwe_decrypt_0/ap_ready]
  connect_bd_net -net resetn_1 [get_bd_ports resetn] [get_bd_pins CtrlRspReceiver_0/ap_rst_n] [get_bd_pins axi_bram_ctrl_0/s_axi_aresetn] [get_bd_pins axi_bram_ctrl_1/s_axi_aresetn] [get_bd_pins axi_dwidth_converter_0/s_axi_aresetn] [get_bd_pins axi_gpio_0/s_axi_aresetn] [get_bd_pins axi_interconnect_1/ARESETN] [get_bd_pins axi_interconnect_1/M00_ARESETN] [get_bd_pins axi_interconnect_1/M01_ARESETN] [get_bd_pins axi_interconnect_1/M02_ARESETN] [get_bd_pins axi_interconnect_1/M03_ARESETN] [get_bd_pins axi_interconnect_1/S00_ARESETN] [get_bd_pins axis_switch_2/aresetn] [get_bd_pins host_write_mem_ctrl/user_resetn] [get_bd_pins util_vector_logic_0/Op2]
  connect_bd_net -net sq_doutb [get_bd_pins AssScheduler_0/sq_q0] [get_bd_pins axi_bram_ctrl_0/bram_rddata_a] [get_bd_pins sq/doutb]
  connect_bd_net -net static_var_bram1_douta [get_bd_pins OperatorController_1/bram_q_out] [get_bd_pins static_var_bram1/douta]
  connect_bd_net -net static_var_bram2_douta [get_bd_pins OperatorController_2/bram_q_out] [get_bd_pins static_var_bram2/douta]
  connect_bd_net -net static_var_bram3_douta [get_bd_pins OperatorController_3/bram_q_out] [get_bd_pins static_var_bram3/douta]
  connect_bd_net -net static_var_bram_douta [get_bd_pins OperatorController_0/bram_q_out] [get_bd_pins static_var_bram/douta]
  connect_bd_net -net util_vector_logic_0_Res [get_bd_pins AssScheduler_0/ap_rst_n] [get_bd_pins OperatorController_0/resetn] [get_bd_pins OperatorController_1/resetn] [get_bd_pins OperatorController_2/resetn] [get_bd_pins OperatorController_3/resetn] [get_bd_pins axis_switch_0/aresetn] [get_bd_pins axis_switch_1/aresetn] [get_bd_pins axis_switch_4/aresetn] [get_bd_pins util_vector_logic_0/Res]
  connect_bd_net -net xlconcat_0_dout [get_bd_pins axi_gpio_0/gpio_io_i] [get_bd_pins xlconcat_0/dout]
  connect_bd_net -net xlconcat_1_dout [get_bd_pins sq/addrb] [get_bd_pins xlconcat_1/dout]
  connect_bd_net -net xlconstant_0_dout [get_bd_pins AssScheduler_0/ap_start] [get_bd_pins CtrlRspReceiver_0/ap_start] [get_bd_pins xlconstant_0/dout]
  connect_bd_net -net xlconstant_1_dout [get_bd_pins OperatorController_0/op_id] [get_bd_pins xlconstant_1/dout]
  connect_bd_net -net xlconstant_2_dout [get_bd_pins host_write_mem_ctrl/m_axis_data_channel1_tvalid] [get_bd_pins xlconstant_2/dout]
  connect_bd_net -net xlconstant_3_dout [get_bd_pins OperatorController_1/op_id] [get_bd_pins xlconstant_3/dout]
  connect_bd_net -net xlconstant_4_dout [get_bd_pins OperatorController_1/bram_address_in] [get_bd_pins OperatorController_1/bram_ce_in] [get_bd_pins OperatorController_1/bram_d_in] [get_bd_pins OperatorController_1/bram_we_in] [get_bd_pins OperatorController_1/done_stream_tvalid] [get_bd_pins OperatorController_2/done_stream_tvalid] [get_bd_pins OperatorController_3/done_stream_tvalid] [get_bd_pins xlconstant_4/dout]
  connect_bd_net -net xlconstant_5_dout [get_bd_pins xlconcat_1/In0] [get_bd_pins xlconstant_5/dout]
  connect_bd_net -net xlconstant_6_dout [get_bd_pins OperatorController_2/op_id] [get_bd_pins xlconstant_6/dout]
  connect_bd_net -net xlconstant_7_dout [get_bd_pins OperatorController_3/op_id] [get_bd_pins xlconstant_7/dout]
  connect_bd_net -net xlslice_1_Dout [get_bd_pins cq/addrb] [get_bd_pins xlslice_1/Dout]

  # Create address segments
  assign_bd_address -offset 0x00004000 -range 0x00001000 -target_address_space [get_bd_addr_spaces s_axi_Manager] [get_bd_addr_segs AssScheduler_0/s_axi_Manager/reg0] -force
  assign_bd_address -offset 0x00002000 -range 0x00002000 -target_address_space [get_bd_addr_spaces s_axi_Manager] [get_bd_addr_segs axi_bram_ctrl_0/S_AXI/Mem0] -force
  assign_bd_address -offset 0x00001000 -range 0x00001000 -target_address_space [get_bd_addr_spaces s_axi_Manager] [get_bd_addr_segs axi_bram_ctrl_1/S_AXI/Mem0] -force
  assign_bd_address -offset 0x00000080 -range 0x00000080 -target_address_space [get_bd_addr_spaces s_axi_Manager] [get_bd_addr_segs axi_gpio_0/S_AXI/Reg] -force


  # Restore current instance
  current_bd_instance $oldCurInst

  save_bd_design
}
# End of create_root_design()


##################################################################
# MAIN FLOW
##################################################################

create_root_design ""


common::send_gid_msg -ssname BD::TCL -id 2053 -severity "WARNING" "This Tcl script was generated from a block design that has not been validated. It is possible that design <$design_name> may result in errors during validation."
