// Copyright 1986-2018 Xilinx, Inc. All Rights Reserved.
// --------------------------------------------------------------------------------
// Tool Version: Vivado v.2018.2 (win64) Build 2258646 Thu Jun 14 20:03:12 MDT 2018
// Date        : Thu May 22 15:57:00 2025
// Host        : Christopher-Desktop running 64-bit major release  (build 9200)
// Command     : write_verilog -force -mode synth_stub
//               C:/Vivado/FPGAFrogger/FPGAFrogger.srcs/sources_1/bd/design_1/ip/design_1_vga_framebuffer_top_0_0/design_1_vga_framebuffer_top_0_0_stub.v
// Design      : design_1_vga_framebuffer_top_0_0
// Purpose     : Stub declaration of top-level module interface
// Device      : xc7a35tcpg236-1
// --------------------------------------------------------------------------------

// This empty module with port declaration file causes synthesis tools to infer a black box for IP.
// The synthesis directives are for Synopsys Synplify support to prevent IO buffer insertion.
// Please paste the declaration into a Verilog source file or add the file as an additional source.
(* X_CORE_INFO = "vga_framebuffer_top,Vivado 2018.2" *)
module design_1_vga_framebuffer_top_0_0(clk_pix, rst_pix, cpu_we, cpu_addr, cpu_dat, 
  VGA_Hsync, VGA_Vsync, VGA_Red, VGA_Green, VGA_Blue, frame_ready)
/* synthesis syn_black_box black_box_pad_pin="clk_pix,rst_pix,cpu_we,cpu_addr[15:0],cpu_dat[3:0],VGA_Hsync,VGA_Vsync,VGA_Red[3:0],VGA_Green[3:0],VGA_Blue[3:0],frame_ready" */;
  input clk_pix;
  input rst_pix;
  input cpu_we;
  input [15:0]cpu_addr;
  input [3:0]cpu_dat;
  output VGA_Hsync;
  output VGA_Vsync;
  output [3:0]VGA_Red;
  output [3:0]VGA_Green;
  output [3:0]VGA_Blue;
  output frame_ready;
endmodule
