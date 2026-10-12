//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
//------------------------------------------------------------------------------

// core_bridge_cmd + savestate_ctrl: the APF save-state handshake as the
// Pocket host drives it (0x00A0 / 0x00A4), with the CPU side as tb inputs.

`default_nettype none

module tb_savestate (
    input  wire        clk,
    input  wire        por_n,
    output wire        core_reset_n,

    input  wire [31:0] bridge_addr,
    input  wire        bridge_rd,
    output wire [31:0] bridge_rd_data,
    input  wire        bridge_wr,
    input  wire [31:0] bridge_wr_data,

    input  wire        cpu_start_done_tgl,
    input  wire        cpu_start_err,
    input  wire        cpu_load_done_tgl,
    input  wire        cpu_load_err,
    input  wire        cpu_clear_tgl,
    input  wire [31:0] cpu_size,

    output wire        start_req_tgl,
    output wire        load_req_tgl,
    output wire [31:0] rd_words,
    output wire [31:0] wr_words
);

wire        reset_n;
assign      core_reset_n = reset_n;
wire        shutdown_pending;
wire        osnotify_inmenu;
wire        dataslot_requestread, dataslot_requestwrite, dataslot_update;
wire [15:0] dataslot_requestread_id, dataslot_requestwrite_id, dataslot_update_id;
wire [31:0] dataslot_requestwrite_size, dataslot_update_size;
wire        dataslot_allcomplete;
wire [31:0] rtc_epoch_seconds, rtc_date_bcd, rtc_time_bcd;
wire        rtc_valid;
wire        target_dataslot_ack, target_dataslot_done;
wire [2:0]  target_dataslot_err;
wire [31:0] datatable_q;

wire        savestate_supported;
wire [31:0] savestate_addr, savestate_size, savestate_maxloadsize;
wire        savestate_start, savestate_start_ack, savestate_start_busy,
            savestate_start_ok, savestate_start_err;
wire        savestate_load, savestate_load_ack, savestate_load_busy,
            savestate_load_ok, savestate_load_err;

core_bridge_cmd dut (
    .clk                (clk),
    .reset_n            (reset_n),
    .bridge_endian_little(1'b0),
    .bridge_addr        (bridge_addr),
    .bridge_rd          (bridge_rd),
    .bridge_rd_data     (bridge_rd_data),
    .bridge_wr          (bridge_wr),
    .bridge_wr_data     (bridge_wr_data),
    .shutdown_pending   (shutdown_pending),
    .shutdown_ack_s     (1'b1),
    .status_boot_done   (1'b1),
    .status_setup_done  (1'b0),
    .status_running     (1'b1),
    .dataslot_requestread    (dataslot_requestread),
    .dataslot_requestread_id (dataslot_requestread_id),
    .dataslot_requestread_ack(1'b1),
    .dataslot_requestread_ok (1'b1),
    .dataslot_requestwrite     (dataslot_requestwrite),
    .dataslot_requestwrite_id  (dataslot_requestwrite_id),
    .dataslot_requestwrite_size(dataslot_requestwrite_size),
    .dataslot_requestwrite_ack (1'b1),
    .dataslot_requestwrite_ok  (1'b1),
    .dataslot_update      (dataslot_update),
    .dataslot_update_id   (dataslot_update_id),
    .dataslot_update_size (dataslot_update_size),
    .dataslot_allcomplete (dataslot_allcomplete),
    .rtc_epoch_seconds (rtc_epoch_seconds),
    .rtc_date_bcd      (rtc_date_bcd),
    .rtc_time_bcd      (rtc_time_bcd),
    .rtc_valid         (rtc_valid),
    .savestate_supported   (savestate_supported),
    .savestate_addr        (savestate_addr),
    .savestate_size        (savestate_size),
    .savestate_maxloadsize (savestate_maxloadsize),
    .osnotify_inmenu (osnotify_inmenu),
    .savestate_start     (savestate_start),
    .savestate_start_ack (savestate_start_ack),
    .savestate_start_busy(savestate_start_busy),
    .savestate_start_ok  (savestate_start_ok),
    .savestate_start_err (savestate_start_err),
    .savestate_load     (savestate_load),
    .savestate_load_ack (savestate_load_ack),
    .savestate_load_busy(savestate_load_busy),
    .savestate_load_ok  (savestate_load_ok),
    .savestate_load_err (savestate_load_err),
    .target_dataslot_read       (1'b0),
    .target_dataslot_write      (1'b0),
    .target_dataslot_getfile    (1'b0),
    .target_dataslot_openfile   (1'b0),
    .target_dataslot_ack  (target_dataslot_ack),
    .target_dataslot_done (target_dataslot_done),
    .target_dataslot_err  (target_dataslot_err),
    .target_dataslot_id        (16'b0),
    .target_dataslot_slotoffset(32'b0),
    .target_dataslot_bridgeaddr(32'b0),
    .target_dataslot_length    (32'b0),
    .target_buffer_param_struct(32'b0),
    .target_buffer_resp_struct (32'b0),
    .datatable_addr (10'b0),
    .datatable_wren (1'b0),
    .datatable_data (32'b0),
    .datatable_q    (datatable_q)
);

savestate_ctrl ss (
    .clk_74a               (clk),
    .por_n                 (por_n),
    .core_reset_n          (reset_n),
    .savestate_start       (savestate_start),
    .savestate_start_ack   (savestate_start_ack),
    .savestate_start_busy  (savestate_start_busy),
    .savestate_start_ok    (savestate_start_ok),
    .savestate_start_err   (savestate_start_err),
    .savestate_load        (savestate_load),
    .savestate_load_ack    (savestate_load_ack),
    .savestate_load_busy   (savestate_load_busy),
    .savestate_load_ok     (savestate_load_ok),
    .savestate_load_err    (savestate_load_err),
    .savestate_supported   (savestate_supported),
    .savestate_addr        (savestate_addr),
    .savestate_size        (savestate_size),
    .savestate_maxloadsize (savestate_maxloadsize),
    .bridge_addr           (bridge_addr),
    .bridge_rd             (bridge_rd),
    .bridge_wr             (bridge_wr),
    .cpu_start_done_tgl    (cpu_start_done_tgl),
    .cpu_start_err         (cpu_start_err),
    .cpu_load_done_tgl     (cpu_load_done_tgl),
    .cpu_load_err          (cpu_load_err),
    .cpu_clear_tgl         (cpu_clear_tgl),
    .cpu_size              (cpu_size),
    .start_req_tgl         (start_req_tgl),
    .load_req_tgl          (load_req_tgl),
    .rd_words              (rd_words),
    .wr_words              (wr_words)
);

endmodule
