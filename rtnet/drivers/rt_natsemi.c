/* natsemi.c: A Linux PCI Ethernet driver for the NatSemi DP8381x series. */
/*
	Written/copyright 1999-2001 by Donald Becker.
	Portions copyright (c) 2001,2002 Sun Microsystems (thockin@sun.com)
	Portions copyright 2001,2002 Manfred Spraul (manfred@colorfullife.com)

	This software may be used and distributed according to the terms of
	the GNU General Public License (GPL), incorporated herein by reference.
	Drivers based on or derived from this code fall under the GPL and must
	retain the authorship, copyright and license notice.  This file is not
	a complete program and may only be used when the entire operating
	system is licensed under the GPL.  License for under other terms may be
	available.  Contact the original author for details.

	The original author may be reached as becker@scyld.com, or at
	Scyld Computing Corporation
	410 Severn Ave., Suite 210
	Annapolis MD 21403

	Support information and updates available at
	http://www.scyld.com/network/netsemi.html


	Linux kernel modifications:

	Version 1.0.1:
		- Spinlock fixes
		- Bug fixes and better intr performance (Tjeerd)
	Version 1.0.2:
		- Now reads correct MAC address from eeprom
	Version 1.0.3:
		- Eliminate redundant priv->tx_full flag
		- Call netif_start_queue from dev->tx_timeout
		- wmb() in start_tx() to flush data
		- Update Tx locking
		- Clean up PCI enable (davej)
	Version 1.0.4:
		- Merge Donald Becker's natsemi.c version 1.07
	Version 1.0.5:
		- { fill me in }
	Version 1.0.6:
		* ethtool support (jgarzik)
		* Proper initialization of the card (which sometimes
		fails to occur and leaves the card in a non-functional
		state). (uzi)

		* Some documented register settings to optimize some
		of the 100Mbit autodetection circuitry in rev C cards. (uzi)

		* Polling of the PHY intr for stuff like link state
		change and auto- negotiation to finally work properly. (uzi)

		* One-liner removal of a duplicate declaration of
		netdev_error(). (uzi)

	Version 1.0.7: (Manfred Spraul)
		* pci dma
		* SMP locking update
		* full reset added into tx_timeout
		* correct multicast hash generation (both big and little endian)
			[copied from a natsemi driver version
			 from Myrio Corporation, Greg Smith]
		* suspend/resume

	version 1.0.8 (Tim Hockin <thockin@sun.com>)
		* ETHTOOL_* support
		* Wake on lan support (Erik Gilling)
		* MXDMA fixes for serverworks
		* EEPROM reload

	version 1.0.9 (Manfred Spraul)
		* Main change: fix lack of synchronize
		netif_close/netif_suspend against a last interrupt
		or packet.
		* do not enable superflous interrupts (e.g. the
		drivers relies on TxDone - TxIntr not needed)
		* wait that the hardware has really stopped in close
		and suspend.
		* workaround for the (at least) gcc-2.95.1 compiler
		problem. Also simplifies the code a bit.
		* disable_irq() in tx_timeout - needed to protect
		against rx interrupts.
		* stop the nic before switching into silent rx mode
		for wol (required according to docu).

	version 1.0.10:
		* use long for ee_addr (various)
		* print pointers properly (DaveM)
		* include asm/irq.h (?)

	version 1.0.11:
		* check and reset if PHY errors appear (Adrian Sun)
		* WoL cleanup (Tim Hockin)
		* Magic number cleanup (Tim Hockin)
		* Don't reload EEPROM on every reset (Tim Hockin)
		* Save and restore EEPROM state across reset (Tim Hockin)
		* MDIO Cleanup (Tim Hockin)
		* Reformat register offsets/bits (jgarzik)

	version 1.0.12:
		* ETHTOOL_* further support (Tim Hockin)

	version 1.0.13:
		* ETHTOOL_[G]EEPROM support (Tim Hockin)

	version 1.0.13:
		* crc cleanup (Matt Domsch <Matt_Domsch@dell.com>)

	version 1.0.14:
		* Cleanup some messages and autoneg in ethtool (Tim Hockin)

	version 1.0.15:
		* Get rid of cable_magic flag
		* use new (National provided) solution for cable magic issue

	version 1.0.16:
		* call netdev_rx() for RxErrors (Manfred Spraul)
		* formatting and cleanups
		* change options and full_duplex arrays to be zero
		  initialized
		* enable only the WoL and PHY interrupts in wol mode

	version 1.0.17:
		* only do cable_magic on 83815 and early 83816 (Tim Hockin)
		* create a function for rx refill (Manfred Spraul)
		* combine drain_ring and init_ring (Manfred Spraul)
		* oom handling (Manfred Spraul)
		* hands_off instead of playing with netif_device_{de,a}ttach
		  (Manfred Spraul)
		* be sure to write the MAC back to the chip (Manfred Spraul)
		* lengthen EEPROM timeout, and always warn about timeouts
		  (Manfred Spraul)
		* comments update (Manfred)
		* do the right thing on a phy-reset (Manfred and Tim)

	TODO:
	* big endian support with CFG:BEM instead of cpu_to_le32
	* support for an external PHY
	* NAPI
	
	Ported to RTNET: December 2003, Erik Buit <e.buit@student.utwente.nl>
*/

#if !defined(__OPTIMIZE__)
#warning  You must compile this file with the correct options!
#warning  See the last lines of the source file.
#error You must compile this driver with "-O".
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/ethtool.h>
#include <linux/delay.h>
#include <linux/rtnetlink.h>
#include <linux/mii.h>
#include <asm/processor.h>	/* Processor type for cache alignment. */
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>

/*** RTnet ***/
#include <rtnet_port.h>

#define MAX_UNITS 8		/* More are supported, limit only on options */
#define DEFAULT_RX_POOL_SIZE    16

static int cards[MAX_UNITS] = { [0 ... (MAX_UNITS-1)] = 1 };
compat_module_int_param_array(cards, MAX_UNITS);
MODULE_PARM_DESC(cards, "array of cards to be supported (e.g. 1,0,1)");
/*** RTnet ***/

#define DRV_NAME	"natsemi-rt"
#define DRV_VERSION	"1.07+LK1.0.17-RTnet-0.2"
#define DRV_RELDATE	"Dec 16, 2003"

/* Updated to recommendations in pci-skeleton v2.03. */

/* The user-configurable values.
   These may be modified when a driver module is loaded.*/

#define NATSEMI_DEF_MSG		(NETIF_MSG_DRV		| \
				 NETIF_MSG_LINK		| \
				 NETIF_MSG_WOL		| \
				 NETIF_MSG_RX_ERR	| \
				 NETIF_MSG_TX_ERR)
static int debug = -1;

/* Maximum events (Rx packets, etc.) to handle at each interrupt. */
static int max_interrupt_work = 20;
static int mtu;

/* Maximum number of multicast addresses to filter (vs. rx-all-multicast).
   This chip uses a 512 element hash table based on the Ethernet CRC.  */
static int multicast_filter_limit = 100;

/* Set the copy breakpoint for the copy-only-tiny-frames scheme.
   Setting to > 1518 effectively disables this feature. */
/*** RTnet ***
static int rx_copybreak;
 *** RTnet ***/

/* Used to pass the media type, etc.
   Both 'options[]' and 'full_duplex[]' should exist for driver
   interoperability.
   The media type is usually passed in 'options[]'.
*/
static int options[MAX_UNITS];
static int full_duplex[MAX_UNITS];

/* Operational parameters that are set at compile time. */

/* Keep the ring sizes a power of two for compile efficiency.
   The compiler will convert <unsigned>'%'<2^N> into a bit mask.
   Making the Tx ring too large decreases the effectiveness of channel
   bonding and packet priority.
   There are no ill effects from too-large receive rings. */
#define TX_RING_SIZE	16
#define TX_QUEUE_LEN	10 /* Limit ring entries actually used, min 4. */
#define RX_RING_SIZE	8 /*** RTnet ***/

/* Operational parameters that usually are not changed. */
/* Time in jiffies before concluding the transmitter is hung. */
#define TX_TIMEOUT  (2*HZ)

#define NATSEMI_HW_TIMEOUT	400
#define NATSEMI_TIMER_FREQ	3*HZ
#define NATSEMI_PG0_NREGS	64
#define NATSEMI_RFDR_NREGS	8
#define NATSEMI_PG1_NREGS	4
#define NATSEMI_NREGS		(NATSEMI_PG0_NREGS + NATSEMI_RFDR_NREGS + \
				 NATSEMI_PG1_NREGS)
#define NATSEMI_REGS_VER	1 /* v1 added RFDR registers */
#define NATSEMI_REGS_SIZE	(NATSEMI_NREGS * sizeof(u32))
#define NATSEMI_EEPROM_SIZE	24 /* 12 16-bit values */

#define PKT_BUF_SZ		1536 /* Size of each temporary Rx buffer. */

/* These identify the driver base version and may not be removed. */
static char version[] __devinitdata =
  KERN_INFO DRV_NAME " dp8381x driver, version "
      DRV_VERSION ", " DRV_RELDATE "\n"
  KERN_INFO "  originally by Donald Becker <becker@scyld.com>\n"
  KERN_INFO "  http://www.scyld.com/network/natsemi.html\n"
  KERN_INFO "  2.4.x kernel port by Jeff Garzik, Tjeerd Mulder\n"
  KERN_INFO "  RTnet port by Erik Buit\n";

MODULE_AUTHOR("Erik Buit");
MODULE_DESCRIPTION("RTnet National Semiconductor DP8381x series PCI Ethernet driver");
MODULE_LICENSE("GPL");

module_param(max_interrupt_work, int, 0444);
module_param(mtu, int, 0444);
module_param(debug, int, 0444);
/*** RTnet ***
MODULE_PARM(rx_copybreak, "i");
 *** RTnet ***/
compat_module_int_param_array(options, MAX_UNITS);
compat_module_int_param_array(full_duplex, MAX_UNITS);
MODULE_PARM_DESC(max_interrupt_work,
	"DP8381x maximum events handled per interrupt");
MODULE_PARM_DESC(mtu, "DP8381x MTU (all boards)");
MODULE_PARM_DESC(debug, "DP8381x default debug level");
/*** RTnet ***
MODULE_PARM_DESC(rx_copybreak, 
	"DP8381x copy breakpoint for copy-only-tiny-frames");
 *** RTnet ***/
MODULE_PARM_DESC(options, "DP8381x: Bits 0-3: media type, bit 17: full duplex");
MODULE_PARM_DESC(full_duplex, "DP8381x full duplex setting(s) (1)");

/*
				Theory of Operation

I. Board Compatibility

This driver is designed for National Semiconductor DP83815 PCI Ethernet NIC.
It also works with other chips in in the DP83810 series.

II. Board-specific settings

This driver requires the PCI interrupt line to be valid.
It honors the EEPROM-set values.

III. Driver operation

IIIa. Ring buffers

This driver uses two statically allocated fixed-size descriptor lists
formed into rings by a branch from the final descriptor to the beginning of
the list.  The ring sizes are set at compile time by RX/TX_RING_SIZE.
The NatSemi design uses a 'next descriptor' pointer that the driver forms
into a list.

IIIb/c. Transmit/Receive Structure

This driver uses a zero-copy receive and transmit scheme.
The driver allocates full frame size skbuffs for the Rx ring buffers at
open() time and passes the skb->data field to the chip as receive data
buffers.  When an incoming frame is less than RX_COPYBREAK bytes long,
a fresh skbuff is allocated and the frame is copied to the new skbuff.
When the incoming frame is larger, the skbuff is passed directly up the
protocol stack.  Buffers consumed this way are replaced by newly allocated
skbuffs in a later phase of receives.

The RX_COPYBREAK value is chosen to trade-off the memory wasted by
using a full-sized skbuff for small frames vs. the copying costs of larger
frames.  New boards are typically used in generously configured machines
and the underfilled buffers have negligible impact compared to the benefit of
a single allocation size, so the default value of zero results in never
copying packets.  When copying is done, the cost is usually mitigated by using
a combined copy/checksum routine.  Copying also preloads the cache, which is
most useful with small frames.

A subtle aspect of the operation is that unaligned buffers are not permitted
by the hardware.  Thus the IP header at offset 14 in an ethernet frame isn't
longword aligned for further processing.  On copies frames are put into the
skbuff at an offset of "+2", 16-byte aligning the IP header.

IIId. Synchronization

Most operations are synchronized on the np->lock irq spinlock, except the
performance critical codepaths:

The rx process only runs in the interrupt handler. Access from outside
the interrupt handler is only permitted after disable_irq().

The rx process usually runs under the dev->xmit_lock. If np->intr_tx_reap
is set, then access is permitted under spin_lock_irq(&np->lock).

Thus configuration functions that want to access everything must call
	disable_irq(dev->irq);
	spin_lock_bh(dev->xmit_lock);
	spin_lock_irq(&np->lock);

IV. Notes

NatSemi PCI network controllers are very uncommon.

IVb. References

http://www.scyld.com/expert/100mbps.html
http://www.scyld.com/expert/NWay.html
Datasheet is available from:
http://www.national.com/pf/DP/DP83815.html

IVc. Errata
	
None characterised.
*/



