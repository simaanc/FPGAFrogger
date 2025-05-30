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

  // center window constants
  localparam X0     = (640 - FB_WIDTH)/2;
  localparam Y0     = (480 - FB_HEIGHT)/2;
  localparam DEPTH0 = FB_WIDTH * FB_HEIGHT;
  localparam ADDRW  = $clog2(DEPTH0);

  //---- VGA timing core ----
  wire        active;
  wire [9:0]  xpos;
  wire [8:0]  ypos;
  vga640x480 u_vga (
    .i_clk     (clk_pix),
    .i_pix_stb (1'b1),
    .i_rst     (rst_pix),
    .o_hs      (VGA_Hsync),
    .o_vs      (VGA_Vsync),
    .o_active  (active),
    .o_x       (xpos),
    .o_y       (ypos),
    .o_blanking(), .o_screenend(), .o_animate()
  );

  //---- palette ROM ----
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

  //==================================================================
  // Stage 0: sample VGA timing
  //==================================================================
  reg         active_s0;
  reg [9:0]   xpos_s0;
  reg [8:0]   ypos_s0;
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      active_s0 <= 1'b0;
      xpos_s0   <= 10'd0;
      ypos_s0   <=  9'd0;
    end else begin
      active_s0 <= active;
      xpos_s0   <= xpos;
      ypos_s0   <= ypos;
    end
  end

  //==================================================================
  // Stage 1: compute window-in & X/Y offsets
  //==================================================================
  wire                   in_win_raw = active_s0
    && xpos_s0 >= X0 && xpos_s0 < X0 + FB_WIDTH
    && ypos_s0 >= Y0 && ypos_s0 < Y0 + FB_HEIGHT;
  wire [ADDRW-1:0]       dx_raw     = xpos_s0 - X0;
  wire [ADDRW-1:0]       dy_raw     = ypos_s0 - Y0;

  reg                    in_win_s1;
  reg [ADDRW-1:0]        dx_s1;
  reg [ADDRW-1:0]        dy_s1;
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      in_win_s1 <= 1'b0;
      dx_s1     <= {ADDRW{1'b0}};
      dy_s1     <= {ADDRW{1'b0}};
    end else begin
      in_win_s1 <= in_win_raw;
      dx_s1     <= dx_raw;
      dy_s1     <= dy_raw;
    end
  end

  //==================================================================
  // Stage 2: multiply+add ? pix_idx
  //==================================================================
  wire [ADDRW-1:0] pix_idx_raw = dy_s1 * FB_WIDTH + dx_s1;
  reg  [ADDRW-1:0] pix_idx_s2;
  reg              in_win_s2;
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      pix_idx_s2 <= {ADDRW{1'b0}};
      in_win_s2  <= 1'b0;
    end else begin
      pix_idx_s2 <= pix_idx_raw;
      in_win_s2  <= in_win_s1;
    end
  end

  //==================================================================
  // BRAM (single-port write, sync-read)
  //==================================================================
  wire [BPP-1:0] dout_raw;
  bram_sdp #(
    .WIDTH  (BPP),
    .DEPTH  (DEPTH0),
    .INIT_F (INIT_FILE)
  ) framebuffer (
    .clk_write  (clk_pix),
    .clk_read   (clk_pix),
    .we         (cpu_we),
    .addr_write (cpu_addr[ADDRW-1:0]),
    .data_in    (cpu_dat),
    .addr_read  (pix_idx_s2),
    .data_out   (dout_raw)
  );

  //==================================================================
  // Stage 3: capture BRAM output
  //==================================================================
  reg [BPP-1:0] dout_s3;
  reg           in_win_s3;
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      dout_s3   <= {BPP{1'b0}};
      in_win_s3 <= 1'b0;
    end else begin
      dout_s3   <= dout_raw;
      in_win_s3 <= in_win_s2;
    end
  end

  //==================================================================
  // Stage 4: palette lookup & final register
  //==================================================================
  wire [11:0] rgb_raw = palette[dout_s3];
  reg  [11:0] rgb_s4;
  reg         in_win_s4;
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      rgb_s4    <= 12'h000;
      in_win_s4 <= 1'b0;
    end else begin
      rgb_s4    <= rgb_raw;
      in_win_s4 <= in_win_s3;
    end
  end

  //==================================================================
  // Drive VGA outputs from Stage 4
  //==================================================================
  assign VGA_Red   = in_win_s4 ? rgb_s4[11:8] : 4'h0;
  assign VGA_Green = in_win_s4 ? rgb_s4[ 7:4] : 4'h0;
  assign VGA_Blue  = in_win_s4 ? rgb_s4[ 3:0] : 4'h0;

endmodule
