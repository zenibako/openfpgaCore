//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
//------------------------------------------------------------------------------

//
// APF save-state handshake (clk_74a domain).
//
// The Pocket's sleep/wake and "Memories" both use the bridge commands
// 0x00A0 (Savestate: Start/Query) and 0x00A4 (Savestate: Load/Query),
// decoded by core_bridge_cmd.  This block answers them for a soft-CPU
// core: the hardware acknowledges each request at once and reports busy,
// the OS does the actual work (serialize the app into / out of the CRAM0
// blob window) and reports ok or err back through a toggle.
//
//   Save: host 0xA0+start -> ack, busy -> CPU sees start_req_tgl flip,
//         writes the blob to CRAM0 at SS_CRAM0_OFFSET, hands CRAM0 to the
//         bridge, then toggles start_done (err bit alongside) -> ok/err.
//         Host then reads savestate_size bytes from savestate_addr.
//   Load: host writes the blob to savestate_addr FIRST, then 0xA4+load
//         -> ack, busy -> CPU sees load_req_tgl flip, reads the blob,
//         toggles load_done -> ok/err.
//
// The order of operations above matches agg23's openFPGA-NES
// save_state_controller.sv, a shipping Pocket core with sleep support.
//
// Wake: the Pocket reloads the bitstream and pushes the blob and its 0xA4
// at once.  Measured on hardware, both are already there when the OS
// starts -- long before a soft-CPU core has an app that could restore
// anything.  So while no app has registered (cpu_size == 0, its reset
// value) or the core is still in APF reset, a load is acknowledged and
// reported ok at once -- the blob is already on its way into CRAM0, whose
// bridge path depends on neither -- and load_req_tgl is left flipped, so
// the OS finds the request pending when it boots and the app applies it
// when it starts.  A save in that state is refused.  This block stays
// alive through APF reset (por_n is its only reset); entering reset from a
// running core (Reset Enter) clears any stale request and the counters.
//
// Bridge reads/writes inside the blob window are counted (rd_words,
// wr_words) so firmware can tell when the host has finished pulling the
// blob (and so keep CRAM0 on the bridge side until then) and whether a
// pushed blob arrived whole.  The counters are quasi-static from the CPU
// side: firmware reads them twice until two reads agree.
//