enum pcistuff {
	PCI_USES_IO = 0x01,
	PCI_USES_MEM = 0x02,
	PCI_USES_MASTER = 0x04,
	PCI_ADDR0 = 0x08,
	PCI_ADDR1 = 0x10,
};

/* MMIO operations required */
#define PCI_IOTYPE (PCI_USES_MASTER | PCI_USES_MEM | PCI_ADDR1)


/* array of board data directly indexed by pci_tbl[x].driver_data */
static struct {
	const char *name;
	unsigned long flags;
} natsemi_pci_info[] __devinitdata = {
	{ "NatSemi DP8381[56]", PCI_IOTYPE },
};

static struct pci_device_id natsemi_pci_tbl[] __devinitdata = {
	{ PCI_VENDOR_ID_NS, PCI_DEVICE_ID_NS_83815, PCI_ANY_ID, PCI_ANY_ID, },
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, natsemi_pci_tbl);

/* Offsets to the device registers.
   Unlike software-only systems, device drivers interact with complex hardware.
   It's not useful to define symbolic names for every register bit in the
   device.
*/
enum register_offsets {
	ChipCmd			= 0x00,
	ChipConfig		= 0x04,
	EECtrl			= 0x08,
	PCIBusCfg		= 0x0C,
	IntrStatus		= 0x10,
	IntrMask		= 0x14,
	IntrEnable		= 0x18,
	IntrHoldoff		= 0x16, /* DP83816 only */
	TxRingPtr		= 0x20,
	TxConfig		= 0x24,
	RxRingPtr		= 0x30,
	RxConfig		= 0x34,
	ClkRun			= 0x3C,
	WOLCmd			= 0x40,
	PauseCmd		= 0x44,
	RxFilterAddr		= 0x48,
	RxFilterData		= 0x4C,
	BootRomAddr		= 0x50,
	BootRomData		= 0x54,
	SiliconRev		= 0x58,
	StatsCtrl		= 0x5C,
	StatsData		= 0x60,
	RxPktErrs		= 0x60,
	RxMissed		= 0x68,
	RxCRCErrs		= 0x64,
	BasicControl		= 0x80,
	BasicStatus		= 0x84,
	AnegAdv			= 0x90,
	AnegPeer		= 0x94,
	PhyStatus		= 0xC0,
	MIntrCtrl		= 0xC4,
	MIntrStatus		= 0xC8,
	PhyCtrl			= 0xE4,

	/* These are from the spec, around page 78... on a separate table.
	 * The meaning of these registers depend on the value of PGSEL. */
	PGSEL			= 0xCC,
	PMDCSR			= 0xE4,
	TSTDAT			= 0xFC,
	DSPCFG			= 0xF4,
	SDCFG			= 0xF8
};
/* the values for the 'magic' registers above (PGSEL=1) */
#define PMDCSR_VAL	0x189c	/* enable preferred adaptation circuitry */
#define TSTDAT_VAL	0x0
#define DSPCFG_VAL	0x5040
#define SDCFG_VAL	0x008c	/* set voltage thresholds for Signal Detect */
#define DSPCFG_LOCK	0x20	/* coefficient lock bit in DSPCFG */
#define TSTDAT_FIXED	0xe8	/* magic number for bad coefficients */

/* misc PCI space registers */
enum pci_register_offsets {
	PCIPM			= 0x44,
};

enum ChipCmd_bits {
	ChipReset		= 0x100,
	RxReset			= 0x20,
	TxReset			= 0x10,
	RxOff			= 0x08,
	RxOn			= 0x04,
	TxOff			= 0x02,
	TxOn			= 0x01,
};

enum ChipConfig_bits {
	CfgPhyDis		= 0x200,
	CfgPhyRst		= 0x400,
	CfgExtPhy		= 0x1000,
	CfgAnegEnable		= 0x2000,
	CfgAneg100		= 0x4000,
	CfgAnegFull		= 0x8000,
	CfgAnegDone		= 0x8000000,
	CfgFullDuplex		= 0x20000000,
	CfgSpeed100		= 0x40000000,
	CfgLink			= 0x80000000,
};

enum EECtrl_bits {
	EE_ShiftClk		= 0x04,
	EE_DataIn		= 0x01,
	EE_ChipSelect		= 0x08,
	EE_DataOut		= 0x02,
};

enum PCIBusCfg_bits {
	EepromReload		= 0x4,
};

/* Bits in the interrupt status/mask registers. */
enum IntrStatus_bits {
	IntrRxDone		= 0x0001,
	IntrRxIntr		= 0x0002,
	IntrRxErr		= 0x0004,
	IntrRxEarly		= 0x0008,
	IntrRxIdle		= 0x0010,
	IntrRxOverrun		= 0x0020,
	IntrTxDone		= 0x0040,
	IntrTxIntr		= 0x0080,
	IntrTxErr		= 0x0100,
	IntrTxIdle		= 0x0200,
	IntrTxUnderrun		= 0x0400,
	StatsMax		= 0x0800,
	SWInt			= 0x1000,
	WOLPkt			= 0x2000,
	LinkChange		= 0x4000,
	IntrHighBits		= 0x8000,
	RxStatusFIFOOver	= 0x10000,
	IntrPCIErr		= 0xf00000,
	RxResetDone		= 0x1000000,
	TxResetDone		= 0x2000000,
	IntrAbnormalSummary	= 0xCD20,
};

/*
 * Default Interrupts:
 * Rx OK, Rx Packet Error, Rx Overrun,
 * Tx OK, Tx Packet Error, Tx Underrun,
 * MIB Service, Phy Interrupt, High Bits,
 * Rx Status FIFO overrun,
 * Received Target Abort, Received Master Abort,
 * Signalled System Error, Received Parity Error
 */
#define DEFAULT_INTR 0x00f1cd65

enum TxConfig_bits {
	TxDrthMask		= 0x3f,
	TxFlthMask		= 0x3f00,
	TxMxdmaMask		= 0x700000,
	TxMxdma_512		= 0x0,
	TxMxdma_4		= 0x100000,
	TxMxdma_8		= 0x200000,
	TxMxdma_16		= 0x300000,
	TxMxdma_32		= 0x400000,
	TxMxdma_64		= 0x500000,
	TxMxdma_128		= 0x600000,
	TxMxdma_256		= 0x700000,
	TxCollRetry		= 0x800000,
	TxAutoPad		= 0x10000000,
	TxMacLoop		= 0x20000000,
	TxHeartIgn		= 0x40000000,
	TxCarrierIgn		= 0x80000000
};

enum RxConfig_bits {
	RxDrthMask		= 0x3e,
	RxMxdmaMask		= 0x700000,
	RxMxdma_512		= 0x0,
	RxMxdma_4		= 0x100000,
	RxMxdma_8		= 0x200000,
	RxMxdma_16		= 0x300000,
	RxMxdma_32		= 0x400000,
	RxMxdma_64		= 0x500000,
	RxMxdma_128		= 0x600000,
	RxMxdma_256		= 0x700000,
	RxAcceptLong		= 0x8000000,
	RxAcceptTx		= 0x10000000,
	RxAcceptRunt		= 0x40000000,
	RxAcceptErr		= 0x80000000
};

enum ClkRun_bits {
	PMEEnable		= 0x100,
	PMEStatus		= 0x8000,
};

enum WolCmd_bits {
	WakePhy			= 0x1,
	WakeUnicast		= 0x2,
	WakeMulticast		= 0x4,
	WakeBroadcast		= 0x8,
	WakeArp			= 0x10,
	WakePMatch0		= 0x20,
	WakePMatch1		= 0x40,
	WakePMatch2		= 0x80,
	WakePMatch3		= 0x100,
	WakeMagic		= 0x200,
	WakeMagicSecure		= 0x400,
	SecureHack		= 0x100000,
	WokePhy			= 0x400000,
	WokeUnicast		= 0x800000,
	WokeMulticast		= 0x1000000,
	WokeBroadcast		= 0x2000000,
	WokeArp			= 0x4000000,
	WokePMatch0		= 0x8000000,
	WokePMatch1		= 0x10000000,
	WokePMatch2		= 0x20000000,
	WokePMatch3		= 0x40000000,
	WokeMagic		= 0x80000000,
	WakeOptsSummary		= 0x7ff
};

enum RxFilterAddr_bits {
	RFCRAddressMask		= 0x3ff,
	AcceptMulticast		= 0x00200000,
	AcceptMyPhys		= 0x08000000,
	AcceptAllPhys		= 0x10000000,
	AcceptAllMulticast	= 0x20000000,
	AcceptBroadcast		= 0x40000000,
	RxFilterEnable		= 0x80000000
};

enum StatsCtrl_bits {
	StatsWarn		= 0x1,
	StatsFreeze		= 0x2,
	StatsClear		= 0x4,
	StatsStrobe		= 0x8,
};

enum MIntrCtrl_bits {
	MICRIntEn		= 0x2,
};

enum PhyCtrl_bits {
	PhyAddrMask		= 0xf,
};

/* values we might find in the silicon revision register */
#define SRR_DP83815_C	0x0302
#define SRR_DP83815_D	0x0403
#define SRR_DP83816_A4	0x0504
#define SRR_DP83816_A5	0x0505

/* The Rx and Tx buffer descriptors. */
/* Note that using only 32 bit fields simplifies conversion to big-endian
   architectures. */
struct netdev_desc {
	u32 next_desc;
	s32 cmd_status;
	u32 addr;
	u32 software_use;
};

/* Bits in network_desc.status */
enum desc_status_bits {
	DescOwn=0x80000000, DescMore=0x40000000, DescIntr=0x20000000,
	DescNoCRC=0x10000000, DescPktOK=0x08000000,
	DescSizeMask=0xfff,

	DescTxAbort=0x04000000, DescTxFIFO=0x02000000,
	DescTxCarrier=0x01000000, DescTxDefer=0x00800000,
	DescTxExcDefer=0x00400000, DescTxOOWCol=0x00200000,
	DescTxExcColl=0x00100000, DescTxCollCount=0x000f0000,

	DescRxAbort=0x04000000, DescRxOver=0x02000000,
	DescRxDest=0x01800000, DescRxLong=0x00400000,
	DescRxRunt=0x00200000, DescRxInvalid=0x00100000,
	DescRxCRC=0x00080000, DescRxAlign=0x00040000,
	DescRxLoop=0x00020000, DesRxColl=0x00010000,
};

struct netdev_private {
	/* Descriptor rings first for alignment */
	dma_addr_t ring_dma;
	struct netdev_desc *rx_ring;
	struct netdev_desc *tx_ring;
	/* The addresses of receive-in-place skbuffs */
	struct rtskb *rx_skbuff[RX_RING_SIZE]; /*** RTnet ***/
	dma_addr_t rx_dma[RX_RING_SIZE];
	/* address of a sent-in-place packet/buffer, for later free() */
	struct rtskb *tx_skbuff[TX_RING_SIZE]; /*** RTnet ***/
	dma_addr_t tx_dma[TX_RING_SIZE];
	struct net_device_stats stats;
	/* Media monitoring timer */
	struct timer_list timer;
	/* Frequently used values: keep some adjacent for cache effect */
	struct pci_dev *pci_dev;
	struct netdev_desc *rx_head_desc;
	/* Producer/consumer ring indices */
	unsigned int cur_rx, dirty_rx;
	unsigned int cur_tx, dirty_tx;
	/* Based on MTU+slack. */
	unsigned int rx_buf_sz;
	int oom;
	/* Do not touch the nic registers */
	int hands_off;
	/* These values are keep track of the transceiver/media in use */
	unsigned int full_duplex;
	/* Rx filter */
	u32 cur_rx_mode;
	u32 rx_filter[16];
	/* FIFO and PCI burst thresholds */
	u32 tx_config, rx_config;
	/* original contents of ClkRun register */
	u32 SavedClkRun;
	/* silicon revision */
	u32 srr;
	/* expected DSPCFG value */
	u16 dspcfg;
	/* MII transceiver section */
	u16 advertising;
	unsigned int iosize;
	rtdm_lock_t lock;
	u32 msg_enable;

	struct rtskb_queue skb_pool; /*** RTnet ***/
	rtdm_irq_t irq_handle;
};

