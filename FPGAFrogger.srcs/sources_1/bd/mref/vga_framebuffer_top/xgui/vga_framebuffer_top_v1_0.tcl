# Definitional proc to organize widgets for parameters.
proc init_gui { IPINST } {
  ipgui::add_param $IPINST -name "Component_Name"
  #Adding Page
  set Page_0 [ipgui::add_page $IPINST -name "Page 0"]
  ipgui::add_param $IPINST -name "BPP" -parent ${Page_0}
  ipgui::add_param $IPINST -name "FB_HEIGHT" -parent ${Page_0}
  ipgui::add_param $IPINST -name "FB_WIDTH" -parent ${Page_0}
  ipgui::add_param $IPINST -name "INIT_FILE" -parent ${Page_0}


}

proc update_PARAM_VALUE.BPP { PARAM_VALUE.BPP } {
	# Procedure called to update BPP when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BPP { PARAM_VALUE.BPP } {
	# Procedure called to validate BPP
	return true
}

proc update_PARAM_VALUE.FB_HEIGHT { PARAM_VALUE.FB_HEIGHT } {
	# Procedure called to update FB_HEIGHT when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.FB_HEIGHT { PARAM_VALUE.FB_HEIGHT } {
	# Procedure called to validate FB_HEIGHT
	return true
}

proc update_PARAM_VALUE.FB_WIDTH { PARAM_VALUE.FB_WIDTH } {
	# Procedure called to update FB_WIDTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.FB_WIDTH { PARAM_VALUE.FB_WIDTH } {
	# Procedure called to validate FB_WIDTH
	return true
}

proc update_PARAM_VALUE.INIT_FILE { PARAM_VALUE.INIT_FILE } {
	# Procedure called to update INIT_FILE when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.INIT_FILE { PARAM_VALUE.INIT_FILE } {
	# Procedure called to validate INIT_FILE
	return true
}


proc update_MODELPARAM_VALUE.FB_WIDTH { MODELPARAM_VALUE.FB_WIDTH PARAM_VALUE.FB_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.FB_WIDTH}] ${MODELPARAM_VALUE.FB_WIDTH}
}

proc update_MODELPARAM_VALUE.FB_HEIGHT { MODELPARAM_VALUE.FB_HEIGHT PARAM_VALUE.FB_HEIGHT } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.FB_HEIGHT}] ${MODELPARAM_VALUE.FB_HEIGHT}
}

proc update_MODELPARAM_VALUE.BPP { MODELPARAM_VALUE.BPP PARAM_VALUE.BPP } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BPP}] ${MODELPARAM_VALUE.BPP}
}

proc update_MODELPARAM_VALUE.INIT_FILE { MODELPARAM_VALUE.INIT_FILE PARAM_VALUE.INIT_FILE } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.INIT_FILE}] ${MODELPARAM_VALUE.INIT_FILE}
}

