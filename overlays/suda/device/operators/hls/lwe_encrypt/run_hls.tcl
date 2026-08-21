# run_hls.tcl - HLS script for project creation and RTL export.

if {[llength $argv] >= 4} {
    set target_name [lindex $argv 2]
    set mode [lindex $argv 3]
} else {
    set target_name [lindex $argv 0]
    set mode [lindex $argv 1]
}

set project_name ${target_name}
set solution_name "solution1"

catch {unset env(DEBUG)}
set vivado_root "/opt/Xilinx_2020.2/Vivado/2020.2"
set env(RDI_BINROOT) "${vivado_root}/bin"
set env(RDI_APPROOT) "${vivado_root}"
set env(RDI_BASEROOT) "/opt/Xilinx_2020.2/Vivado"
set env(RDI_DATADIR) "${vivado_root}/data"
set env(HDI_APPROOT) "${vivado_root}"
set vivado_lib_dir "/opt/Xilinx_2020.2/Vivado/2020.2/lib/lnx64.o"
if {[info exists env(LD_LIBRARY_PATH)]} {
    set env(LD_LIBRARY_PATH) "${vivado_lib_dir}:$env(LD_LIBRARY_PATH)"
} else {
    set env(LD_LIBRARY_PATH) "${vivado_lib_dir}"
}

set top_dir [file normalize [file join [file dirname [info script]] "../.."]]
set shared_include_dir [file join $top_dir "../shared_components/hls"]
set vitis_hls_include "/opt/Xilinx_2020.2/Vitis_HLS/2020.2/include"
set linux_arch_include [file normalize [file join [file dirname [info script]] "compat/x86_64_linux_gnu"]]
set compile_flags "-I${shared_include_dir} -I${vitis_hls_include} -I${linux_arch_include} -DUSING_XILINX_STREAM"

proc repack_ip_with_safe_revision {project_name solution_name target_name} {
    set ip_dir [file join [pwd] $project_name $solution_name impl ip]
    set pack_tcl [file join $ip_dir run_ippack.tcl]
    if {![file exists $pack_tcl]} {
        error "IP pack script not found: ${pack_tcl}"
    }

    set f [open $pack_tcl r]
    set content [read $f]
    close $f

    regsub {set Revision[ \t]+"[0-9]+"} $content {set Revision    "1"} content

    set f [open $pack_tcl w]
    puts -nonewline $f $content
    close $f

    set old_dir [pwd]
    cd $ip_dir
    set pack_status [catch {exec ./pack.sh 2>@1} pack_output]
    cd $old_dir
    puts $pack_output
    if {$pack_status != 0} {
        error "IP repack failed after forcing safe core revision"
    }

    set generated_zips [glob -nocomplain [file join $ip_dir *.zip]]
    if {[llength $generated_zips] == 0} {
        error "IP repack completed but no zip was generated in ${ip_dir}"
    }
    file copy -force [lindex $generated_zips 0] ./${target_name}_ip.zip
}

puts "Creating/opening project: ${project_name}"
open_project -reset ${project_name}

set source_file "${target_name}.cpp"
if {[file exists $source_file]} {
    puts "Adding design file: ${source_file}"
    add_files ${source_file} -cflags "${compile_flags}"
} else {
    puts "Error: Source file ${source_file} not found!"
    exit 1
}

set testbench_file "test.cpp"
if {[file exists $testbench_file]} {
    puts "Adding testbench file: ${testbench_file}"
    add_files -tb ${testbench_file} -cflags "${compile_flags}"
} else {
    puts "Warning: Testbench file ${testbench_file} not found. Continuing without testbench."
}

puts "Setting top function to: lwe_encrypt"
set_top lwe_encrypt

puts "Creating solution: ${solution_name}"
open_solution -reset ${solution_name}

set_part {xczu19eg-ffvc1760-2-e}
create_clock -period "250MHz"

config_interface -m_axi_max_bitwidth 512
config_rtl -reset all

if {$mode eq "prj_gen"} {
    puts "Mode: Project Generation - project only"
} elseif {$mode eq "csim"} {
    puts "Mode: C Simulation"
    csim_design
    puts "C simulation completed."
} elseif {$mode eq "csynth"} {
    puts "Mode: C Synthesis"
    csynth_design
    puts "C synthesis completed."
} elseif {$mode eq "cosim"} {
    puts "Mode: RTL Co-simulation"
    csynth_design
    cosim_design
    puts "RTL co-simulation completed."
} elseif {$mode eq "rtl_gen"} {
    puts "Mode: RTL Generation - Running synthesis and exporting design"
    csynth_design
    set export_status [catch {
        export_design -format ip_catalog -output ./${target_name}_ip.zip
    } export_output]
    if {$export_status != 0} {
        puts "export_design failed; retrying IP pack with a safe core revision."
        puts $export_output
        repack_ip_with_safe_revision $project_name $solution_name $target_name
    }
    puts "RTL generation completed. IP catalog exported to ${target_name}_ip.zip"
} else {
    puts "Error: Unknown mode '${mode}'. Valid modes are 'prj_gen', 'csim', 'csynth', 'cosim', or 'rtl_gen'."
    exit 1
}

puts "HLS processing for ${target_name} completed successfully."
exit
