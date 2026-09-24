# ps7_starlite.tcl -- configure a ZYNQ7 Processing System block for the Puzhi PZ7020-StarLite.
#
#   In a Vivado block design with a processing_system7 instance named "ps7":
#     source ps7_starlite.tcl
#
# Every value is from the vendor schematic/manual -- see ../PS-CONFIG.md for the source of each.
# Property names are those emitted by Vivado's write_bd_tcl for processing_system7 v5.5
# (Vivado 2020-2024); if your version renames one, the GUI shows the same setting under
# Page Navigator > PS-PL / Peripheral I/O Pins / DDR Configuration / Clock Configuration.

set ps7 [get_bd_cells ps7]

# ---- DDR width: 16 Bit matches schematic V1.0 (one MT41K256M16). Set 32 Bit ONLY if the
#      board physically carries two DRAM chips (PS-CONFIG.md section 1).
set DDR_BUS_WIDTH "16 Bit"
# ---- DDR rail: measure TP3. 1.35 V -> "DDR 3 (Low Voltage)", 1.5 V -> "DDR 3".
set DDR_TYPE "DDR 3 (Low Voltage)"

set_property -dict [list \
  CONFIG.PCW_CRYSTAL_PERIPHERAL_FREQMHZ {33.333333} \
  CONFIG.PCW_APU_PERIPHERAL_FREQMHZ     {766.666666} \
  CONFIG.PCW_PRESET_BANK0_VOLTAGE       {LVCMOS 3.3V} \
  CONFIG.PCW_PRESET_BANK1_VOLTAGE       {LVCMOS 1.8V} \
  \
  CONFIG.PCW_UIPARAM_DDR_PARTNO         {MT41K256M16 RE-125} \
  CONFIG.PCW_UIPARAM_DDR_MEMORY_TYPE    $DDR_TYPE \
  CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH      $DDR_BUS_WIDTH \
  CONFIG.PCW_UIPARAM_DDR_ECC            {Disabled} \
  CONFIG.PCW_UIPARAM_DDR_FREQ_MHZ       {533.333333} \
  CONFIG.PCW_UIPARAM_DDR_TRAIN_WRITE_LEVEL {1} \
  CONFIG.PCW_UIPARAM_DDR_TRAIN_READ_GATE   {1} \
  CONFIG.PCW_UIPARAM_DDR_TRAIN_DATA_EYE    {1} \
  \
  CONFIG.PCW_QSPI_PERIPHERAL_ENABLE     {1} \
  CONFIG.PCW_QSPI_QSPI_IO               {MIO 1 .. 6} \
  CONFIG.PCW_QSPI_GRP_SINGLE_SS_ENABLE  {1} \
  CONFIG.PCW_QSPI_GRP_FBCLK_ENABLE      {0} \
  \
  CONFIG.PCW_UART0_PERIPHERAL_ENABLE    {1} \
  CONFIG.PCW_UART0_UART0_IO             {MIO 10 .. 11} \
  CONFIG.PCW_UART0_BAUD_RATE            {115200} \
  \
  CONFIG.PCW_ENET0_PERIPHERAL_ENABLE    {1} \
  CONFIG.PCW_ENET0_ENET0_IO             {MIO 16 .. 27} \
  CONFIG.PCW_ENET0_GRP_MDIO_ENABLE      {1} \
  CONFIG.PCW_ENET0_GRP_MDIO_IO          {MIO 52 .. 53} \
  CONFIG.PCW_ENET0_PERIPHERAL_FREQMHZ   {1000 Mbps} \
  \
  CONFIG.PCW_ENET1_PERIPHERAL_ENABLE    {1} \
  CONFIG.PCW_ENET1_ENET1_IO             {EMIO} \
  CONFIG.PCW_ENET1_PERIPHERAL_FREQMHZ   {1000 Mbps} \
  \
  CONFIG.PCW_USB0_PERIPHERAL_ENABLE     {1} \
  CONFIG.PCW_USB0_USB0_IO               {MIO 28 .. 39} \
  CONFIG.PCW_USB0_RESET_ENABLE          {1} \
  CONFIG.PCW_USB0_RESET_IO              {MIO 46} \
  \
  CONFIG.PCW_SD0_PERIPHERAL_ENABLE      {1} \
  CONFIG.PCW_SD0_SD0_IO                 {MIO 40 .. 45} \
  \
  CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ   {100} \
  CONFIG.PCW_USE_M_AXI_GP0              {1} \
  CONFIG.PCW_USE_S_AXI_HP0              {1} \
] $ps7

puts "ps7_starlite.tcl: PS7 configured -- DDR $DDR_BUS_WIDTH ($DDR_TYPE), QSPI MIO1-6, UART0 MIO10/11,\
 GEM0 MIO16-27 + MDIO 52/53 (PHY addr 1), GEM1 on EMIO (PL PHY addr 2 via GMII-to-RGMII), USB0 MIO28-39 rst MIO46, SD0 MIO40-45"
