// xv6H on the ZedBoard: Rocket with the hypervisor extension.
package freechips.rocketchip.system

import org.chipsalliance.cde.config.Config
import freechips.rocketchip.subsystem._
import freechips.rocketchip.rocket._
import freechips.rocketchip.devices.tilelink.{BootROMLocated, BootROMParams}
import freechips.rocketchip.util.SystemFileName

class XV6HZedBase extends Config(
  new WithHypervisor ++
  new WithNBigCores(1) ++
  new WithNExtTopInterrupts(4) ++
  new WithCoherentBusTopology ++
  new BaseConfig
)
class XV6HZedConfig extends Config(new XV6HZedBase)
class XV6HZedSmallConfig extends Config(
  new WithL1ICacheSets(32) ++ new WithL1DCacheSets(32) ++ new WithNoBtb ++ new XV6HZedBase)

// The SoC the shell wraps: RAM at 0x8000_0000 (256 MB, the upper half of the ZedBoard's DDR
// through S_AXI_HP0), device window at 0x1000_0000 (the 16550 at 0x1000_0000 that xv6H
// expects, and the RAM-disk controller), reset jumps straight to DRAM (boot ROM above).
class WithXV6HBootROM extends Config((site, here, up) => {
  case BootROMLocated(x) => up(BootROMLocated(x)).map(_.copy(
    hang = 0x10000, appendDTB = false,
    contentFileName = SystemFileName(sys.env.getOrElse("XV6H_BOOTROM", "./bootrom/xv6h-bootrom.img"))))
})
class XV6HZedSoCConfig extends Config(
  new WithXV6HBootROM ++
  new WithCustomMMIOPort(BigInt("10000000", 16), BigInt("10000000", 16), 64, 4, 64) ++
  new WithCustomMemPort(BigInt("80000000", 16), BigInt("10000000", 16), 64, 4, 64) ++
  new WithTimebase(BigInt(1000000)) ++
  new XV6HZedBase
)
