`timescale 1ns/1ps
module vga_framebuffer_top #(
  parameter FB_WIDTH   = 224,
  parameter FB_HEIGHT  = 256,
  parameter BPP        = 4,
  parameter INIT_FILE  = "background.mem"
)(
  input  wire             clk_pix,
  input  wire             rst_pix,
  input  wire             cpu_we,
  input  wire [15:0]      cpu_addr,
  input  wire [BPP-1:0]   cpu_dat,
  output wire             VGA_Hsync,
  output wire             VGA_Vsync,
  output wire [BPP-1:0]   VGA_Red,
  output wire [BPP-1:0]   VGA_Green,
  output wire [BPP-1:0]   VGA_Blue
);

  // center window
  localparam X0     = (640 - FB_WIDTH)/2;
  localparam Y0     = (480 - FB_HEIGHT)/2;
  localparam DEPTH0 = FB_WIDTH * FB_HEIGHT;
  localparam ADDRW  = $clog2(DEPTH0);

  //---- VGA timing ----
  wire        active;
  wire [9:0]  xpos;
  wire [8:0]  ypos;
  wire        screenend, animate;
  
  vga640x480 u_vga (
    .i_clk(clk_pix), .i_pix_stb(1'b1), .i_rst(rst_pix),
    .o_hs(VGA_Hsync), .o_vs(VGA_Vsync),
    .o_active(active), .o_x(xpos), .o_y(ypos),
    .o_blanking(), .o_screenend(screenend), .o_animate(animate)
  );

  //---- Fixed ping-pong buffer control ----
  reg fb_front, prev_animate;
  
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      fb_front     <= 1'b0;  // Start with buffer 0 as front
      prev_animate <= 1'b0;
    end else begin
      prev_animate <= animate;
      
      // Swap at end of active display (falling edge of animate)
      if (prev_animate && !animate) begin
        fb_front <= ~fb_front;
      end
    end
  end

  //---- Compute pixel index ----
  wire in_win = active
    && xpos >= X0 && xpos < X0 + FB_WIDTH
    && ypos >= Y0 && ypos < Y0 + FB_HEIGHT;
  wire [ADDRW-1:0] pix_idx = (ypos - Y0) * FB_WIDTH + (xpos - X0);

  //---- FIXED: CPU writes to BACK buffer, display reads FRONT buffer ----
  wire [BPP-1:0] dout0, dout1;

  bram_sdp #(
    .WIDTH  (BPP),
    .DEPTH  (DEPTH0),
    .INIT_F (INIT_FILE)
  ) bram0 (
    .clk_write  (clk_pix),
    .clk_read   (clk_pix),
    .we         (cpu_we && fb_front),        // FIXED: Write when bram0 is BACK buffer
    .addr_write (cpu_addr[ADDRW-1:0]),
    .data_in    (cpu_dat),
    .addr_read  (pix_idx),
    .data_out   (dout0)
  );

  bram_sdp #(
    .WIDTH  (BPP),
    .DEPTH  (DEPTH0),
    .INIT_F (INIT_FILE)
  ) bram1 (
    .clk_write  (clk_pix),
    .clk_read   (clk_pix),
    .we         (cpu_we && !fb_front),       // FIXED: Write when bram1 is BACK buffer
    .addr_write (cpu_addr[ADDRW-1:0]),
    .data_in    (cpu_dat),
    .addr_read  (pix_idx),
    .data_out   (dout1)
  );

  //---- Select front-buffer pixel for display ----
  wire [BPP-1:0] pix4 = fb_front ? dout1 : dout0;

  //---- Palette and VGA outputs ----
  reg [11:0] palette [0:15];
  initial begin
    palette[ 0]=12'h000; palette[ 1]=12'h004;
    palette[ 2]=12'h00F; palette[ 3]=12'h0DF;
    palette[ 4]=12'hFF0; palette[ 5]=12'h9F0;
    palette[ 6]=12'h2D0; palette[ 7]=12'hF0F;
    palette[ 8]=12'h90F; palette[ 9]=12'hF40;
    palette[10]=12'hF00; palette[11]=12'hD64;
    palette[12]=12'h964; palette[13]=12'hDDF;
    palette[14]=12'hFFF; palette[15]=12'h000;
  end
  wire [11:0] rgb = palette[pix4];
  assign VGA_Red   = in_win ? rgb[11:8] : 4'h0;
  assign VGA_Green = in_win ? rgb[ 7:4] : 4'h0;
  assign VGA_Blue  = in_win ? rgb[ 3:0] : 4'h0;

endmodule