//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------
//
// scaler_geom_cdc: carries the selected Pocket scaler slot and its raster
// size from clk_cpu to clk_vid, and applies it at a frame boundary.
//
// The CPU side is a level (axi_periph_slave's slot register and the size of
// that slot in its writable table).  A two-phase req/ack handshake moves it:
//   - clk_cpu loads `hold` only when the previous transfer has been
//     acknowledged, so `hold` is frozen while clk_vid copies it (no torn
//     capture of the 23-bit word);
//   - a change made while a transfer is in flight is not lost: once the ack
//     returns, `hold` differs from the source again and the next transfer
//     starts, so clk_vid always converges on the latest value;
//   - clk_vid keeps the received word pending and applies it only on
//     `frame_start_vid`, so the raster never changes size mid-frame.
//
// Writes closer together than one round trip (~2 clk_vid + 2 clk_cpu cycles)
// collapse to the last value, which is the intended level semantics.
//
module scaler_geom_cdc (
    input  wire        clk_cpu,
    input  wire        reset_cpu_n,
    input  wire [2:0]  slot_cpu,
    input  wire [9:0]  width_cpu,
    input  wire [9:0]  height_cpu,

    input  wire        clk_vid,
    input  wire        reset_vid_n,
    input  wire        frame_start_vid,
    output reg  [2:0]  slot_vid,
    output reg  [9:0]  width_vid,
    output reg  [9:0]  height_vid
);
    localparam [22:0] RESET_WORD = {3'd0, 10'd320, 10'd240};

    wire [22:0] word_cpu = {slot_cpu, width_cpu, height_cpu};

    reg  [22:0] hold;
    reg         req_cpu;
    reg  [1:0]  ack_sync_cpu;
    reg  [1:0]  req_sync_vid;
    reg         ack_vid;
    reg  [22:0] pending_vid;

    always @(posedge clk_cpu or negedge reset_cpu_n) begin
        if (!reset_cpu_n) begin
            hold         <= RESET_WORD;
            req_cpu      <= 1'b0;
            ack_sync_cpu <= 2'b00;
        end else begin
            ack_sync_cpu <= {ack_sync_cpu[0], ack_vid};
            if (ack_sync_cpu[1] == req_cpu && hold != word_cpu) begin
                hold    <= word_cpu;
                req_cpu <= ~req_cpu;
            end
        end
    end

    always @(posedge clk_vid or negedge reset_vid_n) begin
        if (!reset_vid_n) begin
            req_sync_vid <= 2'b00;
            ack_vid      <= 1'b0;
            pending_vid  <= RESET_WORD;
            {slot_vid, width_vid, height_vid} <= RESET_WORD;
        end else begin
            req_sync_vid <= {req_sync_vid[0], req_cpu};
            if (req_sync_vid[1] != ack_vid) begin
                pending_vid <= hold;            // frozen until this ack lands
                ack_vid     <= req_sync_vid[1];
            end
            if (frame_start_vid)
                {slot_vid, width_vid, height_vid} <= pending_vid;
        end
    end
endmodule