static int eeprom_read(long ioaddr, int location);
static int mdio_read(struct rtnet_device *dev, int phy_id, int reg);
/*static void mdio_write(struct rtnet_device *dev, int phy_id, int reg, u16 data);*/
static void natsemi_reset(struct rtnet_device *dev);
static void natsemi_reload_eeprom(struct rtnet_device *dev);
static void natsemi_stop_rxtx(struct rtnet_device *dev);
static int netdev_open(struct rtnet_device *dev);
static void do_cable_magic(struct rtnet_device *dev);
static void undo_cable_magic(struct rtnet_device *dev);
static void check_link(struct rtnet_device *dev);
/*static void netdev_timer(unsigned long data);*/
static void dump_ring(struct rtnet_device *dev);
/*static void tx_timeout(struct rtnet_device *dev);*/
static int alloc_ring(struct rtnet_device *dev);
static void refill_rx(struct rtnet_device *dev);
static void init_ring(struct rtnet_device *dev);
static void drain_tx(struct rtnet_device *dev);
static void drain_ring(struct rtnet_device *dev);
static void free_ring(struct rtnet_device *dev);
/*static void reinit_ring(struct rtnet_device *dev);*/
static void init_registers(struct rtnet_device *dev);
static int start_tx(struct rtskb *skb, struct rtnet_device *dev);
static int intr_handler(rtdm_irq_t *irq_handle);
static void netdev_error(struct rtnet_device *dev, int intr_status);
static void netdev_rx(struct rtnet_device *dev, nanosecs_abs_t *time_stamp);
static void netdev_tx_done(struct rtnet_device *dev);
static void __set_rx_mode(struct rtnet_device *dev);
/*static void set_rx_mode(struct rtnet_device *dev);*/
static void __get_stats(struct rtnet_device *dev);
/*static struct net_device_stats *get_stats(struct net_device *dev);
static int netdev_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
static int netdev_set_wol(struct rtnet_device *dev, u32 newval);
static int netdev_get_wol(struct rtnet_device *dev, u32 *supported, u32 *cur);
static int netdev_set_sopass(struct rtnet_device *dev, u8 *newval);
static int netdev_get_sopass(struct rtnet_device *dev, u8 *data);
static int netdev_get_ecmd(struct rtnet_device *dev, struct ethtool_cmd *ecmd);
static int netdev_set_ecmd(struct rtnet_device *dev, struct ethtool_cmd *ecmd);
static void enable_wol_mode(struct rtnet_device *dev, int enable_intr);*/
static int netdev_close(struct rtnet_device *dev);
/*static int netdev_get_regs(struct rtnet_device *dev, u8 *buf);
static int netdev_get_eeprom(struct rtnet_device *dev, u8 *buf);*/


static int __devinit natsemi_probe1 (struct pci_dev *pdev,
	const struct pci_device_id *ent)
{
	struct rtnet_device *dev; /*** RTnet ***/
	struct netdev_private *np;
	int i, option, irq, chip_idx = ent->driver_data;
	static int find_cnt = -1;
	unsigned long ioaddr, iosize;
	const int pcibar = 1; /* PCI base address register */
	int prev_eedata;
	u32 tmp;

/* when built into the kernel, we only print version if device is found */
#ifndef MODULE
	static int printed_version;
	if (!printed_version++)
		rtdm_printk(version);
#endif

	i = pci_enable_device(pdev);
	if (i) return i;

	/* natsemi has a non-standard PM control register
	 * in PCI config space.  Some boards apparently need
	 * to be brought to D0 in this manner.
	 */
	pci_read_config_dword(pdev, PCIPM, &tmp);
	if (tmp & PCI_PM_CTRL_STATE_MASK) {
		/* D0 state, disable PME assertion */
		u32 newtmp = tmp & ~PCI_PM_CTRL_STATE_MASK;
		pci_write_config_dword(pdev, PCIPM, newtmp);
	}

	find_cnt++;
	ioaddr = pci_resource_start(pdev, pcibar);
	iosize = pci_resource_len(pdev, pcibar);
	irq = pdev->irq;

/*** RTnet ***/
	if (cards[find_cnt] == 0)
		goto err_out;
/*** RTnet ***/

	if (natsemi_pci_info[chip_idx].flags & PCI_USES_MASTER)
		pci_set_master(pdev);

/*** RTnet ***/
	dev = rt_alloc_etherdev(sizeof(struct netdev_private));
	if (dev == NULL) {
		rtdm_printk(KERN_ERR "init_ethernet failed for card #%d\n", find_cnt);
		goto err_out;
	}
	rtdev_alloc_name(dev, "rteth%d");
	rt_rtdev_connect(dev, &RTDEV_manager);
	RTNET_SET_MODULE_OWNER(dev);
	dev->vers = RTDEV_VERS_2_0;
/*** RTnet ***/

	i = pci_request_regions(pdev, dev->name);
	if (i) {
/*** RTnet ***/
		rt_rtdev_disconnect(dev);
		rtdev_free(dev);
/*** RTnet ***/
		return i;
	}

	{
		void *mmio = ioremap (ioaddr, iosize);
		if (!mmio) {
			pci_release_regions(pdev);
/*** RTnet ***/
			rt_rtdev_disconnect(dev);
			rtdev_free(dev);
/*** RTnet ***/
			return -ENOMEM;
		}
		ioaddr = (unsigned long) mmio;
	}

	/* Work around the dropped serial bit. */
	prev_eedata = eeprom_read(ioaddr, 6);
	for (i = 0; i < 3; i++) {
		int eedata = eeprom_read(ioaddr, i + 7);
		dev->dev_addr[i*2] = (eedata << 1) + (prev_eedata >> 15);
		dev->dev_addr[i*2+1] = eedata >> 7;
		prev_eedata = eedata;
	}

	dev->base_addr = ioaddr;
	dev->irq = irq;

	np = dev->priv;

	np->pci_dev = pdev;
	pci_set_drvdata(pdev, dev);
	np->iosize = iosize;
	rtdm_lock_init(&np->lock);
	np->msg_enable = (debug >= 0) ? (1<<debug)-1 : NATSEMI_DEF_MSG;
	np->hands_off = 0;

	/* Reset the chip to erase previous misconfiguration. */
	natsemi_reload_eeprom(dev);
	natsemi_reset(dev);

	option = find_cnt < MAX_UNITS ? options[find_cnt] : 0;
	if (dev->mem_start)
		option = dev->mem_start;

/*** RTnet ***/
	if (rtskb_pool_init(&np->skb_pool, RX_RING_SIZE*2) < RX_RING_SIZE*2) {
		rtskb_pool_release(&np->skb_pool);
		goto err_out_unmap;
	}
/*** RTnet ***/
	
	/* The lower four bits are the media type. */
	if (option) {
		if (option & 0x200)
			np->full_duplex = 1;
		if (option & 15)
			rtdm_printk(KERN_INFO
				"%s: ignoring user supplied media type %d",
				dev->name, option & 15);
	}
	if (find_cnt < MAX_UNITS  &&  full_duplex[find_cnt])
		np->full_duplex = 1;

	/* The chip-specific entries in the device structure. */
	dev->open = &netdev_open;
	dev->hard_start_xmit = &start_tx;
	dev->stop = &netdev_close;
/*** RTnet ***
	dev->get_stats = &get_stats;
	dev->set_multicast_list = &set_rx_mode;
	dev->do_ioctl = &netdev_ioctl;
	dev->tx_timeout = &tx_timeout;
	dev->watchdog_timeo = TX_TIMEOUT;
  *** RTnet ***/

	if (mtu)
		dev->mtu = mtu;

/*** RTnet ***/
	i = rt_register_rtnetdev(dev);
	if (i) {
		rtskb_pool_release(&np->skb_pool);
		goto err_out_unmap;
	}
/*** RTnet ***/
	
	rtnetif_carrier_off(dev);

	if (netif_msg_drv(np)) {
		rtdm_printk(KERN_INFO "%s: %s at %#08lx, ",
			dev->name, natsemi_pci_info[chip_idx].name, ioaddr);
		for (i = 0; i < ETH_ALEN-1; i++)
				rtdm_printk("%02x:", dev->dev_addr[i]);
		rtdm_printk("%02x, IRQ %d.\n", dev->dev_addr[i], irq);
	}

	np->advertising = mdio_read(dev, 1, MII_ADVERTISE);
	if ((readl((void *)(ioaddr + ChipConfig)) & 0xe000) != 0xe000
	 && netif_msg_probe(np)) {
		u32 chip_config = readl((void *)(ioaddr + ChipConfig));
		rtdm_printk(KERN_INFO "%s: Transceiver default autonegotiation %s "
			"10%s %s duplex.\n",
			dev->name,
			chip_config & CfgAnegEnable ?
			  "enabled, advertise" : "disabled, force",
			chip_config & CfgAneg100 ? "0" : "",
			chip_config & CfgAnegFull ? "full" : "half");
	}
	if (netif_msg_probe(np))
		rtdm_printk(KERN_INFO
			"%s: Transceiver status %#04x advertising %#04x.\n",
			dev->name, mdio_read(dev, 1, MII_BMSR),
			np->advertising);

	/* save the silicon revision for later querying */
	np->srr = readl((void *)(ioaddr + SiliconRev));
	if (netif_msg_hw(np))
		rtdm_printk(KERN_INFO "%s: silicon revision %#04x.\n",
				dev->name, np->srr);


	return 0;

err_out_unmap:
#ifdef USE_MEM
	iounmap((void *)ioaddr);
err_out_free_res:
#endif
	pci_release_regions(pdev);
/*err_out_free_netdev:*/
/*** RTnet ***/
	rt_rtdev_disconnect(dev);
	rtdev_free(dev);
/*** RTnet ***/
err_out:
	return -ENODEV;

}


/* Read the EEPROM and MII Management Data I/O (MDIO) interfaces.
   The EEPROM code is for the common 93c06/46 EEPROMs with 6 bit addresses. */

/* Delay between EEPROM clock transitions.
   No extra delay is needed with 33Mhz PCI, but future 66Mhz access may need
   a delay.  Note that pre-2.0.34 kernels had a cache-alignment bug that
   made udelay() unreliable.
   The old method of using an ISA access as a delay, __SLOW_DOWN_IO__, is
   depricated.
*/
#define eeprom_delay(ee_addr)	readl((void *)(ee_addr))

#define EE_Write0 (EE_ChipSelect)
#define EE_Write1 (EE_ChipSelect | EE_DataIn)

/* The EEPROM commands include the alway-set leading bit. */
enum EEPROM_Cmds {
	EE_WriteCmd=(5 << 6), EE_ReadCmd=(6 << 6), EE_EraseCmd=(7 << 6),
};

static int eeprom_read(long addr, int location)
{
	int i;
	int retval = 0;
	long ee_addr = addr + EECtrl;
	int read_cmd = location | EE_ReadCmd;
	writel(EE_Write0, (void *)ee_addr);

	/* Shift the read command bits out. */
	for (i = 10; i >= 0; i--) {
		short dataval = (read_cmd & (1 << i)) ? EE_Write1 : EE_Write0;
		writel(dataval, (void *)ee_addr);
		eeprom_delay(ee_addr);
		writel(dataval | EE_ShiftClk, (void *)ee_addr);
		eeprom_delay(ee_addr);
	}
	writel(EE_ChipSelect, (void *)ee_addr);
	eeprom_delay(ee_addr);

	for (i = 0; i < 16; i++) {
		writel(EE_ChipSelect | EE_ShiftClk, (void *)ee_addr);
		eeprom_delay(ee_addr);
		retval |= (readl((void *)ee_addr) & EE_DataOut) ? 1 << i : 0;
		writel(EE_ChipSelect, (void *)ee_addr);
		eeprom_delay(ee_addr);
	}

	/* Terminate the EEPROM access. */
	writel(EE_Write0, (void *)ee_addr);
	writel(0, (void *)ee_addr);
	return retval;
}

/* MII transceiver control section.
 * The 83815 series has an internal transceiver, and we present the
 * management registers as if they were MII connected. */

static int mdio_read(struct rtnet_device *dev, int phy_id, int reg)
{
	if (phy_id == 1 && reg < 32)
		return readl((void *)(dev->base_addr+BasicControl+(reg<<2)))&0xffff;
	else
		return 0xffff;
}
/*** RTnet
static void mdio_write(struct rtnet_device *dev, int phy_id, int reg, u16 data)
{
	struct netdev_private *np = dev->priv;
	if (phy_id == 1 && reg < 32) {
		writew(data, dev->base_addr+BasicControl+(reg<<2));
		switch (reg) {
			case MII_ADVERTISE: np->advertising = data; break;
		}
	}
}
RTnet ***/
/* CFG bits [13:16] [18:23] */
#define CFG_RESET_SAVE 0xfde000
/* WCSR bits [0:4] [9:10] */
#define WCSR_RESET_SAVE 0x61f
/* RFCR bits [20] [22] [27:31] */
#define RFCR_RESET_SAVE 0xf8500000;

