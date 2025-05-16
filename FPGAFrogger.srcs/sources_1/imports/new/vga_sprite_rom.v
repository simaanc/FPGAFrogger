`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 05/07/2025 02:57:30 AM
// Design Name: 
// Module Name: vga_sprite_rom
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////

module vga_sprite_rom #(
  parameter BASE_ADDR = 0  // 0 for sprite0, 16 for sprite1
)(
  input  wire        pix_clk,   // 25 MHz
  input  wire        pix_rst,   // sync reset
  input  wire        active,    // o_active
  input  wire [9:0]  xpos, ypos, // o_x, o_y
  input  wire [9:0]  spr_x, spr_y, // sprite XY
  output wire        pixel_on   // 1 = draw
);

  localparam W = 16, H = 16;

  // single 32-word ROM (two 16×16 sprites)
  (* ram_style = "block" *) reg [W-1:0] rom [0:2*H-1];
  initial begin
    $readmemb("sprite.mem", rom);
  end

  // row counter & shift register
  reg [3:0] row;
  reg [W-1:0] shift_reg;

  // compute when/which ROM word to fetch
  wire rom_ena  = active && (ypos >= spr_y && ypos < spr_y + H);
  wire [4:0] rom_addr = BASE_ADDR + row;  // BASE_ADDR=0 or 16

  always @(posedge pix_clk or posedge pix_rst) begin
    if (pix_rst) begin
      row       <= 0;
      shift_reg <= 0;
    end else if (active) begin
      if (ypos == spr_y && xpos == spr_x) begin
        row       <= 0;
        shift_reg <= rom[rom_addr];
      end else if (ypos >= spr_y && ypos < spr_y + H) begin
        if (xpos == spr_x) begin
          row       <= row + 1;
          shift_reg <= rom[rom_addr];
        end else if (xpos > spr_x && xpos < spr_x + W) begin
          shift_reg <= { shift_reg[W-2:0], 1'b0 };
        end
      end
    end
  end

  assign pixel_on = active
    && xpos>=spr_x && xpos<spr_x+W
    && ypos>=spr_y && ypos<spr_y+H
    && shift_reg[W-1];

endmodule
