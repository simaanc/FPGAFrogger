// -----------------------------------------------------------------------------
//  VGA Frame-buffer Top (640×480 @ 60 Hz)   –   double-buffered
//  with CPU-done handshake, using Project-F display_480p timing block
// -----------------------------------------------------------------------------
//  Tested with Vivado 2018.2  (Artix-7, Basys-3, 25 MHz pixel clock)
// -----------------------------------------------------------------------------

`timescale 1ns/1ps
`default_nettype none

module vga_framebuffer_top #(
    parameter FB_WIDTH  = 224,
    parameter FB_HEIGHT = 256,
    parameter BPP       = 4,
    parameter INIT_FILE = "background.mem"
)(
    // clocks & reset ----------------------------------------------------------
    input  wire clk_pix,      // 25 MHz pixel clock
    input  wire rst_pix,

    // CPU write port ----------------------------------------------------------
    input  wire         cpu_we,
    input  wire [15:0]  cpu_addr,
    input  wire [BPP-1:0] cpu_dat,
    input  wire         cpu_done,   // asserted by CPU when frame is finished

    // VGA outputs -------------------------------------------------------------
    output wire         VGA_Hsync,
    output wire         VGA_Vsync,
    output wire [BPP-1:0] VGA_Red,
    output wire [BPP-1:0] VGA_Green,
    output wire [BPP-1:0] VGA_Blue,

    // status ------------------------------------------------------------------
    output wire         frame_ready  // high while CPU may draw
);

    // -------------------------------------------------------------------------
    //                    Constants and local parameters
    // -------------------------------------------------------------------------
    localparam X0      = (640 - FB_WIDTH )/2;   // window offset
    localparam Y0      = (480 - FB_HEIGHT)/2;
    localparam DEPTH0  = FB_WIDTH * FB_HEIGHT;  // words per buffer
    localparam ADDRW   = $clog2(DEPTH0);        // BRAM address width

    // -------------------------------------------------------------------------
    //             Timing generator  (Project-F display_480p)
    // -------------------------------------------------------------------------
    wire de, frame, line;
    wire signed [15:0] sx, sy;   // screen position (signed)
    display_480p u_disp (
        .clk_pix (clk_pix),
        .rst_pix (rst_pix),
        .hsync   (VGA_Hsync),
        .vsync   (VGA_Vsync),
        .de      (de),
        .frame   (frame),        // 1-cycle pulse at start of frame
        .line    (line),
        .sx      (sx),
        .sy      (sy)
    );

    // de = 1 inside active area, so sx ∈ 0..639, sy ∈ 0..479
    wire [9:0] xpos = sx[9:0];
    wire [8:0] ypos = sy[8:0];
    wire       active = de;

    // -------------------------------------------------------------------------
    //         Frame-pulse detector  (rising edge of timing “frame”)
    // -------------------------------------------------------------------------
    reg prev_frame;
    wire frame_pulse = frame & ~prev_frame;
    always @(posedge clk_pix) prev_frame <= frame;

    // -------------------------------------------------------------------------
    //                    Double-buffering state machine
    // -------------------------------------------------------------------------
    localparam S_IDLE  = 2'd0,
               S_CLEAR = 2'd1,
               S_DRAW  = 2'd2,
               S_WAIT  = 2'd3;

    reg [1:0] state;
    reg fb_front;          // 0 = bram0 is front, 1 = bram1 is front
    reg clearing;
    reg [ADDRW-1:0] clear_addr;

    always @(posedge clk_pix) begin
        if (rst_pix) begin
            state      <= S_IDLE;
            fb_front   <= 1'b0;
            clearing   <= 1'b0;
            clear_addr <= {ADDRW{1'b0}};
        end else begin
            case (state)
            //-------------------------------------------------------------
            S_IDLE:  if (frame_pulse)
                        state <= S_CLEAR;
            //-------------------------------------------------------------
            S_CLEAR: begin
                        clearing   <= 1'b1;
                        clear_addr <= clear_addr + 1'b1;
                        if (clear_addr == DEPTH0-1) begin
                            clearing   <= 1'b0;
                            clear_addr <= {ADDRW{1'b0}};
                            state      <= S_DRAW;
                        end
                     end
            //-------------------------------------------------------------
            S_DRAW:  if (cpu_done) state <= S_WAIT;
            //-------------------------------------------------------------
            S_WAIT:  if (frame_pulse) begin
                        fb_front <= ~fb_front;   // swap now
                        state    <= S_CLEAR;     // clear new back buffer
                     end
            endcase
        end
    end

    assign frame_ready = (state == S_DRAW);

    // -------------------------------------------------------------------------
    //                    Write-side mux (CPU or clear)
    // -------------------------------------------------------------------------
    reg [ADDRW-1:0] fb_addr_write;
    reg [BPP-1:0]   fb_data_write;
    reg             fb_we;

    always @(posedge clk_pix) begin
        if (rst_pix) fb_we <= 1'b0;
        else begin
            if (clearing) begin
                fb_addr_write <= clear_addr;
                fb_data_write <= {BPP{1'b0}};
                fb_we         <= 1'b1;
            end
            else if (state == S_DRAW) begin
                fb_addr_write <= cpu_addr[ADDRW-1:0];
                fb_data_write <= cpu_dat;
                fb_we         <= cpu_we & (cpu_addr < DEPTH0);
            end
            else
                fb_we <= 1'b0;
        end
    end

    // -------------------------------------------------------------------------
    //               Pixel address pipeline  (4 stages total)
    // -------------------------------------------------------------------------
    // S1: inside window?
    reg in_win_r1;
    reg [9:0] xpos_r1; reg [8:0] ypos_r1;
    always @(posedge clk_pix) begin
        xpos_r1   <= xpos;
        ypos_r1   <= ypos;
        in_win_r1 <= active &
                     (xpos >= X0) & (xpos < X0+FB_WIDTH) &
                     (ypos >= Y0) & (ypos < Y0+FB_HEIGHT);
    end

    // S2: offsets
    reg [9:0]  x_off_r2;  reg [8:0] y_off_r2; reg in_win_r2;
    always @(posedge clk_pix) begin
        x_off_r2 <= xpos_r1 - X0;
        y_off_r2 <= ypos_r1 - Y0;
        in_win_r2 <= in_win_r1;
    end

    // S3: row_base = y * 224 (shift-add: 128+64+32)
    reg [ADDRW-1:0] row_base_r3; reg [9:0] x_off_r3; reg in_win_r3;
    always @(posedge clk_pix) begin
        row_base_r3 <= (y_off_r2 << 7) + (y_off_r2 << 6) + (y_off_r2 << 5);
        x_off_r3    <= x_off_r2;
        in_win_r3   <= in_win_r2;
    end

    // S4: final address
    reg [ADDRW-1:0] pix_addr_r4; reg in_win_r4;
    always @(posedge clk_pix) begin
        pix_addr_r4 <= row_base_r3 + x_off_r3;
        in_win_r4   <= in_win_r3;
    end

    // -------------------------------------------------------------------------
    //                Dual-port BRAMs (simple dual-port)
    // -------------------------------------------------------------------------
    wire [BPP-1:0] dout0, dout1;

    bram_sdp #(.WIDTH(BPP), .DEPTH(DEPTH0), .INIT_F(INIT_FILE)) bram0 (
        .clk_write (clk_pix),
        .clk_read  (clk_pix),
        .we        (fb_we &  fb_front),
        .addr_write(fb_addr_write),
        .addr_read (pix_addr_r4),
        .data_in   (fb_data_write),
        .data_out  (dout0)
    );

    bram_sdp #(.WIDTH(BPP), .DEPTH(DEPTH0), .INIT_F(INIT_FILE)) bram1 (
        .clk_write (clk_pix),
        .clk_read  (clk_pix),
        .we        (fb_we & ~fb_front),
        .addr_write(fb_addr_write),
        .addr_read (pix_addr_r4),
        .data_in   (fb_data_write),
        .data_out  (dout1)
    );

    // -------------------------------------------------------------------------
    //                Front-buffer selector (5-cycle delay)
    // -------------------------------------------------------------------------
    reg [4:0] fb_front_d;
    always @(posedge clk_pix) fb_front_d <= {fb_front_d[3:0], fb_front};

    reg [BPP-1:0] pix_col_r5; reg in_win_r5;
    always @(posedge clk_pix) begin
        pix_col_r5 <= fb_front_d[4] ? dout1 : dout0;
        in_win_r5  <= in_win_r4;
    end

    // -------------------------------------------------------------------------
    //                     16-entry RGB444 palette
    // -------------------------------------------------------------------------
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

    reg [11:0] rgb_r6; reg in_win_r6;
    always @(posedge clk_pix) begin
        rgb_r6    <= palette[pix_col_r5];
        in_win_r6 <= in_win_r5;
    end

    // output RGB
    assign VGA_Red   = in_win_r6 ? rgb_r6[11:8] : 4'h0;
    assign VGA_Green = in_win_r6 ? rgb_r6[ 7:4] : 4'h0;
    assign VGA_Blue  = in_win_r6 ? rgb_r6[ 3:0] : 4'h0;

endmodule