static void natsemi_reset(struct rtnet_device *dev)
{
	int i;
	u32 cfg;
	u32 wcsr;
	u32 rfcr;
	u16 pmatch[3];
	u16 sopass[3];
	struct netdev_private *np = dev->priv;

	/*
	 * Resetting the chip causes some registers to be lost.
	 * Natsemi suggests NOT reloading the EEPROM while live, so instead
	 * we save the state that would have been loaded from EEPROM
	 * on a normal power-up (see the spec EEPROM map).  This assumes
	 * whoever calls this will follow up with init_registers() eventually.
	 */

	/* CFG */
	cfg = readl((void *)(dev->base_addr + ChipConfig)) & CFG_RESET_SAVE;
	/* WCSR */
	wcsr = readl((void *)(dev->base_addr + WOLCmd)) & WCSR_RESET_SAVE;
	/* RFCR */
	rfcr = readl((void *)(dev->base_addr + RxFilterAddr)) & RFCR_RESET_SAVE;
	/* PMATCH */
	for (i = 0; i < 3; i++) {
		writel(i*2, (void *)(dev->base_addr + RxFilterAddr));
		pmatch[i] = readw((void *)(dev->base_addr + RxFilterData));
	}
	/* SOPAS */
	for (i = 0; i < 3; i++) {
		writel(0xa+(i*2), (void *)(dev->base_addr + RxFilterAddr));
		sopass[i] = readw((void *)(dev->base_addr + RxFilterData));
	}

	/* now whack the chip */
	writel(ChipReset, (void *)(dev->base_addr + ChipCmd));
	for (i=0;i<NATSEMI_HW_TIMEOUT;i++) {
		if (!(readl((void *)(dev->base_addr + ChipCmd)) & ChipReset))
			break;
		udelay(5);
	}
	if (i==NATSEMI_HW_TIMEOUT) {
		rtdm_printk(KERN_WARNING "%s: reset did not complete in %d usec.\n",
			dev->name, i*5);
	} else if (netif_msg_hw(np)) {
		rtdm_printk(KERN_DEBUG "%s: reset completed in %d usec.\n",
			dev->name, i*5);
	}

	/* restore CFG */
	cfg |= readl((void *)(dev->base_addr + ChipConfig)) & ~CFG_RESET_SAVE;
	writel(cfg, (void *)(dev->base_addr + ChipConfig));
	/* restore WCSR */
	wcsr |= readl((void *)(dev->base_addr + WOLCmd)) & ~WCSR_RESET_SAVE;
	writel(wcsr, (void *)(dev->base_addr + WOLCmd));
	/* read RFCR */
	rfcr |= readl((void *)(dev->base_addr + RxFilterAddr)) & ~RFCR_RESET_SAVE;
	/* restore PMATCH */
	for (i = 0; i < 3; i++) {
		writel(i*2, (void *)(dev->base_addr + RxFilterAddr));
		writew(pmatch[i], (void *)(dev->base_addr + RxFilterData));
	}
	for (i = 0; i < 3; i++) {
		writel(0xa+(i*2), (void *)(dev->base_addr + RxFilterAddr));
		writew(sopass[i], (void *)(dev->base_addr + RxFilterData));
	}
	/* restore RFCR */
	writel(rfcr, (void *)(dev->base_addr + RxFilterAddr));
}

static void natsemi_reload_eeprom(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	int i;

	writel(EepromReload, (void *)(dev->base_addr + PCIBusCfg));
	for (i=0;i<NATSEMI_HW_TIMEOUT;i++) {
		udelay(50);
		if (!(readl((void *)(dev->base_addr + PCIBusCfg)) & EepromReload))
			break;
	}
	if (i==NATSEMI_HW_TIMEOUT) {
		rtdm_printk(KERN_WARNING "%s: EEPROM did not reload in %d usec.\n",
			dev->name, i*50);
	} else if (netif_msg_hw(np)) {
		rtdm_printk(KERN_DEBUG "%s: EEPROM reloaded in %d usec.\n",
			dev->name, i*50);
	}
}

static void natsemi_stop_rxtx(struct rtnet_device *dev)
{
	long ioaddr = dev->base_addr;
	struct netdev_private *np = dev->priv;
	int i;

	writel(RxOff | TxOff, (void *)(ioaddr + ChipCmd));
	for(i=0;i< NATSEMI_HW_TIMEOUT;i++) {
		if ((readl((void *)(ioaddr + ChipCmd)) & (TxOn|RxOn)) == 0)
			break;
		udelay(5);
	}
	if (i==NATSEMI_HW_TIMEOUT) {
		rtdm_printk(KERN_WARNING "%s: Tx/Rx process did not stop in %d usec.\n",
			dev->name, i*5);
	} else if (netif_msg_hw(np)) {
		rtdm_printk(KERN_DEBUG "%s: Tx/Rx process stopped in %d usec.\n",
			dev->name, i*5);
	}
}

static int netdev_open(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	long ioaddr = dev->base_addr;
	int i;

	RTNET_MOD_INC_USE_COUNT;

	/* Reset the chip, just in case. */
	natsemi_reset(dev);

/*** RTnet ***/
	rt_stack_connect(dev, &STACK_manager);
	i = rtdm_irq_request(&np->irq_handle, dev->irq, intr_handler, 0,
	                     "rt_natsemi", dev);
/*** RTnet ***/
/*	i = request_irq(dev->irq, &intr_handler, SA_SHIRQ, dev->name, dev);*/
	if (i) {
		RTNET_MOD_DEC_USE_COUNT;
		return i;
	}

	if (netif_msg_ifup(np))
		rtdm_printk(KERN_DEBUG "%s: netdev_open() irq %d.\n",
			dev->name, dev->irq);
	i = alloc_ring(dev);
	if (i < 0) {
		rtdm_irq_free(&np->irq_handle);
		RTNET_MOD_DEC_USE_COUNT;
		return i;
	}
	init_ring(dev);
	init_registers(dev);
	/* now set the MAC address according to dev->dev_addr */
	for (i = 0; i < 3; i++) {
		u16 mac = (dev->dev_addr[2*i+1]<<8) + dev->dev_addr[2*i];

		writel(i*2, (void *)(ioaddr + RxFilterAddr));
		writew(mac, (void *)(ioaddr + RxFilterData));
	}
	writel(np->cur_rx_mode, (void *)(ioaddr + RxFilterAddr));

	rtnetif_start_queue(dev); /*** RTnet ***/

	if (netif_msg_ifup(np))
		rtdm_printk(KERN_DEBUG "%s: Done netdev_open(), status: %#08x.\n",
			dev->name, (int)readl((void *)(ioaddr + ChipCmd)));

/*** RTnet ***/
	/* Set the timer to check for link beat. */
#if 0
	init_timer(&np->timer);
	np->timer.expires = jiffies + NATSEMI_TIMER_FREQ;
	np->timer.data = (unsigned long)dev;
	np->timer.function = &netdev_timer; /* timer handler */
	add_timer(&np->timer);
#endif
/*** RTnet ***/

	return 0;
}

static void do_cable_magic(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;

	if (np->srr >= SRR_DP83816_A5)
		return;

	/*
	 * 100 MBit links with short cables can trip an issue with the chip.
	 * The problem manifests as lots of CRC errors and/or flickering
	 * activity LED while idle.  This process is based on instructions
	 * from engineers at National.
	 */
	if (readl((void *)(dev->base_addr + ChipConfig)) & CfgSpeed100) {
		u16 data;

		writew(1, (void *)(dev->base_addr + PGSEL));
		/*
		 * coefficient visibility should already be enabled via
		 * DSPCFG | 0x1000
		 */
		data = readw((void *)(dev->base_addr + TSTDAT)) & 0xff;
		/*
		 * the value must be negative, and within certain values
		 * (these values all come from National)
		 */
		if (!(data & 0x80) || ((data >= 0xd8) && (data <= 0xff))) {
			struct netdev_private *np = dev->priv;

			/* the bug has been triggered - fix the coefficient */
			writew(TSTDAT_FIXED, (void *)(dev->base_addr + TSTDAT));
			/* lock the value */
			data = readw((void *)(dev->base_addr + DSPCFG));
			np->dspcfg = data | DSPCFG_LOCK;
			writew(np->dspcfg, (void *)(dev->base_addr + DSPCFG));
		}
		writew(0, (void *)(dev->base_addr + PGSEL));
	}
}

static void undo_cable_magic(struct rtnet_device *dev)
{
	u16 data;
	struct netdev_private *np = dev->priv;

	if (np->srr >= SRR_DP83816_A5)
		return;

	writew(1, (void *)(dev->base_addr + PGSEL));
	/* make sure the lock bit is clear */
	data = readw((void *)(dev->base_addr + DSPCFG));
	np->dspcfg = data & ~DSPCFG_LOCK;
	writew(np->dspcfg, (void *)(dev->base_addr + DSPCFG));
	writew(0, (void *)(dev->base_addr + PGSEL));
}

static void check_link(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	long ioaddr = dev->base_addr;
	int duplex;
	int chipcfg = readl((void *)(ioaddr + ChipConfig));

	if (!(chipcfg & CfgLink)) {
		if (rtnetif_carrier_ok(dev)) {
			if (netif_msg_link(np))
				rtdm_printk(KERN_NOTICE "%s: link down.\n",
					dev->name);
			rtnetif_carrier_off(dev);
			undo_cable_magic(dev);
		}
		return;
	}
	if (!rtnetif_carrier_ok(dev)) {
		if (netif_msg_link(np))
			rtdm_printk(KERN_NOTICE "%s: link up.\n", dev->name);
		rtnetif_carrier_on(dev);
		do_cable_magic(dev);
	}

	duplex = np->full_duplex || (chipcfg & CfgFullDuplex ? 1 : 0);

	/* if duplex is set then bit 28 must be set, too */
	if (duplex ^ !!(np->rx_config & RxAcceptTx)) {
		if (netif_msg_link(np))
			rtdm_printk(KERN_INFO
				"%s: Setting %s-duplex based on negotiated "
				"link capability.\n", dev->name,
				duplex ? "full" : "half");
		if (duplex) {
			np->rx_config |= RxAcceptTx;
			np->tx_config |= TxCarrierIgn | TxHeartIgn;
		} else {
			np->rx_config &= ~RxAcceptTx;
			np->tx_config &= ~(TxCarrierIgn | TxHeartIgn);
		}
		writel(np->tx_config, (void *)(ioaddr + TxConfig));
		writel(np->rx_config, (void *)(ioaddr + RxConfig));
	}
}

static void init_registers(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	long ioaddr = dev->base_addr;
	int i;

	for (i=0;i<NATSEMI_HW_TIMEOUT;i++) {
		if (readl((void *)(dev->base_addr + ChipConfig)) & CfgAnegDone)
			break;
		udelay(10);
	}
	if (i==NATSEMI_HW_TIMEOUT && netif_msg_link(np)) {
		rtdm_printk(KERN_INFO
			"%s: autonegotiation did not complete in %d usec.\n",
			dev->name, i*10);
	}

	/* On page 78 of the spec, they recommend some settings for "optimum
	   performance" to be done in sequence.  These settings optimize some
	   of the 100Mbit autodetection circuitry.  They say we only want to
	   do this for rev C of the chip, but engineers at NSC (Bradley
	   Kennedy) recommends always setting them.  If you don't, you get
	   errors on some autonegotiations that make the device unusable.
	*/
	writew(1, (void *)(ioaddr + PGSEL));
	writew(PMDCSR_VAL, (void *)(ioaddr + PMDCSR));
	writew(TSTDAT_VAL, (void *)(ioaddr + TSTDAT));
	writew(DSPCFG_VAL, (void *)(ioaddr + DSPCFG));
	writew(SDCFG_VAL, (void *)(ioaddr + SDCFG));
	writew(0, (void *)(ioaddr + PGSEL));
	np->dspcfg = DSPCFG_VAL;

	/* Enable PHY Specific event based interrupts.  Link state change
	   and Auto-Negotiation Completion are among the affected.
	   Read the intr status to clear it (needed for wake events).
	*/
	readw((void *)(ioaddr + MIntrStatus));
	writew(MICRIntEn, (void *)(ioaddr + MIntrCtrl));

	/* clear any interrupts that are pending, such as wake events */
	readl((void *)(ioaddr + IntrStatus));

	writel(np->ring_dma, (void *)(ioaddr + RxRingPtr));
	writel(np->ring_dma + RX_RING_SIZE * sizeof(struct netdev_desc),
		(void *)(ioaddr + TxRingPtr));

	/* Initialize other registers.
	 * Configure the PCI bus bursts and FIFO thresholds.
	 * Configure for standard, in-spec Ethernet.
	 * Start with half-duplex. check_link will update
	 * to the correct settings.
	 */

	/* DRTH: 2: start tx if 64 bytes are in the fifo
	 * FLTH: 0x10: refill with next packet if 512 bytes are free
	 * MXDMA: 0: up to 256 byte bursts.
	 * 	MXDMA must be <= FLTH
	 * ECRETRY=1
	 * ATP=1
	 */
	np->tx_config = TxAutoPad | TxCollRetry | TxMxdma_256 | (0x1002);
	writel(np->tx_config, (void *)(ioaddr + TxConfig));

	/* DRTH 0x10: start copying to memory if 128 bytes are in the fifo
	 * MXDMA 0: up to 256 byte bursts
	 */
	np->rx_config = RxMxdma_256 | 0x20;
	writel(np->rx_config, (void *)(ioaddr + RxConfig));

	/* Disable PME:
	 * The PME bit is initialized from the EEPROM contents.
	 * PCI cards probably have PME disabled, but motherboard
	 * implementations may have PME set to enable WakeOnLan.
	 * With PME set the chip will scan incoming packets but
	 * nothing will be written to memory. */
	np->SavedClkRun = readl((void *)(ioaddr + ClkRun));
	writel(np->SavedClkRun & ~PMEEnable, (void *)(ioaddr + ClkRun));
	if (np->SavedClkRun & PMEStatus && netif_msg_wol(np)) {
		rtdm_printk(KERN_NOTICE "%s: Wake-up event %#08x\n",
			dev->name, readl((void *)(ioaddr + WOLCmd)));
	}

	check_link(dev);
	__set_rx_mode(dev);

	/* Enable interrupts by setting the interrupt mask. */
	writel(DEFAULT_INTR, (void *)(ioaddr + IntrMask));
	writel(1, (void *)(ioaddr + IntrEnable));

	writel(RxOn | TxOn, (void *)(ioaddr + ChipCmd));
	writel(StatsClear, (void *)(ioaddr + StatsCtrl)); /* Clear Stats */
}

