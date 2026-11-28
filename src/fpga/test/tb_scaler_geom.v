//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------
//
// tb_scaler_geom: scaler_geom_cdc plus the Pocket core_top raster that uses
// it (H_TOTAL 780, centring of short slots in the 240-line window), so the
// harness can switch slot geometry at runtime and check per frame:
//   - DE lines and DE pixels per line equal the applied slot size;
//   - short slots are centred like core_top (first active line);
//   - geometry only changes at a frame boundary;
//   - every applied word is one the CPU side actually presented (no tearing)
//     and the latest value always arrives (no lost update).
// The raster formulas mirror src/fpga/targets/pocket/core_top.v; keep them
// in step if that file changes.
//
module tb_scaler_geom (
    input  wire        clk_cpu,
    input  wire        clk_vid,
    input  wire        reset_n,
    input  wire [2:0]  slot_cpu,
    input  wire [9:0]  width_cpu,
    input  wire [9:0]  height_cpu,
    input  wire [9:0]  v_total,       // frame length in lines (VRR stand-in)

    output wire [2:0]  slot_vid,
    output wire [9:0]  width_vid,
    output wire [9:0]  height_vid,
    output reg         frame_done,     // pulses at the end of each frame
    output reg  [9:0]  last_de_lines,  // DE lines in the frame just ended
    output reg  [9:0]  last_de_pixels, // DE pixels on the first active line
    output reg  [9:0]  last_first_line,// y_count of the first DE line
    output reg         last_mixed      // geometry changed mid-frame
);
    localparam CRT_V_SYNC   = 3;
    localparam CRT_V_BPORCH = 15;
    localparam CRT_H_SYNC   = 58;
    localparam CRT_H_BPORCH = 62;
    localparam CRT_H_FPORCH = 20;
    localparam CRT_H_TOTAL  = CRT_H_SYNC + CRT_H_BPORCH + 640 + CRT_H_FPORCH;
    localparam [9:0] CRT_V_ACTIVE_BASE   = CRT_V_SYNC + CRT_V_BPORCH;
    localparam [9:0] CRT_V_CENTER_HEIGHT = 10'd240;

    reg [9:0] x_count, y_count;

    scaler_geom_cdc dut (
        .clk_cpu(clk_cpu),
        .reset_cpu_n(reset_n),
        .slot_cpu(slot_cpu),
        .width_cpu(width_cpu),
        .height_cpu(height_cpu),
        .clk_vid(clk_vid),
        .reset_vid_n(reset_n),
        .frame_start_vid(x_count == 10'd0 && y_count == 10'd0),
        .slot_vid(slot_vid),
        .width_vid(width_vid),
        .height_vid(height_vid)
    );

    wire [9:0] crt_v_center_offset =
        (height_vid < CRT_V_CENTER_HEIGHT) ?
        ((CRT_V_CENTER_HEIGHT - height_vid) >> 1) : 10'd0;
    wire [9:0] crt_h_active_end   = CRT_H_SYNC + CRT_H_BPORCH + width_vid;
    wire [9:0] crt_v_active_start = CRT_V_ACTIVE_BASE + crt_v_center_offset;
    wire [9:0] crt_v_active_end   = crt_v_active_start + height_vid;
    wire [9:0] v_total_safe =
        (v_total < crt_v_active_end + 10'd4) ? crt_v_active_end + 10'd4 : v_total;

    wire de = (x_count >= CRT_H_SYNC + CRT_H_BPORCH) && (x_count < crt_h_active_end) &&
              (y_count >= crt_v_active_start) && (y_count < crt_v_active_end);

    reg [9:0]  de_lines, de_pixels, first_line, line_pixels;
    reg        line_had_de, seen_first, mixed;
    reg [22:0] frame_word;

    always @(posedge clk_vid or negedge reset_n) begin
        if (!reset_n) begin
            x_count <= 10'd0; y_count <= 10'd0;
            de_lines <= 0; de_pixels <= 0; first_line <= 0; line_pixels <= 0;
            line_had_de <= 0; seen_first <= 0; mixed <= 0;
            frame_done <= 0; last_de_lines <= 0; last_de_pixels <= 0;
            last_first_line <= 0; last_mixed <= 0;
            frame_word <= 23'd0;
        end else begin
            frame_done <= 1'b0;
            if (x_count == 10'd1 && y_count == 10'd0)
                frame_word <= {slot_vid, width_vid, height_vid};
            else if (frame_word != {slot_vid, width_vid, height_vid} &&
                     !(x_count <= 10'd1 && y_count == 10'd0))
                mixed <= 1'b1;

            if (de) begin
                line_pixels <= line_pixels + 10'd1;
                if (!line_had_de) begin
                    line_had_de <= 1'b1;
                    de_lines <= de_lines + 10'd1;
                    if (!seen_first) begin
                        seen_first <= 1'b1;
                        first_line <= y_count;
                    end
                end
            end

            x_count <= x_count + 10'd1;
            if (x_count == CRT_H_TOTAL - 1) begin
                x_count <= 10'd0;
                if (line_had_de && de_pixels == 10'd0)
                    de_pixels <= line_pixels;
                line_pixels <= 10'd0;
                line_had_de <= 1'b0;
                y_count <= y_count + 10'd1;
                if (y_count >= v_total_safe - 10'd1) begin
                    y_count <= 10'd0;
                    frame_done      <= 1'b1;
                    last_de_lines   <= de_lines;
                    last_de_pixels  <= de_pixels;
                    last_first_line <= first_line;
                    last_mixed      <= mixed;
                    de_lines <= 0; de_pixels <= 0; first_line <= 0;
                    seen_first <= 0; mixed <= 0;
                end
            end
        end
    end
endmodule
