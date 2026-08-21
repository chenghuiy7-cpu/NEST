# run_hls.tcl - HLS script for the native psi64 Big-LWE decrypt operator.

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
add_files ${target_name}.cpp -cflags "${compile_flags}"
add_files -tb test.cpp -cflags "${compile_flags}"
set_top lwe_decrypt

open_solution -reset ${solution_name}
set_part {xczu19eg-ffvc1760-2-e}
create_clock -period "250MHz"
config_interface -m_axi_max_bitwidth 512
config_rtl -reset all

if {$mode eq "prj_gen"} {
    puts "Mode: Project Generation - project only"
} elseif {$mode eq "csim"} {
    csim_design
} elseif {$mode eq "csynth"} {
    csynth_design
} elseif {$mode eq "cosim"} {
    csynth_design
    cosim_design
} elseif {$mode eq "rtl_gen"} {
    csynth_design
    set export_status [catch {
        export_design -format ip_catalog -output ./${target_name}_ip.zip
    } export_output]
    if {$export_status != 0} {
        puts "export_design failed; retrying IP pack with a safe core revision."
        puts $export_output
        repack_ip_with_safe_revision $project_name $solution_name $target_name
    }
} else {
    puts "Error: unknown mode '${mode}'"
    exit 1
}

puts "HLS processing for ${target_name} completed successfully."
exit