/*
 * netdev_timer:
 * Purpose:
 * 1) check for link changes. Usually they are handled by the MII interrupt
 *    but it doesn't hurt to check twice.
 * 2) check for sudden death of the NIC:
 *    It seems that a reference set for this chip went out with incorrect info,
 *    and there exist boards that aren't quite right.  An unexpected voltage
 *    drop can cause the PHY to get itself in a weird state (basically reset).
 *    NOTE: this only seems to affect revC chips.
 * 3) check of death of the RX path due to OOM
 */
/*** RTnet ***/
#if 0
static void netdev_timer(unsigned long data)
{
	struct net_device *dev = (struct net_device *)data;
	struct netdev_private *np = dev->priv;
	int next_tick = 5*HZ;
	long ioaddr = dev->base_addr;		
	u16 dspcfg;

	if (netif_msg_timer(np)) {
		/* DO NOT read the IntrStatus register,
		 * a read clears any pending interrupts.
		 */
		rtdm_printk(KERN_DEBUG "%s: Media selection timer tick.\n",
			dev->name);
	}

	spin_lock_irq(&np->lock);

	/* check for a nasty random phy-reset - use dspcfg as a flag */
	writew(1, ioaddr+PGSEL);
	dspcfg = readw(ioaddr+DSPCFG);
	writew(0, ioaddr+PGSEL);
	if (dspcfg != np->dspcfg) {
		if (!netif_queue_stopped(dev)) {
			spin_unlock_irq(&np->lock);
			if (netif_msg_hw(np))
				rtdm_printk(KERN_NOTICE "%s: possible phy reset: "
					"re-initializing\n", dev->name);
			disable_irq(dev->irq);
			spin_lock_irq(&np->lock);
			natsemi_stop_rxtx(dev);
			dump_ring(dev);
			reinit_ring(dev);
			init_registers(dev);
			spin_unlock_irq(&np->lock);
			enable_irq(dev->irq);
		} else {
			/* hurry back */
			next_tick = HZ;
			spin_unlock_irq(&np->lock);
		}
	} else {
		/* init_registers() calls check_link() for the above case */
		check_link(dev);
		spin_unlock_irq(&np->lock);
	}
	if (np->oom) {
		disable_irq(dev->irq);
		np->oom = 0;
		refill_rx(dev);
		enable_irq(dev->irq);
		if (!np->oom) {
			writel(RxOn, dev->base_addr + ChipCmd);
		} else {
			next_tick = 1;
		}
	}
	mod_timer(&np->timer, jiffies + next_tick);
}
#endif
/*** RTnet ***/

static void dump_ring(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;

	if (netif_msg_pktdata(np)) {
		int i;
		rtdm_printk(KERN_DEBUG "  Tx ring at %p:\n", np->tx_ring);
		for (i = 0; i < TX_RING_SIZE; i++) {
			rtdm_printk(KERN_DEBUG " #%d desc. %#08x %#08x %#08x.\n",
				i, np->tx_ring[i].next_desc,
				np->tx_ring[i].cmd_status,
				np->tx_ring[i].addr);
		}
		rtdm_printk(KERN_DEBUG "  Rx ring %p:\n", np->rx_ring);
		for (i = 0; i < RX_RING_SIZE; i++) {
			rtdm_printk(KERN_DEBUG " #%d desc. %#08x %#08x %#08x.\n",
				i, np->rx_ring[i].next_desc,
				np->rx_ring[i].cmd_status,
				np->rx_ring[i].addr);
		}
	}
}

/*** RTnet ***/
#if 0
static void tx_timeout(struct net_device *dev)
{
	struct netdev_private *np = dev->priv;
	long ioaddr = dev->base_addr;

	disable_irq(dev->irq);
	spin_lock_irq(&np->lock);
	if (!np->hands_off) {
		if (netif_msg_tx_err(np))
			rtdm_printk(KERN_WARNING
				"%s: Transmit timed out, status %#08x,"
				" resetting...\n",
				dev->name, readl(ioaddr + IntrStatus));
		dump_ring(dev);

		natsemi_reset(dev);
		reinit_ring(dev);
		init_registers(dev);
	} else {
		rtdm_printk(KERN_WARNING
			"%s: tx_timeout while in hands_off state?\n",
			dev->name);
	}
	spin_unlock_irq(&np->lock);
	enable_irq(dev->irq);

	dev->trans_start = jiffies;
	np->stats.tx_errors++;
	netif_wake_queue(dev);
}
#endif
/*** RTnet ***/

static int alloc_ring(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	np->rx_ring = pci_alloc_consistent(np->pci_dev,
		sizeof(struct netdev_desc) * (RX_RING_SIZE+TX_RING_SIZE),
		&np->ring_dma);
	if (!np->rx_ring)
		return -ENOMEM;
	np->tx_ring = &np->rx_ring[RX_RING_SIZE];
	return 0;
}

static void refill_rx(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;

	/* Refill the Rx ring buffers. */
	for (; np->cur_rx - np->dirty_rx > 0; np->dirty_rx++) {
		struct rtskb *skb;
		int entry = np->dirty_rx % RX_RING_SIZE;
		if (np->rx_skbuff[entry] == NULL) {
			skb = dev_alloc_rtskb(np->rx_buf_sz, &np->skb_pool);
			np->rx_skbuff[entry] = skb;
			if (skb == NULL)
				break; /* Better luck next round. */
			skb->rtdev = dev; /* Mark as being used by this device. */
			np->rx_dma[entry] = pci_map_single(np->pci_dev,
				skb->data, np->rx_buf_sz, PCI_DMA_FROMDEVICE);
			np->rx_ring[entry].addr = cpu_to_le32(np->rx_dma[entry]);
		}
		np->rx_ring[entry].cmd_status = cpu_to_le32(np->rx_buf_sz);
	}
	if (np->cur_rx - np->dirty_rx == RX_RING_SIZE) {
		if (netif_msg_rx_err(np))
			rtdm_printk(KERN_WARNING "%s: going OOM.\n", dev->name);
		np->oom = 1;
	}
}

/* Initialize the Rx and Tx rings, along with various 'dev' bits. */
static void init_ring(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	int i;

	/* 1) TX ring */
	np->dirty_tx = np->cur_tx = 0;
	for (i = 0; i < TX_RING_SIZE; i++) {
		np->tx_skbuff[i] = NULL;
		np->tx_ring[i].next_desc = cpu_to_le32(np->ring_dma
			+sizeof(struct netdev_desc)
			*((i+1)%TX_RING_SIZE+RX_RING_SIZE));
		np->tx_ring[i].cmd_status = 0;
	}

	/* 2) RX ring */
	np->dirty_rx = 0;
	np->cur_rx = RX_RING_SIZE;
	np->rx_buf_sz = (dev->mtu <= 1500 ? PKT_BUF_SZ : dev->mtu + 32);
	np->oom = 0;
	np->rx_head_desc = &np->rx_ring[0];

	/* Please be carefull before changing this loop - at least gcc-2.95.1
	 * miscompiles it otherwise.
	 */
	/* Initialize all Rx descriptors. */
	for (i = 0; i < RX_RING_SIZE; i++) {
		np->rx_ring[i].next_desc = cpu_to_le32(np->ring_dma
				+sizeof(struct netdev_desc)
				*((i+1)%RX_RING_SIZE));
		np->rx_ring[i].cmd_status = cpu_to_le32(DescOwn);
		np->rx_skbuff[i] = NULL;
	}
	refill_rx(dev);
	dump_ring(dev);
}

static void drain_tx(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	int i;

	for (i = 0; i < TX_RING_SIZE; i++) {
		if (np->tx_skbuff[i]) {
			pci_unmap_single(np->pci_dev,
				np->rx_dma[i], np->tx_skbuff[i]->len,
				PCI_DMA_TODEVICE);
			dev_kfree_rtskb(np->tx_skbuff[i]);
			np->stats.tx_dropped++;
		}
		np->tx_skbuff[i] = NULL;
	}
}

static void drain_ring(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	int i;

	/* Free all the skbuffs in the Rx queue. */
	for (i = 0; i < RX_RING_SIZE; i++) {
		np->rx_ring[i].cmd_status = 0;
		np->rx_ring[i].addr = 0xBADF00D0; /* An invalid address. */
		if (np->rx_skbuff[i]) {
			pci_unmap_single(np->pci_dev,
				np->rx_dma[i], np->rx_skbuff[i]->len,
				PCI_DMA_FROMDEVICE);
			dev_kfree_rtskb(np->rx_skbuff[i]);
		}
		np->rx_skbuff[i] = NULL;
	}
	drain_tx(dev);
}

static void free_ring(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	pci_free_consistent(np->pci_dev,
		sizeof(struct netdev_desc) * (RX_RING_SIZE+TX_RING_SIZE),
		np->rx_ring, np->ring_dma);
}

#if 0
static void reinit_ring(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	int i;

	/* drain TX ring */
	drain_tx(dev);
	np->dirty_tx = np->cur_tx = 0;
	for (i=0;i<TX_RING_SIZE;i++)
		np->tx_ring[i].cmd_status = 0;

	/* RX Ring */
	np->dirty_rx = 0;
	np->cur_rx = RX_RING_SIZE;
	np->rx_head_desc = &np->rx_ring[0];
	/* Initialize all Rx descriptors. */
	for (i = 0; i < RX_RING_SIZE; i++)
		np->rx_ring[i].cmd_status = cpu_to_le32(DescOwn);

	refill_rx(dev);
}
#endif

static int start_tx(struct rtskb *skb, struct rtnet_device *dev) /*** RTnet ***/
{
	struct netdev_private *np = dev->priv;
	unsigned entry;
/*** RTnet ***/
	rtdm_lockctx_t context;
/*** RTnet ***/

	/* Note: Ordering is important here, set the field with the
	   "ownership" bit last, and only then increment cur_tx. */

	/* Calculate the next Tx descriptor entry. */
	entry = np->cur_tx % TX_RING_SIZE;

	np->tx_skbuff[entry] = skb;
	np->tx_dma[entry] = pci_map_single(np->pci_dev,
				skb->data,skb->len, PCI_DMA_TODEVICE);

	np->tx_ring[entry].addr = cpu_to_le32(np->tx_dma[entry]);

/*	spin_lock_irq(&np->lock);*/
/*** RTnet ***/
	rtdm_lock_get_irqsave(&np->lock, context);
/*** RTnet ***/

	if (!np->hands_off) {
		/* get and patch time stamp just before the transmission */
		if (skb->xmit_stamp)
			*skb->xmit_stamp = cpu_to_be64(rtdm_clock_read() +
				*skb->xmit_stamp);
		np->tx_ring[entry].cmd_status = cpu_to_le32(DescOwn | skb->len);
		/* StrongARM: Explicitly cache flush np->tx_ring and
		 * skb->data,skb->len. */
		wmb();
		np->cur_tx++;
		if (np->cur_tx - np->dirty_tx >= TX_QUEUE_LEN - 1) {
			netdev_tx_done(dev);
			if (np->cur_tx - np->dirty_tx >= TX_QUEUE_LEN - 1)
				rtnetif_stop_queue(dev);
		}
		/* Wake the potentially-idle transmit channel. */
		writel(TxOn, (void *)(dev->base_addr + ChipCmd));
	} else {
		dev_kfree_rtskb(skb); /*** RTnet ***/
		np->stats.tx_dropped++;
	}

/*	spin_unlock_irq(&np->lock);*/
/*** RTnet ***/
	rtdm_lock_put_irqrestore(&np->lock, context);
/*** RTnet ***/

/*	dev->trans_start = jiffies;*/

	if (netif_msg_tx_queued(np)) {
		rtdm_printk(KERN_DEBUG "%s: Transmit frame #%d queued in slot %d.\n",
			dev->name, np->cur_tx, entry);
	}
	return 0;
}

static void netdev_tx_done(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;

	for (; np->cur_tx - np->dirty_tx > 0; np->dirty_tx++) {
		int entry = np->dirty_tx % TX_RING_SIZE;
		if (np->tx_ring[entry].cmd_status & cpu_to_le32(DescOwn))
			break;
		if (netif_msg_tx_done(np))
			rtdm_printk(KERN_DEBUG
				"%s: tx frame #%d finished, status %#08x.\n",
					dev->name, np->dirty_tx,
					le32_to_cpu(np->tx_ring[entry].cmd_status));
		if (np->tx_ring[entry].cmd_status & cpu_to_le32(DescPktOK)) {
			np->stats.tx_packets++;
			np->stats.tx_bytes += np->tx_skbuff[entry]->len;
		} else { /* Various Tx errors */
			int tx_status =
				le32_to_cpu(np->tx_ring[entry].cmd_status);
			if (tx_status & (DescTxAbort|DescTxExcColl))
				np->stats.tx_aborted_errors++;
			if (tx_status & DescTxFIFO)
				np->stats.tx_fifo_errors++;
			if (tx_status & DescTxCarrier)
				np->stats.tx_carrier_errors++;
			if (tx_status & DescTxOOWCol)
				np->stats.tx_window_errors++;
			np->stats.tx_errors++;
		}
		pci_unmap_single(np->pci_dev,np->tx_dma[entry],
					np->tx_skbuff[entry]->len,
					PCI_DMA_TODEVICE);
		/* Free the original skb. */
		dev_kfree_rtskb(np->tx_skbuff[entry]); /*** RTnet ***/
/*		dev_kfree_skb_irq(np->tx_skbuff[entry]);*/
		np->tx_skbuff[entry] = NULL;
	}
	if (rtnetif_queue_stopped(dev)
		&& np->cur_tx - np->dirty_tx < TX_QUEUE_LEN - 4) {
		/* The ring is no longer full, wake queue. */
		rtnetif_wake_queue(dev);
	}
}