`default_nettype none

module savestate_ctrl #(
    parameter [31:0] SS_BRIDGE_ADDR = 32'h2060_0000,  // CRAM0 bridge window
    parameter [31:0] SS_MAX_SIZE    = 32'h0010_0000   // 1 MB blob window
) (
    input  wire        clk_74a,
    input  wire        por_n,           // power-on reset only (PLL lock)
    input  wire        core_reset_n,    // APF reset (clk_74a domain)

    // core_bridge_cmd side
    input  wire        savestate_start,
    output reg         savestate_start_ack,
    output reg         savestate_start_busy,
    output reg         savestate_start_ok,
    output reg         savestate_start_err,

    input  wire        savestate_load,
    output reg         savestate_load_ack,
    output reg         savestate_load_busy,
    output reg         savestate_load_ok,
    output reg         savestate_load_err,

    output wire        savestate_supported,
    output wire [31:0] savestate_addr,
    output wire [31:0] savestate_size,
    output wire [31:0] savestate_maxloadsize,

    // APF bridge (for blob-window traffic counters)
    input  wire [31:0] bridge_addr,
    input  wire        bridge_rd,
    input  wire        bridge_wr,

    // CPU side (raw clk_cpu signals; synchronized here)
    input  wire        cpu_start_done_tgl,
    input  wire        cpu_start_err,
    input  wire        cpu_load_done_tgl,
    input  wire        cpu_load_err,
    input  wire        cpu_clear_tgl,       // zero both window counters
    input  wire [31:0] cpu_size,            // quasi-static; 0 = SS_MAX_SIZE

    // To CPU (synchronized on the CPU side)
    output reg         start_req_tgl,
    output reg         load_req_tgl,
    output reg  [31:0] rd_words,
    output reg  [31:0] wr_words
);

assign savestate_supported   = 1'b1;
assign savestate_addr        = SS_BRIDGE_ADDR;
assign savestate_maxloadsize = SS_MAX_SIZE;

// Size is written by the OS when an app registers, long before any host
// request, so a plain multi-bit 2FF sync is safe (quasi-static, like RTC).
reg [31:0] size_s1, size_s2;
always @(posedge clk_74a) begin
    size_s1 <= cpu_size;
    size_s2 <= size_s1;
end
wire [31:0] size_clamped = (size_s2 == 32'd0 || size_s2 > SS_MAX_SIZE)
                           ? SS_MAX_SIZE : {size_s2[31:2], 2'b00};
assign savestate_size = size_clamped;

// An app that can answer requests itself: out of reset and registered.
wire app_ready = core_reset_n && (size_s2 != 32'd0);

// CPU -> 74a toggles.  The err bits are written in the same store as the
// toggle; they are sampled two cycles after the synchronized toggle edge,
// by which time their own synchronizer has settled too.
wire start_done_s, load_done_s, clear_s, start_err_s, load_err_s;
synch_3 #(.WIDTH(5)) sync_cpu (
    {cpu_start_done_tgl, cpu_load_done_tgl, cpu_clear_tgl, cpu_start_err, cpu_load_err},
    {start_done_s,       load_done_s,       clear_s,       start_err_s,   load_err_s},
    clk_74a
);

reg start_done_d, load_done_d, clear_d;
reg [1:0] start_done_pipe, load_done_pipe;
reg start_prev, load_prev;
reg bridge_rd_prev;
reg core_reset_n_d;

wire in_window = (bridge_addr[31:24] == SS_BRIDGE_ADDR[31:24]) &&
                 (bridge_addr[23:0]  >= SS_BRIDGE_ADDR[23:0])  &&
                 (bridge_addr[23:0]  <  SS_BRIDGE_ADDR[23:0] + SS_MAX_SIZE[23:0]);

always @(posedge clk_74a or negedge por_n) begin
    if (!por_n) begin
        savestate_start_ack  <= 1'b0;
        savestate_start_busy <= 1'b0;
        savestate_start_ok   <= 1'b0;
        savestate_start_err  <= 1'b0;
        savestate_load_ack   <= 1'b0;
        savestate_load_busy  <= 1'b0;
        savestate_load_ok    <= 1'b0;
        savestate_load_err   <= 1'b0;
        start_req_tgl        <= 1'b0;
        load_req_tgl         <= 1'b0;
        rd_words             <= 32'd0;
        wr_words             <= 32'd0;
        start_prev           <= 1'b0;
        load_prev            <= 1'b0;
        bridge_rd_prev       <= 1'b0;
        start_done_d         <= 1'b0;
        load_done_d          <= 1'b0;
        clear_d              <= 1'b0;
        start_done_pipe      <= 2'b00;
        load_done_pipe       <= 2'b00;
        core_reset_n_d       <= 1'b0;
    end else begin
        core_reset_n_d <= core_reset_n;

        start_prev     <= savestate_start;
        load_prev      <= savestate_load;
        bridge_rd_prev <= bridge_rd;
        start_done_d   <= start_done_s;
        load_done_d    <= load_done_s;
        clear_d        <= clear_s;

        // Edge of a CPU toggle -> act two cycles later (err settled).
        start_done_pipe <= {start_done_pipe[0], start_done_s ^ start_done_d};
        load_done_pipe  <= {load_done_pipe[0],  load_done_s  ^ load_done_d};

        // ---- Save (0xA0 + start) ----
        if (savestate_start && !start_prev) begin
            savestate_start_ack  <= 1'b1;
            savestate_start_busy <= 1'b1;
            savestate_start_ok   <= 1'b0;
            savestate_start_err  <= 1'b0;
            savestate_load_ok    <= 1'b0;
            savestate_load_err   <= 1'b0;
            if (app_ready) begin
                start_req_tgl    <= ~start_req_tgl;
            end else begin
                // Nobody to serialize anything: refuse.
                savestate_start_busy <= 1'b0;
                savestate_start_err  <= 1'b1;
            end
            rd_words             <= 32'd0;
        end else if (!savestate_start) begin
            savestate_start_ack  <= 1'b0;
        end

        if (start_done_pipe[1] && savestate_start_busy) begin
            savestate_start_busy <= 1'b0;
            savestate_start_ok   <= ~start_err_s;
            savestate_start_err  <=  start_err_s;
        end

        // ---- Load (0xA4 + load) ----
        if (savestate_load && !load_prev) begin
            savestate_load_ack   <= 1'b1;
            savestate_load_busy  <= 1'b1;
            savestate_load_ok    <= 1'b0;
            savestate_load_err   <= 1'b0;
            savestate_start_ok   <= 1'b0;
            savestate_start_err  <= 1'b0;
            load_req_tgl         <= ~load_req_tgl;
            if (!app_ready) begin
                // Wake: accepted now, restored once the app is up.
                savestate_load_busy <= 1'b0;
                savestate_load_ok   <= 1'b1;
            end
        end else if (!savestate_load) begin
            savestate_load_ack   <= 1'b0;
        end

        if (load_done_pipe[1] && savestate_load_busy) begin
            savestate_load_busy  <= 1'b0;
            savestate_load_ok    <= ~load_err_s;
            savestate_load_err   <=  load_err_s;
        end

        // ---- Reset Enter from a running core: drop stale state ----
        // (Placed before the counters so a blob pushed in reset still
        // counts; requests arriving in the same cycle cannot happen, the
        // bridge is busy with the 0x0010 command.)
        if (core_reset_n_d && !core_reset_n) begin
            savestate_start_busy <= 1'b0;
            savestate_start_ok   <= 1'b0;
            savestate_start_err  <= 1'b0;
            savestate_load_busy  <= 1'b0;
            savestate_load_ok    <= 1'b0;
            savestate_load_err   <= 1'b0;
            start_req_tgl        <= 1'b0;
            load_req_tgl         <= 1'b0;
        end

        // ---- Blob-window traffic counters ----
        if (core_reset_n_d && !core_reset_n) begin
            rd_words <= 32'd0;
            wr_words <= 32'd0;
        end else if (clear_s ^ clear_d) begin
            rd_words <= 32'd0;
            wr_words <= 32'd0;
        end else begin
            if (bridge_rd && !bridge_rd_prev && in_window)
                rd_words <= rd_words + 32'd1;
            if (bridge_wr && in_window)
                wr_words <= wr_words + 32'd1;
        end
    end
end

endmodule

`default_nettype wire
