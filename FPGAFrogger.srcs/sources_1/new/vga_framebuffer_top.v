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
  output wire [BPP-1:0]   VGA_Blue,
  output wire             frame_ready
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

  //---- Frame detection ----
  reg prev_screenend, frame_pulse;
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      prev_screenend <= 1'b0;
      frame_pulse <= 1'b0;
    end else begin
      prev_screenend <= screenend;
      frame_pulse <= screenend && !prev_screenend;
    end
  end

  //---- State machine (same clock domain as CPU for simplicity) ----
  reg [2:0] state;
  localparam IDLE  = 3'd0;
  localparam INIT  = 3'd1;
  localparam CLEAR = 3'd2;
  localparam DRAW  = 3'd3;

  reg fb_front;
  reg [ADDRW-1:0] clear_addr;
  reg clearing;
  
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      state <= IDLE;
      fb_front <= 1'b0;
      clear_addr <= 0;
      clearing <= 1'b0;
    end else begin
      case (state)
        IDLE: begin
          if (frame_pulse) begin
            state <= INIT;
          end
        end
        INIT: begin
          state <= CLEAR;
          fb_front <= ~fb_front;
          clear_addr <= 0;
          clearing <= 1'b1;
        end
        CLEAR: begin
          clear_addr <= clear_addr + 1;
          if (clear_addr == DEPTH0-1) begin
            state <= DRAW;
            clearing <= 1'b0;
          end
        end
        DRAW: begin
          if (frame_pulse) begin
            state <= INIT;
          end
        end
      endcase
    end
  end

  //---- CPU write logic ----
  reg [ADDRW-1:0] fb_addr_write;
  reg [BPP-1:0] fb_colr_write;
  reg fb_we;
  
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      fb_addr_write <= 0;
      fb_colr_write <= 0;
      fb_we <= 1'b0;
    end else begin
      if (clearing) begin
        fb_addr_write <= clear_addr;
        fb_colr_write <= 4'h0;
        fb_we <= 1'b1;
      end else if (state == DRAW) begin
        fb_addr_write <= cpu_addr[ADDRW-1:0];
        fb_colr_write <= cpu_dat;
        fb_we <= cpu_we && (cpu_addr < DEPTH0);
      end else begin
        fb_we <= 1'b0;
      end
    end
  end

  //---- CRITICAL: Pipeline the display address calculation ----
  // Stage 1: Register inputs and check window
  reg active_r1;
  reg [9:0] xpos_r1;
  reg [8:0] ypos_r1;
  reg in_win_r1;
  
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      active_r1 <= 1'b0;
      xpos_r1 <= 0;
      ypos_r1 <= 0;
      in_win_r1 <= 1'b0;
    end else begin
      active_r1 <= active;
      xpos_r1 <= xpos;
      ypos_r1 <= ypos;
      in_win_r1 <= active && (xpos >= X0) && (xpos < X0 + FB_WIDTH) && 
                   (ypos >= Y0) && (ypos < Y0 + FB_HEIGHT);
    end
  end

  // Stage 2: Calculate offsets (this is fast)
  reg [9:0] x_offset_r2;
  reg [8:0] y_offset_r2;
  reg in_win_r2;
  
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      x_offset_r2 <= 0;
      y_offset_r2 <= 0;
      in_win_r2 <= 1'b0;
    end else begin
      x_offset_r2 <= xpos_r1 - X0;
      y_offset_r2 <= ypos_r1 - Y0;
      in_win_r2 <= in_win_r1;
    end
  end

  // Stage 3: Do the multiplication (this was the slow part!)
  reg [ADDRW-1:0] line_addr_r3;
  reg [9:0] x_offset_r3;
  reg in_win_r3;
  
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      line_addr_r3 <= 0;
      x_offset_r3 <= 0;
      in_win_r3 <= 1'b0;
    end else begin
      line_addr_r3 <= y_offset_r2 * FB_WIDTH;  // The slow multiplication, now pipelined
      x_offset_r3 <= x_offset_r2;
      in_win_r3 <= in_win_r2;
    end
  end

  // Stage 4: Add X offset (fast addition)
  reg [ADDRW-1:0] pix_addr_r4;
  reg in_win_r4;
  
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      pix_addr_r4 <= 0;
      in_win_r4 <= 1'b0;
    end else begin
      pix_addr_r4 <= line_addr_r3 + x_offset_r3;
      in_win_r4 <= in_win_r3;
    end
  end

  //---- Dual BRAMs (now using pipelined address) ----
  wire [BPP-1:0] dout0, dout1;

  bram_sdp #(
    .WIDTH  (BPP),
    .DEPTH  (DEPTH0),
    .INIT_F (INIT_FILE)
  ) bram0 (
    .clk_write  (clk_pix),
    .clk_read   (clk_pix),
    .we         (fb_we && fb_front),
    .addr_write (fb_addr_write),
    .data_in    (fb_colr_write),
    .addr_read  (pix_addr_r4),  // Using pipelined address
    .data_out   (dout0)
  );

  bram_sdp #(
    .WIDTH  (BPP),
    .DEPTH  (DEPTH0),
    .INIT_F (INIT_FILE)
  ) bram1 (
    .clk_write  (clk_pix),
    .clk_read   (clk_pix),
    .we         (fb_we && !fb_front),
    .addr_write (fb_addr_write),
    .data_in    (fb_colr_write),
    .addr_read  (pix_addr_r4),  // Using pipelined address
    .data_out   (dout1)
  );

  //---- Stage 5: Buffer selection and palette (pipeline this too) ----
  // Need to delay fb_front to match the 4-stage pipeline
  reg [3:0] fb_front_delay;
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      fb_front_delay <= 0;
    end else begin
      fb_front_delay <= {fb_front_delay[2:0], fb_front};
    end
  end

  reg [BPP-1:0] fb_colr_r5;
  reg in_win_r5;
  
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      fb_colr_r5 <= 0;
      in_win_r5 <= 1'b0;
    end else begin
      fb_colr_r5 <= fb_front_delay[3] ? dout1 : dout0;  // Use delayed fb_front
      in_win_r5 <= in_win_r4;
    end
  end

  //---- Stage 6: Palette lookup ----
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
  
  reg [11:0] rgb_r6;
  reg in_win_r6;
  
  always @(posedge clk_pix) begin
    if (rst_pix) begin
      rgb_r6 <= 12'h000;
      in_win_r6 <= 1'b0;
    end else begin
      rgb_r6 <= palette[fb_colr_r5];
      in_win_r6 <= in_win_r5;
    end
  end
  
  //---- Final output ----
  assign VGA_Red   = in_win_r6 ? rgb_r6[11:8] : 4'h0;
  assign VGA_Green = in_win_r6 ? rgb_r6[7:4]  : 4'h0;
  assign VGA_Blue  = in_win_r6 ? rgb_r6[3:0]  : 4'h0;
  
  assign frame_ready = (state == DRAW);

endmodule