/* The interrupt handler does all of the Rx thread work and cleans up
   after the Tx thread. */
static int intr_handler(rtdm_irq_t *irq_handle)
{
	nanosecs_abs_t time_stamp = rtdm_clock_read(); /*** RTnet ***/
	struct rtnet_device *dev =
	    rtdm_irq_get_arg(irq_handle, struct rtnet_device); /*** RTnet ***/
	struct netdev_private *np = dev->priv;
	unsigned int old_packet_cnt = np->stats.rx_packets; /*** RTnet ***/
	long ioaddr = dev->base_addr;
	int boguscnt = max_interrupt_work;

	if (np->hands_off)
		return RTDM_IRQ_NONE;
	do {
		/* Reading automatically acknowledges all int sources. */
		u32 intr_status = readl((void *)(ioaddr + IntrStatus));

		if (netif_msg_intr(np))
			rtdm_printk(KERN_DEBUG
				"%s: Interrupt, status %#08x, mask %#08x.\n",
				dev->name, intr_status,
				readl((void *)(ioaddr + IntrMask)));

		if (intr_status == 0)
			break;

		if (intr_status &
		   (IntrRxDone | IntrRxIntr | RxStatusFIFOOver |
		    IntrRxErr | IntrRxOverrun)) {
			netdev_rx(dev, &time_stamp);
		}

		if (intr_status &
		   (IntrTxDone | IntrTxIntr | IntrTxIdle | IntrTxErr)) {
			rtdm_lock_get(&np->lock);
			netdev_tx_done(dev);
			rtdm_lock_put(&np->lock);
		}

		/* Abnormal error summary/uncommon events handlers. */
		if (intr_status & IntrAbnormalSummary)
			netdev_error(dev, intr_status);

		if (--boguscnt < 0) {
			if (netif_msg_intr(np))
				rtdm_printk(KERN_WARNING
					"%s: Too much work at interrupt, "
					"status=%#08x.\n",
					dev->name, intr_status);
			break;
		}
	} while (1);

	if (netif_msg_intr(np))
		rtdm_printk(KERN_DEBUG "%s: exiting interrupt.\n", dev->name);

/*** RTnet ***/
	if (old_packet_cnt != np->stats.rx_packets)
		rt_mark_stack_mgr(dev);
	return RTDM_IRQ_HANDLED;
}

/* This routine is logically part of the interrupt handler, but separated
   for clarity and better register allocation. */
