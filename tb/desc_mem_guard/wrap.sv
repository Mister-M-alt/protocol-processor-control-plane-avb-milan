// SPDX-License-Identifier: CERN-OHL-W-2.0
// Store + guard seam with independent hard and store-only reset controls.
`default_nettype none
module desc_mem_guard_wrap (
    input  wire         clk_i,
    input  wire         rst_n,
    input  wire         store_rst_n,
    input  wire         st_req_i,
    input  wire  [19:0] st_addr_i,
    input  wire  [63:0] st_wdata_i,
    output wire         st_rvalid_o,
    output wire  [63:0] st_rdata_o,
    output wire         st_err_o,
    output wire         img_valid_o,
    output wire         mem_req_valid_o,
    input  wire         mem_req_ready_i,
    output wire  [31:0] mem_req_addr_o,
    output wire   [8:0] mem_req_beats_o,
    input  wire         mem_rsp_valid_i,
    output wire         mem_rsp_ready_o,
    input  wire  [63:0] mem_rsp_data_i,
    input  wire         mem_rsp_last_i,
    input  wire         mem_rsp_err_i,
    output wire         debt_o,
    output wire         store_req_o,
    output wire         store_ready_o,
    input  wire         unit_i,
    input  wire         unit_req_valid_i,
    input  wire  [31:0] unit_req_addr_i,
    input  wire   [8:0] unit_req_beats_i,
    input  wire         unit_rsp_ready_i,
    output wire         unit_rsp_valid_o,
    output wire  [63:0] unit_rsp_data_o,
    output wire         unit_rsp_last_o,
    output wire         unit_rsp_err_o
);
  wire [31:0] req_addr_w;
  wire  [8:0] req_beats_w;
  wire        rsp_valid_w, rsp_ready_w, rsp_last_w, rsp_err_w;
  wire [63:0] rsp_data_w;
  wire        st_ready_w, name_wr_nc_w;
  wire  [3:0] fault_w;
  wire [15:0] misses_w, fetches_w, writes_w, length_w;
  assign unit_rsp_valid_o = rsp_valid_w;
  assign unit_rsp_data_o = rsp_data_w;
  assign unit_rsp_last_o = rsp_last_w;
  assign unit_rsp_err_o = rsp_err_w;

  KL_aecp_desc_store u_store (
      .clk_i(clk_i), .rst_n(rst_n && store_rst_n),
      .st_req_i(st_req_i), .st_we_i(1'b0), .st_name_i(1'b0),
      .st_addr_i(st_addr_i), .st_wdata_i(st_wdata_i), .st_wstrb_i(8'd0),
      .st_ready_o(st_ready_w), .st_rvalid_o(st_rvalid_o), .st_rdata_o(st_rdata_o),
      .st_err_o(st_err_o), .name_wr_o(name_wr_nc_w),
      .mem_req_valid_o(store_req_o), .mem_req_ready_i(store_ready_o),
      .mem_req_addr_o(req_addr_w), .mem_req_beats_o(req_beats_w),
      .mem_rsp_valid_i(rsp_valid_w), .mem_rsp_ready_o(rsp_ready_w),
      .mem_rsp_data_i(rsp_data_w), .mem_rsp_last_i(rsp_last_w),
      .mem_rsp_err_i(rsp_err_w),
      .dbg_img_valid_o(img_valid_o), .dbg_fault_o(fault_w), .dbg_locate_miss_o(misses_w),
      .dbg_fetch_cnt_o(fetches_w), .dbg_ro_write_o(writes_w), .dbg_desc_len_o(length_w)
  );

`ifdef DESC_GUARD_BASELINE
  // Reproduction only: the unmodified store connected directly to the FIFO.
  assign mem_req_valid_o = store_req_o;
  assign store_ready_o = mem_req_ready_i;
  assign mem_req_addr_o = req_addr_w;
  assign mem_req_beats_o = req_beats_w;
  assign rsp_valid_w = mem_rsp_valid_i;
  assign mem_rsp_ready_o = rsp_ready_w;
  assign rsp_data_w = mem_rsp_data_i;
  assign rsp_last_w = mem_rsp_last_i;
  assign rsp_err_w = mem_rsp_err_i;
  assign debt_o = 1'b0;
`else
  KL_aecp_desc_mem_guard u_guard (
      .clk_i(clk_i), .rst_n(rst_n),
      .s_req_valid_i(unit_i ? unit_req_valid_i : store_req_o),
      .s_req_ready_o(store_ready_o),
      .s_req_addr_i(unit_i ? unit_req_addr_i : req_addr_w),
      .s_req_beats_i(unit_i ? unit_req_beats_i : req_beats_w),
      .s_rsp_valid_o(rsp_valid_w),
      .s_rsp_ready_i(unit_i ? unit_rsp_ready_i : rsp_ready_w),
      .s_rsp_data_o(rsp_data_w), .s_rsp_last_o(rsp_last_w), .s_rsp_err_o(rsp_err_w),
      .m_req_valid_o(mem_req_valid_o), .m_req_ready_i(mem_req_ready_i),
      .m_req_addr_o(mem_req_addr_o), .m_req_beats_o(mem_req_beats_o),
      .m_rsp_valid_i(mem_rsp_valid_i), .m_rsp_ready_o(mem_rsp_ready_o),
      .m_rsp_data_i(mem_rsp_data_i), .m_rsp_last_i(mem_rsp_last_i),
      .m_rsp_err_i(mem_rsp_err_i), .debt_o(debt_o)
  );
`endif
endmodule : desc_mem_guard_wrap
`default_nettype wire
