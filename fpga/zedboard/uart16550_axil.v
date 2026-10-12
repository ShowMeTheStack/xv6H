// A 16550-compatible UART on an AXI4-Lite slave, for xv6H on the ZedBoard.
// The register subset xv6's uart.c and Linux's 8250 driver use: RBR/THR, IER, IIR/FCR, LCR,
// MCR, LSR, MSR, SCR, and the divisor latch (DLL/DLM when LCR.DLAB). Byte registers at byte
// offsets 0..7 (reg-shift 0), so the register is addr[2:0] and the byte lane is addr[1:0] on
// the 32-bit bus. 16-byte receive and transmit FIFOs; 8N1 only; interrupt on received data
// (IER.0) and on an empty transmitter (IER.1). Fixed 115200 baud (see div).
module uart16550_axil #(
  parameter integer CLK_HZ = 40000000,
  parameter integer RESET_DIV = 22          // 40 MHz / (16 * 115200) ~ 21.7
) (
  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s_axi_aclk CLK" *)
  (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi, ASSOCIATED_RESET s_axi_aresetn" *)
  input  wire        s_axi_aclk,
  input  wire        s_axi_aresetn,
  input  wire [11:0] s_axi_awaddr,
  input  wire        s_axi_awvalid,
  output reg         s_axi_awready,
  input  wire [31:0] s_axi_wdata,
  input  wire [3:0]  s_axi_wstrb,
  input  wire        s_axi_wvalid,
  output reg         s_axi_wready,
  output reg  [1:0]  s_axi_bresp,
  output reg         s_axi_bvalid,
  input  wire        s_axi_bready,
  input  wire [11:0] s_axi_araddr,
  input  wire        s_axi_arvalid,
  output reg         s_axi_arready,
  output reg  [31:0] s_axi_rdata,
  output reg  [1:0]  s_axi_rresp,
  output reg         s_axi_rvalid,
  input  wire        s_axi_rready,
  output wire        tx,
  input  wire        rx,
  (* X_INTERFACE_INFO = "xilinx.com:signal:interrupt:1.0 irq INTERRUPT" *)
  (* X_INTERFACE_PARAMETER = "SENSITIVITY LEVEL_HIGH" *)
  output wire        irq
);
  wire clk = s_axi_aclk, rst = !s_axi_aresetn;

  // ---- registers ----
  reg [7:0] ier, lcr, mcr, scr, dll, dlm;
  wire dlab = lcr[7];

  // ---- FIFOs ----
  // 5-bit pointers, and every comparison of them made in 5 bits: `txr + 1' is a 32-bit sum in
  // Verilog, so at txr = 31 it would be 32, never equal to the wrapped txw, and the
  // transmitter-empty interrupt would be lost once every 32 bytes (a writer waiting for it
  // would sleep for ever); the full tests are computed in 5 bits for the same reason
  reg [7:0] txf [0:15]; reg [4:0] txw, txr; wire [4:0] tx_count = txw - txr, txr_next = txr + 5'd1;
  wire tx_empty = txw == txr; wire tx_full = tx_count == 5'd16;
  reg [7:0] rxf [0:15]; reg [4:0] rxw, rxr; wire [4:0] rx_count = rxw - rxr;
  wire rx_empty = rxw == rxr; wire rx_full = rx_count == 5'd16;
  reg thre_irq;                 // "transmitter empty" indication, cleared by reading IIR or writing THR

  // ---- baud: 16x oversampling tick ----
  // the line runs at a fixed 115200 whatever the divisor latch says: xv6's driver programs
  // it for 38400 at QEMU's 1.8432 MHz, Linux's from the tree's clock-frequency, and the ARM's
  // bridge on the other end is fixed. DLL/DLM stay readable and writable.
  wire [15:0] div = RESET_DIV[15:0];
  reg [15:0] bcnt; reg tick16;
  always @(posedge clk) begin
    if (rst) begin bcnt <= 0; tick16 <= 0; end
    else if (bcnt + 1 >= div) begin bcnt <= 0; tick16 <= 1; end
    else begin bcnt <= bcnt + 1; tick16 <= 0; end
  end

  // ---- transmitter ----
  reg [9:0] tsh; reg [3:0] tbits, t16; reg tbusy;
  assign tx = tbusy ? tsh[0] : 1'b1;
  wire tx_pop = !tbusy && !tx_empty;
  always @(posedge clk) begin
    if (rst) begin tbusy <= 0; tsh <= 10'h3ff; tbits <= 0; t16 <= 0; end
    else if (tx_pop) begin tsh <= {1'b1, txf[txr[3:0]], 1'b0}; tbusy <= 1; tbits <= 0; t16 <= 0; end
    else if (tbusy && tick16) begin
      t16 <= t16 + 1;
      if (t16 == 15) begin
        tsh <= {1'b1, tsh[9:1]};
        tbits <= tbits + 1;
        if (tbits == 9) tbusy <= 0;
      end
    end
  end

  // ---- receiver ----
  reg [1:0] rsync; always @(posedge clk) rsync <= {rsync[0], rx};
  wire rxd = rsync[1];
  reg rbusy; reg [3:0] r16, rbits; reg [7:0] rsh;
  reg rx_push; reg [7:0] rx_byte;
  always @(posedge clk) begin
    rx_push <= 0;
    if (rst) begin rbusy <= 0; r16 <= 0; rbits <= 0; end
    else if (!rbusy) begin
      if (!rxd && tick16) begin rbusy <= 1; r16 <= 0; rbits <= 0; end
    end else if (tick16) begin
      r16 <= r16 + 1;
      if (rbits == 0 && r16 == 7) begin               // middle of the start bit
        if (rxd) rbusy <= 0;                          // a glitch
        else begin rbits <= 1; r16 <= 0; end
      end else if (rbits != 0 && r16 == 15) begin
        r16 <= 0;
        if (rbits <= 8) begin rsh <= {rxd, rsh[7:1]}; rbits <= rbits + 1; end
        else begin rbusy <= 0; rx_push <= 1; rx_byte <= rsh; end   // the stop bit
      end
    end
  end

  // ---- interrupt ----
  wire rx_int = ier[0] && !rx_empty;
  wire tx_int = ier[1] && thre_irq;
  assign irq = rx_int || tx_int;
  wire [7:0] iir = rx_int ? 8'hc4 : tx_int ? 8'hc2 : 8'hc1;   // FIFOs enabled (bits 7:6)
  wire [7:0] lsr = {1'b0, tx_empty && !tbusy, tx_empty, 4'b0, !rx_empty};

  // ---- AXI4-Lite ----
  reg [11:0] awa; reg aw_ok, w_ok; reg [31:0] wd; reg [3:0] ws;
  wire [2:0] wreg = awa[2:0];
  wire [31:0] wsh = wd >> (8 * awa[1:0]);
  wire [7:0] wbyte = wsh[7:0];
  wire do_write = aw_ok && w_ok && !s_axi_bvalid;
  always @(posedge clk) begin
    if (rst) begin
      s_axi_awready <= 0; s_axi_wready <= 0; s_axi_bvalid <= 0; s_axi_bresp <= 0;
      s_axi_arready <= 0; s_axi_rvalid <= 0; s_axi_rresp <= 0; s_axi_rdata <= 0;
      aw_ok <= 0; w_ok <= 0;
      ier <= 0; lcr <= 8'h03; mcr <= 0; scr <= 0; dll <= RESET_DIV[7:0]; dlm <= 0;
      txw <= 0; txr <= 0; rxw <= 0; rxr <= 0; thre_irq <= 0;
    end else begin
      // transmitter FIFO pop, receiver FIFO push
      if (tx_pop) begin txr <= txr_next; if (txw == txr_next) thre_irq <= 1; end
      if (rx_push && !rx_full) begin rxf[rxw[3:0]] <= rx_byte; rxw <= rxw + 1; end

      // write address and data
      s_axi_awready <= !aw_ok && s_axi_awvalid && !s_axi_awready;
      if (s_axi_awvalid && s_axi_awready) begin awa <= s_axi_awaddr; aw_ok <= 1; end
      s_axi_wready <= !w_ok && s_axi_wvalid && !s_axi_wready;
      if (s_axi_wvalid && s_axi_wready) begin wd <= s_axi_wdata; ws <= s_axi_wstrb; w_ok <= 1; end
      if (do_write) begin
        aw_ok <= 0; w_ok <= 0; s_axi_bvalid <= 1; s_axi_bresp <= 0;
        if (ws[awa[1:0]]) case (wreg)
          3'd0: if (dlab) dll <= wbyte; else if (!tx_full) begin txf[txw[3:0]] <= wbyte; txw <= txw + 1; thre_irq <= 0; end
          3'd1: if (dlab) dlm <= wbyte; else begin ier <= wbyte & 8'h0f; if (wbyte[1] && tx_empty) thre_irq <= 1; end
          3'd2: begin if (wbyte[1]) rxr <= rxw; if (wbyte[2]) txr <= txw; end   // FCR: reset FIFOs
          3'd3: lcr <= wbyte;
          3'd4: mcr <= wbyte;
          3'd7: scr <= wbyte;
          default: ;
        endcase
      end
      if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 0;

      // read
      s_axi_arready <= !s_axi_rvalid && s_axi_arvalid && !s_axi_arready;
      if (s_axi_arvalid && s_axi_arready) begin
        s_axi_rvalid <= 1; s_axi_rresp <= 0;
        case (s_axi_araddr[2:0])
          3'd0: if (dlab) s_axi_rdata <= {4{dll}}; else begin
                  s_axi_rdata <= {4{rx_empty ? 8'h00 : rxf[rxr[3:0]]}};
                  if (!rx_empty) rxr <= rxr + 1;
                end
          3'd1: s_axi_rdata <= {4{dlab ? dlm : ier}};
          3'd2: begin s_axi_rdata <= {4{iir}}; if (!rx_int && tx_int) thre_irq <= 0; end
          3'd3: s_axi_rdata <= {4{lcr}};
          3'd4: s_axi_rdata <= {4{mcr}};
          3'd5: s_axi_rdata <= {4{lsr}};
          3'd6: s_axi_rdata <= {4{8'hb0}};      // MSR: DCD, DSR, CTS
          3'd7: s_axi_rdata <= {4{scr}};
        endcase
      end
      if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 0;
    end
  end
endmodule