static void netdev_rx(struct rtnet_device *dev, nanosecs_abs_t *time_stamp)
{
	struct netdev_private *np = dev->priv;
	int entry = np->cur_rx % RX_RING_SIZE;
	int boguscnt = np->dirty_rx + RX_RING_SIZE - np->cur_rx;
	s32 desc_status = le32_to_cpu(np->rx_head_desc->cmd_status);

	/* If the driver owns the next entry it's a new packet. Send it up. */
	while (desc_status < 0) { /* e.g. & DescOwn */
		if (netif_msg_rx_status(np))
			rtdm_printk(KERN_DEBUG
				"  netdev_rx() entry %d status was %#08x.\n",
				entry, desc_status);
		if (--boguscnt < 0)
			break;
		if ((desc_status&(DescMore|DescPktOK|DescRxLong)) != DescPktOK){
			if (desc_status & DescMore) {
				if (netif_msg_rx_err(np))
					rtdm_printk(KERN_WARNING
						"%s: Oversized(?) Ethernet "
						"frame spanned multiple "
						"buffers, entry %#08x "
						"status %#08x.\n", dev->name,
						np->cur_rx, desc_status);
				np->stats.rx_length_errors++;
			} else {
				/* There was an error. */
				np->stats.rx_errors++;
				if (desc_status & (DescRxAbort|DescRxOver))
					np->stats.rx_over_errors++;
				if (desc_status & (DescRxLong|DescRxRunt))
					np->stats.rx_length_errors++;
				if (desc_status & (DescRxInvalid|DescRxAlign))
					np->stats.rx_frame_errors++;
				if (desc_status & DescRxCRC)
					np->stats.rx_crc_errors++;
			}
		} else {
			struct rtskb *skb;
			/* Omit CRC size. */
			int pkt_len = (desc_status & DescSizeMask) - 4;
			/* Check if the packet is long enough to accept
			 * without copying to a minimally-sized skbuff. */
/*** RTnet ***/
#if 0
			if (pkt_len < rx_copybreak
			    && (skb = dev_alloc_skb(pkt_len + 2)) != NULL) {
				skb->dev = dev;
				/* 16 byte align the IP header */
				skb_reserve(skb, 2);
				pci_dma_sync_single(np->pci_dev,
					np->rx_dma[entry],
					np->rx_skbuff[entry]->len,
					PCI_DMA_FROMDEVICE);
#if HAS_IP_COPYSUM
				eth_copy_and_sum(skb,
					np->rx_skbuff[entry]->tail, pkt_len, 0);
				skb_put(skb, pkt_len);
#else
				memcpy(skb_put(skb, pkt_len),
					np->rx_skbuff[entry]->tail, pkt_len);
#endif
			} else {
#endif
			{
				skb = np->rx_skbuff[entry];
				pci_unmap_single(np->pci_dev, np->rx_dma[entry],
					np->rx_skbuff[entry]->len,
					PCI_DMA_FROMDEVICE);
				rtskb_put(skb, pkt_len);
				np->rx_skbuff[entry] = NULL;
			}
/*** RTnet ***/
			skb->protocol = rt_eth_type_trans(skb, dev);
			skb->time_stamp = *time_stamp;
			rtnetif_rx(skb);
			/*dev->last_rx = jiffies;*/
/*** RTnet ***/
			np->stats.rx_packets++;
			np->stats.rx_bytes += pkt_len;
		}
		entry = (++np->cur_rx) % RX_RING_SIZE;
		np->rx_head_desc = &np->rx_ring[entry];
		desc_status = le32_to_cpu(np->rx_head_desc->cmd_status);
	}
	refill_rx(dev);

	/* Restart Rx engine if stopped. */
	if (np->oom)
		;
/*		mod_timer(&np->timer, jiffies + 1);*/
	else
		writel(RxOn, (void *)(dev->base_addr + ChipCmd));
}

static void netdev_error(struct rtnet_device *dev, int intr_status)
{
	struct netdev_private *np = dev->priv;
	long ioaddr = dev->base_addr;

	rtdm_lock_get(&np->lock);
	if (intr_status & LinkChange) {
		u16 adv = mdio_read(dev, 1, MII_ADVERTISE);
		u16 lpa = mdio_read(dev, 1, MII_LPA);
		if (mdio_read(dev, 1, MII_BMCR) & BMCR_ANENABLE
		 && netif_msg_link(np)) {
			rtdm_printk(KERN_INFO
				"%s: Autonegotiation advertising"
				" %#04x  partner %#04x.\n", dev->name,
				adv, lpa);
		}

		/* read MII int status to clear the flag */
		readw((void *)(ioaddr + MIntrStatus));
		check_link(dev);
	}
	if (intr_status & StatsMax) {
		__get_stats(dev);
	}
	if (intr_status & IntrTxUnderrun) {
		if ((np->tx_config & TxDrthMask) < 62)
			np->tx_config += 2;
		if (netif_msg_tx_err(np))
			rtdm_printk(KERN_NOTICE
				"%s: increased Tx threshold, txcfg %#08x.\n",
				dev->name, np->tx_config);
		writel(np->tx_config, (void *)(ioaddr + TxConfig));
	}
	if (intr_status & WOLPkt && netif_msg_wol(np)) {
		int wol_status = readl((void *)(ioaddr + WOLCmd));
		rtdm_printk(KERN_NOTICE "%s: Link wake-up event %#08x\n",
			dev->name, wol_status);
	}
	if (intr_status & RxStatusFIFOOver) {
		if (netif_msg_rx_err(np) && netif_msg_intr(np)) {
			rtdm_printk(KERN_NOTICE "%s: Rx status FIFO overrun\n",
				dev->name);
		}
		np->stats.rx_fifo_errors++;
	}
	/* Hmmmmm, it's not clear how to recover from PCI faults. */
	if (intr_status & IntrPCIErr) {
		rtdm_printk(KERN_NOTICE "%s: PCI error %#08x\n", dev->name,
			intr_status & IntrPCIErr);
		np->stats.tx_fifo_errors++;
		np->stats.rx_fifo_errors++;
	}
	rtdm_lock_put(&np->lock);
}

static void __get_stats(struct rtnet_device *dev)
{
	long ioaddr = dev->base_addr;
	struct netdev_private *np = dev->priv;

	/* The chip only need report frame silently dropped. */
	np->stats.rx_crc_errors	+= readl((void *)(ioaddr + RxCRCErrs));
	np->stats.rx_missed_errors += readl((void *)(ioaddr + RxMissed));
}

/*** RTnet ***/
#if 0
static struct net_device_stats *get_stats(struct net_device *dev)
{
	struct netdev_private *np = dev->priv;

	/* The chip only need report frame silently dropped. */
	spin_lock_irq(&np->lock);
	if (netif_running(dev) && !np->hands_off)
		__get_stats(dev);
	spin_unlock_irq(&np->lock);

	return &np->stats;
}
#endif
/*** RTnet ***/

/**
 * dp83815_crc - computer CRC for hash table entries
 *
 * Note - this is, for some reason, *not* the same function
 * as ether_crc_le() or ether_crc(), though it uses the
 * same big-endian polynomial.
 */
#define DP_POLYNOMIAL			0x04C11DB7
static unsigned dp83815_crc(int length, unsigned char *data)
{
	u32 crc;
	u8 cur_byte;
	u8 msb;
	u8 byte, bit;

	crc = ~0;
	for (byte=0; byte<length; byte++) {
		cur_byte = *data++;
		for (bit=0; bit<8; bit++) {
			msb = crc >> 31;
			crc <<= 1;
			if (msb ^ (cur_byte & 1)) {
				crc ^= DP_POLYNOMIAL;
				crc |= 1;
			}
			cur_byte >>= 1;
		}
	}
	crc >>= 23;

	return (crc);
}


void set_bit_le(int offset, unsigned char * data)
{
	data[offset >> 3] |= (1 << (offset & 0x07));
}
#define HASH_TABLE	0x200
static void __set_rx_mode(struct rtnet_device *dev)
{
	long ioaddr = dev->base_addr;
	struct netdev_private *np = dev->priv;
	u8 mc_filter[64]; /* Multicast hash filter */
	u32 rx_mode;

	if (dev->flags & IFF_PROMISC) { /* Set promiscuous. */
		/* Unconditionally log net taps. */
		rtdm_printk(KERN_NOTICE "%s: Promiscuous mode enabled.\n",
			dev->name);
		rx_mode = RxFilterEnable | AcceptBroadcast
			| AcceptAllMulticast | AcceptAllPhys | AcceptMyPhys;
	} else if ((dev->mc_count > multicast_filter_limit)
	  || (dev->flags & IFF_ALLMULTI)) {
		rx_mode = RxFilterEnable | AcceptBroadcast
			| AcceptAllMulticast | AcceptMyPhys;
	} else {
		struct dev_mc_list *mclist;
		int i;
		memset(mc_filter, 0, sizeof(mc_filter));
		for (i = 0, mclist = dev->mc_list; mclist && i < dev->mc_count;
			 i++, mclist = mclist->next) {
			set_bit_le(
				dp83815_crc(ETH_ALEN, mclist->dmi_addr) & 0x1ff,
				mc_filter);
		}
		rx_mode = RxFilterEnable | AcceptBroadcast
			| AcceptMulticast | AcceptMyPhys;
		for (i = 0; i < 64; i += 2) {
			writew(HASH_TABLE + i, (void *)(ioaddr + RxFilterAddr));
			writew((mc_filter[i+1]<<8) + mc_filter[i],
				(void *)(ioaddr + RxFilterData));
		}
	}
	writel(rx_mode, (void *)(ioaddr + RxFilterAddr));
	np->cur_rx_mode = rx_mode;
}
/*** RTnet
static void set_rx_mode(struct rtnet_device *dev)
{
	struct netdev_private *np = dev->priv;
	spin_lock_irq(&np->lock);
	if (!np->hands_off)
		__set_rx_mode(dev);
	spin_unlock_irq(&np->lock);
}
RTnet ***/
/*** RTnet ***/
#if 0
static int netdev_ethtool_ioctl(struct net_device *dev, void *useraddr)
{
	struct netdev_private *np = dev->priv;
	u32 cmd;

	if (get_user(cmd, (u32 *)useraddr))
		return -EFAULT;

	switch (cmd) {
	/* get driver info */
	case ETHTOOL_GDRVINFO: {
		struct ethtool_drvinfo info = {ETHTOOL_GDRVINFO};
		strncpy(info.driver, DRV_NAME, ETHTOOL_BUSINFO_LEN);
		strncpy(info.version, DRV_VERSION, ETHTOOL_BUSINFO_LEN);
		info.fw_version[0] = '\0';
		strncpy(info.bus_info, np->pci_dev->slot_name,
			ETHTOOL_BUSINFO_LEN);
		info.eedump_len = NATSEMI_EEPROM_SIZE;
		info.regdump_len = NATSEMI_REGS_SIZE;
		if (copy_to_user(useraddr, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	/* get settings */
	case ETHTOOL_GSET: {
		struct ethtool_cmd ecmd = { ETHTOOL_GSET };
		spin_lock_irq(&np->lock);
		netdev_get_ecmd(dev, &ecmd);
		spin_unlock_irq(&np->lock);
		if (copy_to_user(useraddr, &ecmd, sizeof(ecmd)))
			return -EFAULT;
		return 0;
	}
	/* set settings */
	case ETHTOOL_SSET: {
		struct ethtool_cmd ecmd;
		int r;
		if (copy_from_user(&ecmd, useraddr, sizeof(ecmd)))
			return -EFAULT;
		spin_lock_irq(&np->lock);
		r = netdev_set_ecmd(dev, &ecmd);
		spin_unlock_irq(&np->lock);
		return r;
	}
	/* get wake-on-lan */
	case ETHTOOL_GWOL: {
		struct ethtool_wolinfo wol = {ETHTOOL_GWOL};
		spin_lock_irq(&np->lock);
		netdev_get_wol(dev, &wol.supported, &wol.wolopts);
		netdev_get_sopass(dev, wol.sopass);
		spin_unlock_irq(&np->lock);
		if (copy_to_user(useraddr, &wol, sizeof(wol)))
			return -EFAULT;
		return 0;
	}
	/* set wake-on-lan */
	case ETHTOOL_SWOL: {
		struct ethtool_wolinfo wol;
		int r;
		if (copy_from_user(&wol, useraddr, sizeof(wol)))
			return -EFAULT;
		spin_lock_irq(&np->lock);
		netdev_set_wol(dev, wol.wolopts);
		r = netdev_set_sopass(dev, wol.sopass);
		spin_unlock_irq(&np->lock);
		return r;
	}
	/* get registers */
	case ETHTOOL_GREGS: {
		struct ethtool_regs regs;
		u8 regbuf[NATSEMI_REGS_SIZE];
		int r;

		if (copy_from_user(&regs, useraddr, sizeof(regs)))
			return -EFAULT;

		if (regs.len > NATSEMI_REGS_SIZE) {
			regs.len = NATSEMI_REGS_SIZE;
		}
		regs.version = NATSEMI_REGS_VER;
		if (copy_to_user(useraddr, &regs, sizeof(regs)))
			return -EFAULT;

		useraddr += offsetof(struct ethtool_regs, data);

		spin_lock_irq(&np->lock);
		r = netdev_get_regs(dev, regbuf);
		spin_unlock_irq(&np->lock);

		if (r)
			return r;
		if (copy_to_user(useraddr, regbuf, regs.len))
			return -EFAULT;
		return 0;
	}
	/* get message-level */
	case ETHTOOL_GMSGLVL: {
		struct ethtool_value edata = {ETHTOOL_GMSGLVL};
		edata.data = np->msg_enable;
		if (copy_to_user(useraddr, &edata, sizeof(edata)))
			return -EFAULT;
		return 0;
	}
	/* set message-level */
	case ETHTOOL_SMSGLVL: {
		struct ethtool_value edata;
		if (copy_from_user(&edata, useraddr, sizeof(edata)))
			return -EFAULT;
		np->msg_enable = edata.data;
		return 0;
	}
	/* restart autonegotiation */
	case ETHTOOL_NWAY_RST: {
		int tmp;
		int r = -EINVAL;
		/* if autoneg is off, it's an error */
		tmp = mdio_read(dev, 1, MII_BMCR);
		if (tmp & BMCR_ANENABLE) {
			tmp |= (BMCR_ANRESTART);
			mdio_write(dev, 1, MII_BMCR, tmp);
			r = 0;
		}
		return r;
	}
	/* get link status */
	case ETHTOOL_GLINK: {
		struct ethtool_value edata = {ETHTOOL_GLINK};
		/* LSTATUS is latched low until a read - so read twice */
		mdio_read(dev, 1, MII_BMSR);
		edata.data = (mdio_read(dev, 1, MII_BMSR)&BMSR_LSTATUS) ? 1:0;
		if (copy_to_user(useraddr, &edata, sizeof(edata)))
			return -EFAULT;
		return 0;
	}
	/* get EEPROM */
	case ETHTOOL_GEEPROM: {
		struct ethtool_eeprom eeprom;
		u8 eebuf[NATSEMI_EEPROM_SIZE];
		int r;

		if (copy_from_user(&eeprom, useraddr, sizeof(eeprom)))
			return -EFAULT;

		if (eeprom.offset > eeprom.offset+eeprom.len)
			return -EINVAL;

		if ((eeprom.offset+eeprom.len) > NATSEMI_EEPROM_SIZE) {
			eeprom.len = NATSEMI_EEPROM_SIZE-eeprom.offset;
		}
		eeprom.magic = PCI_VENDOR_ID_NS | (PCI_DEVICE_ID_NS_83815<<16);
		if (copy_to_user(useraddr, &eeprom, sizeof(eeprom)))
			return -EFAULT;

		useraddr += offsetof(struct ethtool_eeprom, data);

		spin_lock_irq(&np->lock);
		r = netdev_get_eeprom(dev, eebuf);
		spin_unlock_irq(&np->lock);

		if (r)
			return r;
		if (copy_to_user(useraddr, eebuf+eeprom.offset, eeprom.len))
			return -EFAULT;
		return 0;
	}

	}

	return -EOPNOTSUPP;
}

static int netdev_set_wol(struct rtnet_device *dev, u32 newval)
{
	struct netdev_private *np = dev->priv;
	u32 data = readl(dev->base_addr + WOLCmd) & ~WakeOptsSummary;

	/* translate to bitmasks this chip understands */
	if (newval & WAKE_PHY)
		data |= WakePhy;
	if (newval & WAKE_UCAST)
		data |= WakeUnicast;
	if (newval & WAKE_MCAST)
		data |= WakeMulticast;
	if (newval & WAKE_BCAST)
		data |= WakeBroadcast;
	if (newval & WAKE_ARP)
		data |= WakeArp;
	if (newval & WAKE_MAGIC)
		data |= WakeMagic;
	if (np->srr >= SRR_DP83815_D) {
		if (newval & WAKE_MAGICSECURE) {
			data |= WakeMagicSecure;
		}
	}

	writel(data, dev->base_addr + WOLCmd);

	return 0;
}

static int netdev_get_wol(struct rtnet_device *dev, u32 *supported, u32 *cur)
{
	struct netdev_private *np = dev->priv;
	u32 regval = readl(dev->base_addr + WOLCmd);

	*supported = (WAKE_PHY | WAKE_UCAST | WAKE_MCAST | WAKE_BCAST
			| WAKE_ARP | WAKE_MAGIC);

	if (np->srr >= SRR_DP83815_D) {
		/* SOPASS works on revD and higher */
		*supported |= WAKE_MAGICSECURE;
	}
	*cur = 0;

	/* translate from chip bitmasks */
	if (regval & WakePhy)
		*cur |= WAKE_PHY;
	if (regval & WakeUnicast)
		*cur |= WAKE_UCAST;
	if (regval & WakeMulticast)
		*cur |= WAKE_MCAST;
	if (regval & WakeBroadcast)
		*cur |= WAKE_BCAST;
	if (regval & WakeArp)
		*cur |= WAKE_ARP;
	if (regval & WakeMagic)
		*cur |= WAKE_MAGIC;
	if (regval & WakeMagicSecure) {
		/* this can be on in revC, but it's broken */
		*cur |= WAKE_MAGICSECURE;
	}

	return 0;
}

static int netdev_set_sopass(struct rtnet_device *dev, u8 *newval)
{
	struct netdev_private *np = dev->priv;
	u16 *sval = (u16 *)newval;
	u32 addr;

	if (np->srr < SRR_DP83815_D) {
		return 0;
	}

	/* enable writing to these registers by disabling the RX filter */
	addr = readl(dev->base_addr + RxFilterAddr) & ~RFCRAddressMask;
	addr &= ~RxFilterEnable;
	writel(addr, dev->base_addr + RxFilterAddr);

	/* write the three words to (undocumented) RFCR vals 0xa, 0xc, 0xe */
	writel(addr | 0xa, dev->base_addr + RxFilterAddr);
	writew(sval[0], dev->base_addr + RxFilterData);

	writel(addr | 0xc, dev->base_addr + RxFilterAddr);
	writew(sval[1], dev->base_addr + RxFilterData);

	writel(addr | 0xe, dev->base_addr + RxFilterAddr);
	writew(sval[2], dev->base_addr + RxFilterData);

	/* re-enable the RX filter */
	writel(addr | RxFilterEnable, dev->base_addr + RxFilterAddr);

	return 0;
}

static int netdev_get_sopass(struct rtnet_device *dev, u8 *data)
{
	struct netdev_private *np = dev->priv;
	u16 *sval = (u16 *)data;
	u32 addr;

	if (np->srr < SRR_DP83815_D) {
		sval[0] = sval[1] = sval[2] = 0;
		return 0;
	}

	/* read the three words from (undocumented) RFCR vals 0xa, 0xc, 0xe */
	addr = readl(dev->base_addr + RxFilterAddr) & ~RFCRAddressMask;

	writel(addr | 0xa, dev->base_addr + RxFilterAddr);
	sval[0] = readw(dev->base_addr + RxFilterData);

	writel(addr | 0xc, dev->base_addr + RxFilterAddr);
	sval[1] = readw(dev->base_addr + RxFilterData);

	writel(addr | 0xe, dev->base_addr + RxFilterAddr);
	sval[2] = readw(dev->base_addr + RxFilterData);

	writel(addr, dev->base_addr + RxFilterAddr);

	return 0;
}

static int netdev_get_ecmd(struct rtnet_device *dev, struct ethtool_cmd *ecmd)
{
	u32 tmp;

	ecmd->supported =
		(SUPPORTED_10baseT_Half | SUPPORTED_10baseT_Full |
		SUPPORTED_100baseT_Half | SUPPORTED_100baseT_Full |
		SUPPORTED_Autoneg | SUPPORTED_TP | SUPPORTED_MII);

	/* only supports twisted-pair or MII */
	tmp = readl(dev->base_addr + ChipConfig);
	if (tmp & CfgExtPhy)
		ecmd->port = PORT_MII;
	else
		ecmd->port = PORT_TP;

	/* only supports internal transceiver */
	ecmd->transceiver = XCVR_INTERNAL;

	/* not sure what this is for */
	ecmd->phy_address = readw(dev->base_addr + PhyCtrl) & PhyAddrMask;

	ecmd->advertising = ADVERTISED_TP | ADVERTISED_MII;
	tmp = mdio_read(dev, 1, MII_ADVERTISE);
	if (tmp & ADVERTISE_10HALF)
		ecmd->advertising |= ADVERTISED_10baseT_Half;
	if (tmp & ADVERTISE_10FULL)
		ecmd->advertising |= ADVERTISED_10baseT_Full;
	if (tmp & ADVERTISE_100HALF)
		ecmd->advertising |= ADVERTISED_100baseT_Half;
	if (tmp & ADVERTISE_100FULL)
		ecmd->advertising |= ADVERTISED_100baseT_Full;

	tmp = mdio_read(dev, 1, MII_BMCR);
	if (tmp & BMCR_ANENABLE) {
		ecmd->advertising |= ADVERTISED_Autoneg;
		ecmd->autoneg = AUTONEG_ENABLE;
	} else {
		ecmd->autoneg = AUTONEG_DISABLE;
	}

	tmp = readl(dev->base_addr + ChipConfig);
	if (tmp & CfgSpeed100) {
		ecmd->speed = SPEED_100;
	} else {
		ecmd->speed = SPEED_10;
	}

	if (tmp & CfgFullDuplex) {
		ecmd->duplex = DUPLEX_FULL;
	} else {
		ecmd->duplex = DUPLEX_HALF;
	}

	/* ignore maxtxpkt, maxrxpkt for now */

	return 0;
}

static int netdev_set_ecmd(struct rtnet_device *dev, struct ethtool_cmd *ecmd)
{
	struct netdev_private *np = dev->priv;
	u32 tmp;

	if (ecmd->speed != SPEED_10 && ecmd->speed != SPEED_100)
		return -EINVAL;
	if (ecmd->duplex != DUPLEX_HALF && ecmd->duplex != DUPLEX_FULL)
		return -EINVAL;
	if (ecmd->port != PORT_TP && ecmd->port != PORT_MII)
		return -EINVAL;
	if (ecmd->transceiver != XCVR_INTERNAL)
		return -EINVAL;
	if (ecmd->autoneg != AUTONEG_DISABLE && ecmd->autoneg != AUTONEG_ENABLE)
		return -EINVAL;
	/* ignore phy_address, maxtxpkt, maxrxpkt for now */

	/* WHEW! now lets bang some bits */

	tmp = mdio_read(dev, 1, MII_BMCR);
	if (ecmd->autoneg == AUTONEG_ENABLE) {
		/* turn on autonegotiation */
		tmp |= BMCR_ANENABLE;
		np->advertising = mdio_read(dev, 1, MII_ADVERTISE);
	} else {
		/* turn off auto negotiation, set speed and duplexity */
		tmp &= ~(BMCR_ANENABLE | BMCR_SPEED100 | BMCR_FULLDPLX);
		if (ecmd->speed == SPEED_100)
			tmp |= BMCR_SPEED100;
		if (ecmd->duplex == DUPLEX_FULL)
			tmp |= BMCR_FULLDPLX;
		else
			np->full_duplex = 0;
	}
	mdio_write(dev, 1, MII_BMCR, tmp);
	return 0;
}

static int netdev_get_regs(struct rtnet_device *dev, u8 *buf)
{
	int i;
	int j;
	u32 rfcr;
	u32 *rbuf = (u32 *)buf;

	/* read all of page 0 of registers */
	for (i = 0; i < NATSEMI_PG0_NREGS; i++) {
		rbuf[i] = readl(dev->base_addr + i*4);
	}

	/* read only the 'magic' registers from page 1 */
	writew(1, dev->base_addr + PGSEL);
	rbuf[i++] = readw(dev->base_addr + PMDCSR);
	rbuf[i++] = readw(dev->base_addr + TSTDAT);
	rbuf[i++] = readw(dev->base_addr + DSPCFG);
	rbuf[i++] = readw(dev->base_addr + SDCFG);
	writew(0, dev->base_addr + PGSEL);

	/* read RFCR indexed registers */
	rfcr = readl(dev->base_addr + RxFilterAddr);
	for (j = 0; j < NATSEMI_RFDR_NREGS; j++) {
		writel(j*2, dev->base_addr + RxFilterAddr);
		rbuf[i++] = readw(dev->base_addr + RxFilterData);
	}
	writel(rfcr, dev->base_addr + RxFilterAddr);

	/* the interrupt status is clear-on-read - see if we missed any */
	if (rbuf[4] & rbuf[5]) {
		rtdm_printk(KERN_WARNING
			"%s: shoot, we dropped an interrupt (%#08x)\n",
			dev->name, rbuf[4] & rbuf[5]);
	}

	return 0;
}

#define SWAP_BITS(x)	( (((x) & 0x0001) << 15) | (((x) & 0x0002) << 13) \
			| (((x) & 0x0004) << 11) | (((x) & 0x0008) << 9)  \
			| (((x) & 0x0010) << 7)  | (((x) & 0x0020) << 5)  \
			| (((x) & 0x0040) << 3)  | (((x) & 0x0080) << 1)  \
			| (((x) & 0x0100) >> 1)  | (((x) & 0x0200) >> 3)  \
			| (((x) & 0x0400) >> 5)  | (((x) & 0x0800) >> 7)  \
			| (((x) & 0x1000) >> 9)  | (((x) & 0x2000) >> 11) \
			| (((x) & 0x4000) >> 13) | (((x) & 0x8000) >> 15) )

static int netdev_get_eeprom(struct rtnet_device *dev, u8 *buf)
{
	int i;
	u16 *ebuf = (u16 *)buf;

	/* eeprom_read reads 16 bits, and indexes by 16 bits */
	for (i = 0; i < NATSEMI_EEPROM_SIZE/2; i++) {
		ebuf[i] = eeprom_read(dev->base_addr, i);
		/* The EEPROM itself stores data bit-swapped, but eeprom_read
		 * reads it back "sanely". So we swap it back here in order to
		 * present it to userland as it is stored. */
		ebuf[i] = SWAP_BITS(ebuf[i]);
	}
	return 0;
}

static int netdev_ioctl(struct rtnet_device *dev, struct ifreq *rq, int cmd)
{
	struct mii_ioctl_data *data = (struct mii_ioctl_data *)&rq->ifr_data;

	switch(cmd) {
	case SIOCETHTOOL:
		return netdev_ethtool_ioctl(dev, (void *) rq->ifr_data);
	case SIOCGMIIPHY:		/* Get address of MII PHY in use. */
	case SIOCDEVPRIVATE:		/* for binary compat, remove in 2.5 */
		data->phy_id = 1;
		/* Fall Through */

	case SIOCGMIIREG:		/* Read MII PHY register. */
	case SIOCDEVPRIVATE+1:		/* for binary compat, remove in 2.5 */
		data->val_out = mdio_read(dev, data->phy_id & 0x1f,
			data->reg_num & 0x1f);
		return 0;

	case SIOCSMIIREG:		/* Write MII PHY register. */
	case SIOCDEVPRIVATE+2:		/* for binary compat, remove in 2.5 */
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		mdio_write(dev, data->phy_id & 0x1f, data->reg_num & 0x1f,
			data->val_in);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
#endif
/*** RTnet ***/

static void enable_wol_mode(struct rtnet_device *dev, int enable_intr)
{
	long ioaddr = dev->base_addr;
	struct netdev_private *np = dev->priv;

	if (netif_msg_wol(np))
		rtdm_printk(KERN_INFO "%s: remaining active for wake-on-lan\n",
			dev->name);

	/* For WOL we must restart the rx process in silent mode.
	 * Write NULL to the RxRingPtr. Only possible if
	 * rx process is stopped
	 */
	writel(0, (void *)(ioaddr + RxRingPtr));

	/* read WoL status to clear */
	readl((void *)(ioaddr + WOLCmd));

	/* PME on, clear status */
	writel(np->SavedClkRun | PMEEnable | PMEStatus, (void *)(ioaddr + ClkRun));

	/* and restart the rx process */
	writel(RxOn, (void *)(ioaddr + ChipCmd));

	if (enable_intr) {
		/* enable the WOL interrupt.
		 * Could be used to send a netlink message.
		 */
		writel(WOLPkt | LinkChange, (void *)(ioaddr + IntrMask));
		writel(1, (void *)(ioaddr + IntrEnable));
	}
}

static int netdev_close(struct rtnet_device *dev)
{
	int i;
	long ioaddr = dev->base_addr;
	struct netdev_private *np = dev->priv;

	if (netif_msg_ifdown(np))
		rtdm_printk(KERN_DEBUG
			"%s: Shutting down ethercard, status was %#04x.\n",
			dev->name, (int)readl((void *)(ioaddr + ChipCmd)));
	if (netif_msg_pktdata(np))
		rtdm_printk(KERN_DEBUG
			"%s: Queue pointers were Tx %d / %d,  Rx %d / %d.\n",
			dev->name, np->cur_tx, np->dirty_tx,
			np->cur_rx, np->dirty_rx);

	/*
	 * FIXME: what if someone tries to close a device
	 * that is suspended?
	 * Should we reenable the nic to switch to
	 * the final WOL settings?
	 */
/*** RTnet ***
	del_timer_sync(&np->timer);
 *** RTnet ***/
/*	disable_irq(dev->irq);*/
	rtdm_irq_disable(&np->irq_handle);
	rtdm_lock_get(&np->lock);
	/* Disable interrupts, and flush posted writes */
	writel(0, (void *)(ioaddr + IntrEnable));
	readl((void *)(ioaddr + IntrEnable));
	np->hands_off = 1;
	rtdm_lock_put(&np->lock);

/*** RTnet ***/
	if ( (i=rtdm_irq_free(&np->irq_handle))<0 )
		return i;

	rt_stack_disconnect(dev);
/*** RTnet ***/

/*	enable_irq(dev->irq);*/

/*	free_irq(dev->irq, dev);*/

	/* Interrupt disabled, interrupt handler released,
	 * queue stopped, timer deleted, rtnl_lock held
	 * All async codepaths that access the driver are disabled.
	 */
	rtdm_lock_get(&np->lock);
	np->hands_off = 0;
	readl((void *)(ioaddr + IntrMask));
	readw((void *)(ioaddr + MIntrStatus));

	/* Freeze Stats */
	writel(StatsFreeze, (void *)(ioaddr + StatsCtrl));

	/* Stop the chip's Tx and Rx processes. */
	natsemi_stop_rxtx(dev);

	__get_stats(dev);
	rtdm_lock_put(&np->lock);

	/* clear the carrier last - an interrupt could reenable it otherwise */
	rtnetif_carrier_off(dev);
	rtnetif_stop_queue(dev);

	dump_ring(dev);
	drain_ring(dev);
	free_ring(dev);

	{
		u32 wol = readl((void *)(ioaddr + WOLCmd)) & WakeOptsSummary;
		if (wol) {
			/* restart the NIC in WOL mode.
			 * The nic must be stopped for this.
			 */
			enable_wol_mode(dev, 0);
		} else {
			/* Restore PME enable bit unmolested */
			writel(np->SavedClkRun, (void *)(ioaddr + ClkRun));
		}
	}

	RTNET_MOD_DEC_USE_COUNT;

	return 0;
}


static void __devexit natsemi_remove1 (struct pci_dev *pdev)
{

 /*** RTnet ***/
	struct rtnet_device *dev = pci_get_drvdata(pdev);
	struct netdev_private *np = dev->priv;

	rt_unregister_rtnetdev(dev);
	rt_rtdev_disconnect(dev);
	rtskb_pool_release(&np->skb_pool);
/*** RTnet ***/

	pci_release_regions (pdev);
	iounmap ((char *) dev->base_addr);
	rtdev_free(dev); /*** RTnet ***/
	pci_set_drvdata(pdev, NULL);
}

#ifdef CONFIG_PM

/*
 * The ns83815 chip doesn't have explicit RxStop bits.
 * Kicking the Rx or Tx process for a new packet reenables the Rx process
 * of the nic, thus this function must be very careful:
 *
 * suspend/resume synchronization:
 * entry points:
 *   netdev_open, netdev_close, netdev_ioctl, set_rx_mode, intr_handler,
 *   start_tx, tx_timeout
 *
 * No function accesses the hardware without checking np->hands_off.
 *	the check occurs under spin_lock_irq(&np->lock);
 * exceptions:
 *	* netdev_ioctl: noncritical access.
 *	* netdev_open: cannot happen due to the device_detach
 *	* netdev_close: doesn't hurt.
 *	* netdev_timer: timer stopped by natsemi_suspend.
 *	* intr_handler: doesn't acquire the spinlock. suspend calls
 *		disable_irq() to enforce synchronization.
 *
 * Interrupts must be disabled, otherwise hands_off can cause irq storms.
 */

#if 0
static int natsemi_suspend (struct pci_dev *pdev, u32 state)
{
	struct rtnet_device *dev = pci_get_drvdata (pdev);
	struct netdev_private *np = dev->priv;
	long ioaddr = dev->base_addr;

	rtnl_lock();
	if (rtnetif_running (dev)) {
/*** RTnet ***
		del_timer_sync(&np->timer);
 *** RTnet ***/

		rt_disable_irq(dev->irq);
		rt_spin_lock_irq(&np->lock);

		writel(0, ioaddr + IntrEnable);
		np->hands_off = 1;
		natsemi_stop_rxtx(dev);
		rtnetif_stop_queue(dev);

		rt_spin_unlock_irq(&np->lock);
		rt_enable_irq(dev->irq);

		/* Update the error counts. */
		__get_stats(dev);

		/* pci_power_off(pdev, -1); */
		drain_ring(dev);
		{
			u32 wol = readl(ioaddr + WOLCmd) & WakeOptsSummary;
			/* Restore PME enable bit */
			if (wol) {
				/* restart the NIC in WOL mode.
				 * The nic must be stopped for this.
				 * FIXME: use the WOL interupt
				 */
				enable_wol_mode(dev, 0);
			} else {
				/* Restore PME enable bit unmolested */
				writel(np->SavedClkRun, ioaddr + ClkRun);
			}
		}
	}
	rtnetif_device_detach(dev);
	rtnl_unlock();
	return 0;
}


static int natsemi_resume (struct pci_dev *pdev)
{
	struct rt_net_device *dev = pci_get_drvdata (pdev);
	struct netdev_private *np = dev->priv;

	rtnl_lock();
	if (rtnetif_device_present(dev))
		goto out;
	if (rtnetif_running(dev)) {
		BUG_ON(!np->hands_off);
		pci_enable_device(pdev);
	/*	pci_power_on(pdev); */

		natsemi_reset(dev);
		init_ring(dev);
		rt_disable_irq(dev->irq);
		rt_spin_lock_irq(&np->lock);
		np->hands_off = 0;
		init_registers(dev);
		rtnetif_device_attach(dev);
		rt_spin_unlock_irq(&np->lock);
		rt_enable_irq(dev->irq);

/*** RTnet ***
		mod_timer(&np->timer, jiffies + 1*HZ);
 *** RTnet ***/
	}
	rtnetif_device_attach(dev);
out:
	rtnl_unlock();
	return 0;
}
#endif

#endif /* CONFIG_PM */

static struct pci_driver natsemi_driver = {
	.name		= DRV_NAME,
	.id_table	= natsemi_pci_tbl,
	.probe		= natsemi_probe1,
	.remove		= __devexit_p(natsemi_remove1),
/*#ifdef CONFIG_PM*/
#if 0
	.suspend	= natsemi_suspend,
	.resume		= natsemi_resume,
#endif
};

static int __init natsemi_init_mod (void)
{
/* when a module, this is printed whether or not devices are found in probe */
#ifdef MODULE
	rtdm_printk(version);
#endif

	return pci_module_init (&natsemi_driver);
}

static void __exit natsemi_exit_mod (void)
{
	pci_unregister_driver (&natsemi_driver);
}

module_init(natsemi_init_mod);
module_exit(natsemi_exit_mod);
