/*
 *	The PCI Utilities -- List All PCI Devices
 *
 *	Copyright (c) 1997--2004 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

#include "pciutils.h"

/* Options */

static int verbose;			/* Show detailed information */
static int buscentric_view;		/* Show bus addresses/IRQ's instead of CPU-visible ones */
static int show_hex;			/* Show contents of config space as hexadecimal numbers */
static struct pci_filter filter;	/* Device filter */
static int show_tree;			/* Show bus tree */
static int machine_readable;		/* Generate machine-readable output */
static int map_mode;			/* Bus mapping mode enabled */

static char options[] = "nvbxs:d:ti:mgM" GENERIC_OPTIONS ;

static char help_msg[] = "\
Usage: lspci [<switches>]\n\
\n\
-v\t\tBe verbose\n\
-n\t\tShow numeric ID's\n\
-b\t\tBus-centric view (PCI addresses and IRQ's instead of those seen by the CPU)\n\
-x\t\tShow hex-dump of the standard portion of config space\n\
-xxx\t\tShow hex-dump of the whole config space (dangerous; root only)\n\
-xxxx\t\tShow hex-dump of the 4096-byte extended config space (root only)\n\
-s [[[[<domain>]:]<bus>]:][<slot>][.[<func>]]\tShow only devices in selected slots\n\
-d [<vendor>]:[<device>]\tShow only selected devices\n\
-t\t\tShow bus tree\n\
-m\t\tProduce machine-readable output\n\
-i <file>\tUse specified ID database instead of %s\n\
-M\t\tEnable `bus mapping' mode (dangerous; root only)\n"
GENERIC_HELP
;

/* Communication with libpci */

static struct pci_access *pacc;

/*
 *  If we aren't being compiled by GCC, use malloc() instead of alloca().
 *  This increases our memory footprint, but only slightly since we don't
 *  use alloca() much.
 */

#ifndef __GNUC__
#define alloca malloc
#endif

/* Our view of the PCI bus */

struct device {
  struct device *next;
  struct pci_dev *dev;
  unsigned int config_cnt;
  byte config[256];
};

static struct device *first_dev;

static struct device *
scan_device(struct pci_dev *p)
{
  struct device *d;

  if (!pci_filter_match(&filter, p))
    return NULL;
  d = xmalloc(sizeof(struct device));
  bzero(d, sizeof(*d));
  d->dev = p;
  d->config_cnt = 64;
  if (!pci_read_block(p, 0, d->config, 64))
    die("Unable to read the configuration space header.");
  if ((d->config[PCI_HEADER_TYPE] & 0x7f) == PCI_HEADER_TYPE_CARDBUS)
    {
      /* For cardbus bridges, we need to fetch 64 bytes more to get the full standard header... */
      if (!pci_read_block(p, 64, d->config+64, 64))
	die("Unable to read cardbus bridge extension data.");
      d->config_cnt = 128;
    }
  pci_setup_cache(p, d->config, d->config_cnt);
  pci_fill_info(p, PCI_FILL_IDENT | PCI_FILL_IRQ | PCI_FILL_BASES | PCI_FILL_ROM_BASE | PCI_FILL_SIZES);
  return d;
}

static void
scan_devices(void)
{
  struct device *d;
  struct pci_dev *p;

  pci_scan_bus(pacc);
  for(p=pacc->devices; p; p=p->next)
    if (d = scan_device(p))
      {
	d->next = first_dev;
	first_dev = d;
      }
}

static int
config_fetch(struct device *d, unsigned int pos, unsigned int len)
{
  if (pos + len < d->config_cnt)
    return 1;
  return pci_read_block(d->dev, pos, d->config + pos, len);
}

/* Config space accesses */

static inline byte
get_conf_byte(struct device *d, unsigned int pos)
{
  return d->config[pos];
}

static word
get_conf_word(struct device *d, unsigned int pos)
{
  return d->config[pos] | (d->config[pos+1] << 8);
}

static u32
get_conf_long(struct device *d, unsigned int pos)
{
  return d->config[pos] |
    (d->config[pos+1] << 8) |
    (d->config[pos+2] << 16) |
    (d->config[pos+3] << 24);
}

/* Sorting */

static int
compare_them(const void *A, const void *B)
{
  const struct pci_dev *a = (*(const struct device **)A)->dev;
  const struct pci_dev *b = (*(const struct device **)B)->dev;

  if (a->domain < b->domain)
    return -1;
  if (a->domain > b->domain)
    return 1;
  if (a->bus < b->bus)
    return -1;
  if (a->bus > b->bus)
    return 1;
  if (a->dev < b->dev)
    return -1;
  if (a->dev > b->dev)
    return 1;
  if (a->func < b->func)
    return -1;
  if (a->func > b->func)
    return 1;
  return 0;
}

static void
sort_them(void)
{
  struct device **index, **h, **last_dev;
  int cnt;
  struct device *d;

  cnt = 0;
  for(d=first_dev; d; d=d->next)
    cnt++;
  h = index = alloca(sizeof(struct device *) * cnt);
  for(d=first_dev; d; d=d->next)
    *h++ = d;
  qsort(index, cnt, sizeof(struct device *), compare_them);
  last_dev = &first_dev;
  h = index;
  while (cnt--)
    {
      *last_dev = *h;
      last_dev = &(*h)->next;
      h++;
    }
  *last_dev = NULL;
}

/* Normal output */

#define FLAG(x,y) ((x & y) ? '+' : '-')

static void
show_slot_name(struct device *d)
{
  struct pci_dev *p = d->dev;

  if (p->domain)
    printf("%04x:", p->domain);
  printf("%02x:%02x.%d", p->bus, p->dev, p->func);
}

static void
show_terse(struct device *d)
{
  int c;
  struct pci_dev *p = d->dev;
  byte classbuf[128], devbuf[128];

  show_slot_name(d);
  printf(" %s: %s",
	 pci_lookup_name(pacc, classbuf, sizeof(classbuf),
			 PCI_LOOKUP_CLASS,
			 get_conf_word(d, PCI_CLASS_DEVICE), 0, 0, 0),
	 pci_lookup_name(pacc, devbuf, sizeof(devbuf),
			 PCI_LOOKUP_VENDOR | PCI_LOOKUP_DEVICE,
			 p->vendor_id, p->device_id, 0, 0));
  if (c = get_conf_byte(d, PCI_REVISION_ID))
    printf(" (rev %02x)", c);
  if (verbose)
    {
      char *x;
      c = get_conf_byte(d, PCI_CLASS_PROG);
      x = pci_lookup_name(pacc, devbuf, sizeof(devbuf),
			  PCI_LOOKUP_PROGIF,
			  get_conf_word(d, PCI_CLASS_DEVICE), c, 0, 0);
      if (c || x)
	{
	  printf(" (prog-if %02x", c);
	  if (x)
	    printf(" [%s]", x);
	  putchar(')');
	}
    }
  putchar('\n');
}

static void
show_size(pciaddr_t x)
{
  if (!x)
    return;
  printf(" [size=");
  if (x < 1024)
    printf("%d", (int) x);
  else if (x < 1048576)
    printf("%dK", (int)(x / 1024));
  else if (x < 0x80000000)
    printf("%dM", (int)(x / 1048576));
  else
    printf(PCIADDR_T_FMT, x);
  putchar(']');
}

static void
show_bases(struct device *d, int cnt)
{
  struct pci_dev *p = d->dev;
  word cmd = get_conf_word(d, PCI_COMMAND);
  int i;

  for(i=0; i<cnt; i++)
    {
      pciaddr_t pos = p->base_addr[i];
      pciaddr_t len = (p->known_fields & PCI_FILL_SIZES) ? p->size[i] : 0;
      u32 flg = get_conf_long(d, PCI_BASE_ADDRESS_0 + 4*i);
      if (flg == 0xffffffff)
	flg = 0;
      if (!pos && !flg && !len)
	continue;
      if (verbose > 1)
	printf("\tRegion %d: ", i);
      else
	putchar('\t');
      if (pos && !flg)			/* Reported by the OS, but not by the device */
	{
	  printf("[virtual] ");
	  flg = pos;
	}
      if (flg & PCI_BASE_ADDRESS_SPACE_IO)
	{
	  pciaddr_t a = pos & PCI_BASE_ADDRESS_IO_MASK;
	  printf("I/O ports at ");
	  if (a)
	    printf(PCIADDR_PORT_FMT, a);
	  else if (flg & PCI_BASE_ADDRESS_IO_MASK)
	    printf("<ignored>");
	  else
	    printf("<unassigned>");
	  if (!(cmd & PCI_COMMAND_IO))
	    printf(" [disabled]");
	}
      else
	{
	  int t = flg & PCI_BASE_ADDRESS_MEM_TYPE_MASK;
	  pciaddr_t a = pos & PCI_ADDR_MEM_MASK;
	  int done = 0;
	  u32 z = 0;

	  printf("Memory at ");
	  if (t == PCI_BASE_ADDRESS_MEM_TYPE_64)
	    {
	      if (i >= cnt - 1)
		{
		  printf("<invalid-64bit-slot>");
		  done = 1;
		}
	      else
		{
		  i++;
		  z = get_conf_long(d, PCI_BASE_ADDRESS_0 + 4*i);
		  if (buscentric_view)
		    {
		      if (a || z)
			printf("%08x" PCIADDR_T_FMT, z, a);
		      else
			printf("<unassigned>");
		      done = 1;
		    }
		}
	    }
	  if (!done)
	    {
	      if (a)
		printf(PCIADDR_T_FMT, a);
	      else
		printf(((flg & PCI_BASE_ADDRESS_MEM_MASK) || z) ? "<ignored>" : "<unassigned>");
	    }
	  printf(" (%s, %sprefetchable)",
		 (t == PCI_BASE_ADDRESS_MEM_TYPE_32) ? "32-bit" :
		 (t == PCI_BASE_ADDRESS_MEM_TYPE_64) ? "64-bit" :
		 (t == PCI_BASE_ADDRESS_MEM_TYPE_1M) ? "low-1M" : "type 3",
		 (flg & PCI_BASE_ADDRESS_MEM_PREFETCH) ? "" : "non-");
	  if (!(cmd & PCI_COMMAND_MEMORY))
	    printf(" [disabled]");
	}
      show_size(len);
      putchar('\n');
    }
}

static void
show_pm(struct device *d, int where, int cap)
{
  int t, b;
  static int pm_aux_current[8] = { 0, 55, 100, 160, 220, 270, 320, 375 };

  printf("Power Management version %d\n", cap & PCI_PM_CAP_VER_MASK);
  if (verbose < 2)
    return;
  printf("\t\tFlags: PMEClk%c DSI%c D1%c D2%c AuxCurrent=%dmA PME(D0%c,D1%c,D2%c,D3hot%c,D3cold%c)\n",
	 FLAG(cap, PCI_PM_CAP_PME_CLOCK),
	 FLAG(cap, PCI_PM_CAP_DSI),
	 FLAG(cap, PCI_PM_CAP_D1),
	 FLAG(cap, PCI_PM_CAP_D2),
	 pm_aux_current[(cap >> 6) & 7],
	 FLAG(cap, PCI_PM_CAP_PME_D0),
	 FLAG(cap, PCI_PM_CAP_PME_D1),
	 FLAG(cap, PCI_PM_CAP_PME_D2),
	 FLAG(cap, PCI_PM_CAP_PME_D3_HOT),
	 FLAG(cap, PCI_PM_CAP_PME_D3_COLD));
  if (!config_fetch(d, where + PCI_PM_CTRL, PCI_PM_SIZEOF - PCI_PM_CTRL))
    return;
  t = get_conf_word(d, where + PCI_PM_CTRL);
  printf("\t\tStatus: D%d PME-Enable%c DSel=%d DScale=%d PME%c\n",
	 t & PCI_PM_CTRL_STATE_MASK,
	 FLAG(t, PCI_PM_CTRL_PME_ENABLE),
	 (t & PCI_PM_CTRL_DATA_SEL_MASK) >> 9,
	 (t & PCI_PM_CTRL_DATA_SCALE_MASK) >> 13,
	 FLAG(t, PCI_PM_CTRL_PME_STATUS));
  b = get_conf_byte(d, where + PCI_PM_PPB_EXTENSIONS);
  if (b)
    printf("\t\tBridge: PM%c B3%c\n",
	   FLAG(t, PCI_PM_BPCC_ENABLE),
	   FLAG(~t, PCI_PM_PPB_B2_B3));
}

static void
format_agp_rate(int rate, char *buf, int agp3)
{
  char *c = buf;
  int i;

  for(i=0; i<=2; i++)
    if (rate & (1 << i))
      {
	if (c != buf)
	  *c++ = ',';
	c += sprintf(c, "x%d", 1 << (i + 2*agp3));
      }
  if (c != buf)
    *c = 0;
  else
    strcpy(buf, "<none>");
}

static void
show_agp(struct device *d, int where, int cap)
{
  u32 t;
  char rate[16];
  int ver, rev;
  int agp3 = 0;

  ver = (cap >> 4) & 0x0f;
  rev = cap & 0x0f;
  printf("AGP version %x.%x\n", ver, rev);
  if (verbose < 2)
    return;
  if (!config_fetch(d, where + PCI_AGP_STATUS, PCI_AGP_SIZEOF - PCI_AGP_STATUS))
    return;
  t = get_conf_long(d, where + PCI_AGP_STATUS);
  if (ver >= 3 && (t & PCI_AGP_STATUS_AGP3))
    agp3 = 1;
  format_agp_rate(t & 7, rate, agp3);
  printf("\t\tStatus: RQ=%d Iso%c ArqSz=%d Cal=%d SBA%c ITACoh%c GART64%c HTrans%c 64bit%c FW%c AGP3%c Rate=%s\n",
	 ((t & PCI_AGP_STATUS_RQ_MASK) >> 24U) + 1,
	 FLAG(t, PCI_AGP_STATUS_ISOCH),
	 ((t & PCI_AGP_STATUS_ARQSZ_MASK) >> 13),
	 ((t & PCI_AGP_STATUS_CAL_MASK) >> 10),
	 FLAG(t, PCI_AGP_STATUS_SBA),
	 FLAG(t, PCI_AGP_STATUS_ITA_COH),
	 FLAG(t, PCI_AGP_STATUS_GART64),
	 FLAG(t, PCI_AGP_STATUS_HTRANS),
	 FLAG(t, PCI_AGP_STATUS_64BIT),
	 FLAG(t, PCI_AGP_STATUS_FW),
	 FLAG(t, PCI_AGP_STATUS_AGP3),
	 rate);
  t = get_conf_long(d, where + PCI_AGP_COMMAND);
  format_agp_rate(t & 7, rate, agp3);
  printf("\t\tCommand: RQ=%d ArqSz=%d Cal=%d SBA%c AGP%c GART64%c 64bit%c FW%c Rate=%s\n",
	 ((t & PCI_AGP_COMMAND_RQ_MASK) >> 24U) + 1,
	 ((t & PCI_AGP_COMMAND_ARQSZ_MASK) >> 13),
	 ((t & PCI_AGP_COMMAND_CAL_MASK) >> 10),
	 FLAG(t, PCI_AGP_COMMAND_SBA),
	 FLAG(t, PCI_AGP_COMMAND_AGP),
	 FLAG(t, PCI_AGP_COMMAND_GART64),
	 FLAG(t, PCI_AGP_COMMAND_64BIT),
	 FLAG(t, PCI_AGP_COMMAND_FW),
	 rate);
}

static void
show_pcix_nobridge(struct device *d, int where)
{
  u16 command;
  u32 status;

  printf("PCI-X non-bridge device.\n");
      
  if (verbose < 2)
    return;

  if (!config_fetch(d, where + PCI_PCIX_STATUS, 4))
    return;
  
  command = get_conf_word(d, where + PCI_PCIX_COMMAND);
  status = get_conf_long(d, where + PCI_PCIX_STATUS);
  printf("\t\tCommand: DPERE%c ERO%c RBC=%d OST=%d\n",
	 FLAG(command, PCI_PCIX_COMMAND_DPERE),
	 FLAG(command, PCI_PCIX_COMMAND_ERO),
	 ((command & PCI_PCIX_COMMAND_MAX_MEM_READ_BYTE_COUNT) >> 2U),
	 ((command & PCI_PCIX_COMMAND_MAX_OUTSTANDING_SPLIT_TRANS) >> 4U));
  printf("\t\tStatus: Bus=%u Dev=%u Func=%u 64bit%c 133MHz%c SCD%c USC%c, DC=%s, DMMRBC=%u, DMOST=%u, DMCRS=%u, RSCEM%c\n",
	 ((status >> 8) & 0xffU), // bus
	 ((status >> 3) & 0x1fU), // dev
	 (status & PCI_PCIX_STATUS_FUNCTION), // function
	 FLAG(status, PCI_PCIX_STATUS_64BIT),
	 FLAG(status, PCI_PCIX_STATUS_133MHZ),
	 FLAG(status, PCI_PCIX_STATUS_SC_DISCARDED),
	 FLAG(status, PCI_PCIX_STATUS_UNEXPECTED_SC),
	 ((status & PCI_PCIX_STATUS_DEVICE_COMPLEXITY) ? "bridge" : "simple"),
	 ((status >> 21) & 3U),
	 ((status >> 23) & 7U),
	 ((status >> 26) & 7U),
	 FLAG(status, PCI_PCIX_STATUS_RCVD_SC_ERR_MESS));
}

static void
show_pcix_bridge(struct device *d, int where)
{

  u16 secstatus;
  u32 status, upstcr, downstcr;
  
  printf("PCI-X bridge device.\n");
  
  if (verbose < 2)
    return;
  
  if (!config_fetch(d, where + PCI_PCIX_BRIDGE_STATUS, 12))
    return;
  
  secstatus = get_conf_word(d, where + PCI_PCIX_BRIDGE_SEC_STATUS);
  printf("\t\tSecondary Status: 64bit%c, 133MHz%c, SCD%c, USC%c, SCO%c, SRD%c Freq=%d\n",
	 FLAG(secstatus, PCI_PCIX_BRIDGE_SEC_STATUS_64BIT),
	 FLAG(secstatus, PCI_PCIX_BRIDGE_SEC_STATUS_133MHZ),
	 FLAG(secstatus, PCI_PCIX_BRIDGE_SEC_STATUS_SC_DISCARDED),
	 FLAG(secstatus, PCI_PCIX_BRIDGE_SEC_STATUS_UNEXPECTED_SC),
	 FLAG(secstatus, PCI_PCIX_BRIDGE_SEC_STATUS_SC_OVERRUN),
	 FLAG(secstatus, PCI_PCIX_BRIDGE_SEC_STATUS_SPLIT_REQUEST_DELAYED),
	 ((secstatus >> 6) & 7));
  status = get_conf_long(d, where + PCI_PCIX_BRIDGE_STATUS);
  printf("\t\tStatus: Bus=%u Dev=%u Func=%u 64bit%c 133MHz%c SCD%c USC%c, SCO%c, SRD%c\n", 
	 ((status >> 8) & 0xff), // bus
	 ((status >> 3) & 0x1f), // dev
	 (status & PCI_PCIX_BRIDGE_STATUS_FUNCTION), // function
	 FLAG(status, PCI_PCIX_BRIDGE_STATUS_64BIT),
	 FLAG(status, PCI_PCIX_BRIDGE_STATUS_133MHZ),
	 FLAG(status, PCI_PCIX_BRIDGE_STATUS_SC_DISCARDED),
	 FLAG(status, PCI_PCIX_BRIDGE_STATUS_UNEXPECTED_SC),
	 FLAG(status, PCI_PCIX_BRIDGE_STATUS_SC_OVERRUN),
	 FLAG(status, PCI_PCIX_BRIDGE_STATUS_SPLIT_REQUEST_DELAYED));
  upstcr = get_conf_long(d, where + PCI_PCIX_BRIDGE_UPSTREAM_SPLIT_TRANS_CTRL);
  printf("\t\t: Upstream: Capacity=%u, Commitment Limit=%u\n",
	 (upstcr & PCI_PCIX_BRIDGE_STR_CAPACITY),
	 (upstcr >> 16) & 0xffff);
  downstcr = get_conf_long(d, where + PCI_PCIX_BRIDGE_DOWNSTREAM_SPLIT_TRANS_CTRL);
  printf("\t\t: Downstream: Capacity=%u, Commitment Limit=%u\n",
	 (downstcr & PCI_PCIX_BRIDGE_STR_CAPACITY),
	 (downstcr >> 16) & 0xffff);
}

static void
show_pcix(struct device *d, int where)
{
  switch (get_conf_byte(d, PCI_HEADER_TYPE) & 0x7f)
    {
    case PCI_HEADER_TYPE_NORMAL:
      show_pcix_nobridge(d, where);
      break;
    case PCI_HEADER_TYPE_BRIDGE:
      show_pcix_bridge(d, where);
      break;
    }
}

static inline char *
ht_link_width(unsigned width)
{
  static char * const widths[8] = { "8bit", "16bit", "[2]", "32bit", "2bit", "4bit", "[6]", "N/C" };
  return widths[width];
}

static inline char *
ht_link_freq(unsigned freq)
{
  static char * const freqs[16] = { "200MHz", "300MHz", "400MHz", "500MHz", "600MHz", "800MHz", "1.0GHz", "1.2GHz",
				    "1.4GHz", "1.6GHz", "[a]", "[b]", "[c]", "[d]", "[e]", "Vend" };
  return freqs[freq];
}

static void
show_ht_pri(struct device *d, int where, int cmd)
{
  u16 lctr0, lcnf0, lctr1, lcnf1, eh;
  u8 rid, lfrer0, lfcap0, ftr, lfrer1, lfcap1, mbu, mlu, bn;

  printf("HyperTransport: Slave or Primary Interface\n");
  if (verbose < 2)
    return;

  printf("\t\tCommand: BaseUnitID=%u UnitCnt=%u MastHost%c DefDir%c DUL%c\n",
	 (cmd & PCI_HT_PRI_CMD_BUID),
	 (cmd & PCI_HT_PRI_CMD_UC) >> 5,
	 FLAG(cmd, PCI_HT_PRI_CMD_MH),
	 FLAG(cmd, PCI_HT_PRI_CMD_DD),
	 FLAG(cmd, PCI_HT_PRI_CMD_DUL));
  config_fetch(d, where + PCI_HT_PRI_LCTR0, PCI_HT_PRI_SIZEOF - PCI_HT_PRI_LCTR0);
  lctr0 = get_conf_word(d, where + PCI_HT_PRI_LCTR0);
  printf("\t\tLink Control 0: CFlE%c CST%c CFE%c <LkFail%c Init%c EOC%c TXO%c <CRCErr=%x IsocEn%c LSEn%c ExtCTL%c 64b%c\n",
	 FLAG(lctr0, PCI_HT_LCTR_CFLE),
	 FLAG(lctr0, PCI_HT_LCTR_CST),
	 FLAG(lctr0, PCI_HT_LCTR_CFE),
	 FLAG(lctr0, PCI_HT_LCTR_LKFAIL),
	 FLAG(lctr0, PCI_HT_LCTR_INIT),
	 FLAG(lctr0, PCI_HT_LCTR_EOC),
	 FLAG(lctr0, PCI_HT_LCTR_TXO),
	 (lctr0 & PCI_HT_LCTR_CRCERR) >> 8,
	 FLAG(lctr0, PCI_HT_LCTR_ISOCEN),
	 FLAG(lctr0, PCI_HT_LCTR_LSEN),
	 FLAG(lctr0, PCI_HT_LCTR_EXTCTL),
	 FLAG(lctr0, PCI_HT_LCTR_64B));
  lcnf0 = get_conf_word(d, where + PCI_HT_PRI_LCNF0);
  printf("\t\tLink Config 0: MLWI=%s DwFcIn%c MLWO=%s DwFcOut%c LWI=%s DwFcInEn%c LWO=%s DwFcOutEn%c\n",
	 ht_link_width(lcnf0 & PCI_HT_LCNF_MLWI),
	 FLAG(lcnf0, PCI_HT_LCNF_DFI),
	 ht_link_width((lcnf0 & PCI_HT_LCNF_MLWO) >> 4),
	 FLAG(lcnf0, PCI_HT_LCNF_DFO),
	 ht_link_width((lcnf0 & PCI_HT_LCNF_LWI) >> 8),
	 FLAG(lcnf0, PCI_HT_LCNF_DFIE),
	 ht_link_width((lcnf0 & PCI_HT_LCNF_LWO) >> 12),
	 FLAG(lcnf0, PCI_HT_LCNF_DFOE));
  lctr1 = get_conf_word(d, where + PCI_HT_PRI_LCTR1);
  printf("\t\tLink Control 1: CFlE%c CST%c CFE%c <LkFail%c Init%c EOC%c TXO%c <CRCErr=%x IsocEn%c LSEn%c ExtCTL%c 64b%c\n",
	 FLAG(lctr1, PCI_HT_LCTR_CFLE),
	 FLAG(lctr1, PCI_HT_LCTR_CST),
	 FLAG(lctr1, PCI_HT_LCTR_CFE),
	 FLAG(lctr1, PCI_HT_LCTR_LKFAIL),
	 FLAG(lctr1, PCI_HT_LCTR_INIT),
	 FLAG(lctr1, PCI_HT_LCTR_EOC),
	 FLAG(lctr1, PCI_HT_LCTR_TXO),
	 (lctr1 & PCI_HT_LCTR_CRCERR) >> 8,
	 FLAG(lctr1, PCI_HT_LCTR_ISOCEN),
	 FLAG(lctr1, PCI_HT_LCTR_LSEN),
	 FLAG(lctr1, PCI_HT_LCTR_EXTCTL),
	 FLAG(lctr1, PCI_HT_LCTR_64B));
  lcnf1 = get_conf_word(d, where + PCI_HT_PRI_LCNF1);
  printf("\t\tLink Config 1: MLWI=%s DwFcIn%c MLWO=%s DwFcOut%c LWI=%s DwFcInEn%c LWO=%s DwFcOutEn%c\n",
	 ht_link_width(lcnf1 & PCI_HT_LCNF_MLWI),
	 FLAG(lcnf1, PCI_HT_LCNF_DFI),
	 ht_link_width((lcnf1 & PCI_HT_LCNF_MLWO) >> 4),
	 FLAG(lcnf1, PCI_HT_LCNF_DFO),
	 ht_link_width((lcnf1 & PCI_HT_LCNF_LWI) >> 8),
	 FLAG(lcnf1, PCI_HT_LCNF_DFIE),
	 ht_link_width((lcnf1 & PCI_HT_LCNF_LWO) >> 12),
	 FLAG(lcnf1, PCI_HT_LCNF_DFOE));
  rid = get_conf_byte(d, where + PCI_HT_PRI_RID);
  printf("\t\tRevision ID: %u.%02u\n",
	 (rid & PCI_HT_RID_MAJ) >> 5, (rid & PCI_HT_RID_MIN));
  lfrer0 = get_conf_byte(d, where + PCI_HT_PRI_LFRER0);
  printf("\t\tLink Frequency 0: %s\n", ht_link_freq(lfrer0 & PCI_HT_LFRER_FREQ));
  printf("\t\tLink Error 0: <Prot%c <Ovfl%c <EOC%c CTLTm%c\n",
	 FLAG(lfrer0, PCI_HT_LFRER_PROT),
	 FLAG(lfrer0, PCI_HT_LFRER_OV),
	 FLAG(lfrer0, PCI_HT_LFRER_EOC),
	 FLAG(lfrer0, PCI_HT_LFRER_CTLT));
  lfcap0 = get_conf_byte(d, where + PCI_HT_PRI_LFCAP0);
  printf("\t\tLink Frequency Capability 0: 200MHz%c 300MHz%c 400MHz%c 500MHz%c 600MHz%c 800MHz%c 1.0GHz%c 1.2GHz%c 1.4GHz%c 1.6GHz%c Vend%c\n",
	 FLAG(lfcap0, PCI_HT_LFCAP_200),
	 FLAG(lfcap0, PCI_HT_LFCAP_300),
	 FLAG(lfcap0, PCI_HT_LFCAP_400),
	 FLAG(lfcap0, PCI_HT_LFCAP_500),
	 FLAG(lfcap0, PCI_HT_LFCAP_600),
	 FLAG(lfcap0, PCI_HT_LFCAP_800),
	 FLAG(lfcap0, PCI_HT_LFCAP_1000),
	 FLAG(lfcap0, PCI_HT_LFCAP_1200),
	 FLAG(lfcap0, PCI_HT_LFCAP_1400),
	 FLAG(lfcap0, PCI_HT_LFCAP_1600),
	 FLAG(lfcap0, PCI_HT_LFCAP_VEND));
  ftr = get_conf_byte(d, where + PCI_HT_PRI_FTR);
  printf("\t\tFeature Capability: IsocFC%c LDTSTOP%c CRCTM%c ECTLT%c 64bA%c UIDRD%c\n",
	 FLAG(ftr, PCI_HT_FTR_ISOCFC),
	 FLAG(ftr, PCI_HT_FTR_LDTSTOP),
	 FLAG(ftr, PCI_HT_FTR_CRCTM),
	 FLAG(ftr, PCI_HT_FTR_ECTLT),
	 FLAG(ftr, PCI_HT_FTR_64BA),
	 FLAG(ftr, PCI_HT_FTR_UIDRD));
  lfrer1 = get_conf_byte(d, where + PCI_HT_PRI_LFRER1);
  printf("\t\tLink Frequency 1: %s\n", ht_link_freq(lfrer1 & PCI_HT_LFRER_FREQ));
  printf("\t\tLink Error 1: <Prot%c <Ovfl%c <EOC%c CTLTm%c\n",
	 FLAG(lfrer1, PCI_HT_LFRER_PROT),
	 FLAG(lfrer1, PCI_HT_LFRER_OV),
	 FLAG(lfrer1, PCI_HT_LFRER_EOC),
	 FLAG(lfrer1, PCI_HT_LFRER_CTLT));
  lfcap1 = get_conf_byte(d, where + PCI_HT_PRI_LFCAP1);
  printf("\t\tLink Frequency Capability 1: 200MHz%c 300MHz%c 400MHz%c 500MHz%c 600MHz%c 800MHz%c 1.0GHz%c 1.2GHz%c 1.4GHz%c 1.6GHz%c Vend%c\n",
	 FLAG(lfcap1, PCI_HT_LFCAP_200),
	 FLAG(lfcap1, PCI_HT_LFCAP_300),
	 FLAG(lfcap1, PCI_HT_LFCAP_400),
	 FLAG(lfcap1, PCI_HT_LFCAP_500),
	 FLAG(lfcap1, PCI_HT_LFCAP_600),
	 FLAG(lfcap1, PCI_HT_LFCAP_800),
	 FLAG(lfcap1, PCI_HT_LFCAP_1000),
	 FLAG(lfcap1, PCI_HT_LFCAP_1200),
	 FLAG(lfcap1, PCI_HT_LFCAP_1400),
	 FLAG(lfcap1, PCI_HT_LFCAP_1600),
	 FLAG(lfcap1, PCI_HT_LFCAP_VEND));
  eh = get_conf_word(d, where + PCI_HT_PRI_EH);
  printf("\t\tError Handling: PFlE%c OFlE%c PFE%c OFE%c EOCFE%c RFE%c CRCFE%c SERRFE%c CF%c RE%c PNFE%c ONFE%c EOCNFE%c RNFE%c CRCNFE%c SERRNFE%c\n",
	 FLAG(eh, PCI_HT_EH_PFLE),
	 FLAG(eh, PCI_HT_EH_OFLE),
	 FLAG(eh, PCI_HT_EH_PFE),
	 FLAG(eh, PCI_HT_EH_OFE),
	 FLAG(eh, PCI_HT_EH_EOCFE),
	 FLAG(eh, PCI_HT_EH_RFE),
	 FLAG(eh, PCI_HT_EH_CRCFE),
	 FLAG(eh, PCI_HT_EH_SERRFE),
	 FLAG(eh, PCI_HT_EH_CF),
	 FLAG(eh, PCI_HT_EH_RE),
	 FLAG(eh, PCI_HT_EH_PNFE),
	 FLAG(eh, PCI_HT_EH_ONFE),
	 FLAG(eh, PCI_HT_EH_EOCNFE),
	 FLAG(eh, PCI_HT_EH_RNFE),
	 FLAG(eh, PCI_HT_EH_CRCNFE),
	 FLAG(eh, PCI_HT_EH_SERRNFE));
  mbu = get_conf_byte(d, where + PCI_HT_PRI_MBU);
  mlu = get_conf_byte(d, where + PCI_HT_PRI_MLU);
  printf("\t\tPrefetchable memory behind bridge Upper: %02x-%02x\n", mbu, mlu);
  bn = get_conf_byte(d, where + PCI_HT_PRI_BN);
  printf("\t\tBus Number: %02x\n", bn);
}

static void
show_ht_sec(struct device *d, int where, int cmd)
{
  u16 lctr, lcnf, ftr, eh;
  u8 rid, lfrer, lfcap, mbu, mlu;

  printf("HyperTransport: Host or Secondary Interface\n");
  if (verbose < 2)
    return;

  printf("\t\tCommand: WarmRst%c DblEnd%c DevNum=%u ChainSide%c HostHide%c Slave%c <EOCErr%c DUL%c\n",
	 FLAG(cmd, PCI_HT_SEC_CMD_WR),
	 FLAG(cmd, PCI_HT_SEC_CMD_DE),
	 (cmd & PCI_HT_SEC_CMD_DN) >> 2,
	 FLAG(cmd, PCI_HT_SEC_CMD_CS),
	 FLAG(cmd, PCI_HT_SEC_CMD_HH),
	 FLAG(cmd, PCI_HT_SEC_CMD_AS),
	 FLAG(cmd, PCI_HT_SEC_CMD_HIECE),
	 FLAG(cmd, PCI_HT_SEC_CMD_DUL));
  config_fetch(d, where + PCI_HT_SEC_LCTR, PCI_HT_SEC_SIZEOF - PCI_HT_SEC_LCTR);
  lctr = get_conf_word(d, where + PCI_HT_SEC_LCTR);
  printf("\t\tLink Control: CFlE%c CST%c CFE%c <LkFail%c Init%c EOC%c TXO%c <CRCErr=%x IsocEn%c LSEn%c ExtCTL%c 64b%c\n",
	 FLAG(lctr, PCI_HT_LCTR_CFLE),
	 FLAG(lctr, PCI_HT_LCTR_CST),
	 FLAG(lctr, PCI_HT_LCTR_CFE),
	 FLAG(lctr, PCI_HT_LCTR_LKFAIL),
	 FLAG(lctr, PCI_HT_LCTR_INIT),
	 FLAG(lctr, PCI_HT_LCTR_EOC),
	 FLAG(lctr, PCI_HT_LCTR_TXO),
	 (lctr & PCI_HT_LCTR_CRCERR) >> 8,
	 FLAG(lctr, PCI_HT_LCTR_ISOCEN),
	 FLAG(lctr, PCI_HT_LCTR_LSEN),
	 FLAG(lctr, PCI_HT_LCTR_EXTCTL),
	 FLAG(lctr, PCI_HT_LCTR_64B));
  lcnf = get_conf_word(d, where + PCI_HT_SEC_LCNF);
  printf("\t\tLink Config: MLWI=%s DwFcIn%c MLWO=%s DwFcOut%c LWI=%s DwFcInEn%c LWO=%s DwFcOutEn%c\n",
	 ht_link_width(lcnf & PCI_HT_LCNF_MLWI),
	 FLAG(lcnf, PCI_HT_LCNF_DFI),
	 ht_link_width((lcnf & PCI_HT_LCNF_MLWO) >> 4),
	 FLAG(lcnf, PCI_HT_LCNF_DFO),
	 ht_link_width((lcnf & PCI_HT_LCNF_LWI) >> 8),
	 FLAG(lcnf, PCI_HT_LCNF_DFIE),
	 ht_link_width((lcnf & PCI_HT_LCNF_LWO) >> 12),
	 FLAG(lcnf, PCI_HT_LCNF_DFOE));
  rid = get_conf_byte(d, where + PCI_HT_SEC_RID);
  printf("\t\tRevision ID: %u.%02u\n",
	 (rid & PCI_HT_RID_MAJ) >> 5, (rid & PCI_HT_RID_MIN));
  lfrer = get_conf_byte(d, where + PCI_HT_SEC_LFRER);
  printf("\t\tLink Frequency: %s\n", ht_link_freq(lfrer & PCI_HT_LFRER_FREQ));
  printf("\t\tLink Error: <Prot%c <Ovfl%c <EOC%c CTLTm%c\n",
	 FLAG(lfrer, PCI_HT_LFRER_PROT),
	 FLAG(lfrer, PCI_HT_LFRER_OV),
	 FLAG(lfrer, PCI_HT_LFRER_EOC),
	 FLAG(lfrer, PCI_HT_LFRER_CTLT));
  lfcap = get_conf_byte(d, where + PCI_HT_SEC_LFCAP);
  printf("\t\tLink Frequency Capability: 200MHz%c 300MHz%c 400MHz%c 500MHz%c 600MHz%c 800MHz%c 1.0GHz%c 1.2GHz%c 1.4GHz%c 1.6GHz%c Vend%c\n",
	 FLAG(lfcap, PCI_HT_LFCAP_200),
	 FLAG(lfcap, PCI_HT_LFCAP_300),
	 FLAG(lfcap, PCI_HT_LFCAP_400),
	 FLAG(lfcap, PCI_HT_LFCAP_500),
	 FLAG(lfcap, PCI_HT_LFCAP_600),
	 FLAG(lfcap, PCI_HT_LFCAP_800),
	 FLAG(lfcap, PCI_HT_LFCAP_1000),
	 FLAG(lfcap, PCI_HT_LFCAP_1200),
	 FLAG(lfcap, PCI_HT_LFCAP_1400),
	 FLAG(lfcap, PCI_HT_LFCAP_1600),
	 FLAG(lfcap, PCI_HT_LFCAP_VEND));
  ftr = get_conf_word(d, where + PCI_HT_SEC_FTR);
  printf("\t\tFeature Capability: IsocFC%c LDTSTOP%c CRCTM%c ECTLT%c 64bA%c UIDRD%c ExtRS%c UCnfE%c\n",
	 FLAG(ftr, PCI_HT_FTR_ISOCFC),
	 FLAG(ftr, PCI_HT_FTR_LDTSTOP),
	 FLAG(ftr, PCI_HT_FTR_CRCTM),
	 FLAG(ftr, PCI_HT_FTR_ECTLT),
	 FLAG(ftr, PCI_HT_FTR_64BA),
	 FLAG(ftr, PCI_HT_FTR_UIDRD),
	 FLAG(ftr, PCI_HT_SEC_FTR_EXTRS),
	 FLAG(ftr, PCI_HT_SEC_FTR_UCNFE));
  if (ftr & PCI_HT_SEC_FTR_EXTRS)
    {
      eh = get_conf_word(d, where + PCI_HT_SEC_EH);
      printf("\t\tError Handling: PFlE%c OFlE%c PFE%c OFE%c EOCFE%c RFE%c CRCFE%c SERRFE%c CF%c RE%c PNFE%c ONFE%c EOCNFE%c RNFE%c CRCNFE%c SERRNFE%c\n",
	     FLAG(eh, PCI_HT_EH_PFLE),
	     FLAG(eh, PCI_HT_EH_OFLE),
	     FLAG(eh, PCI_HT_EH_PFE),
	     FLAG(eh, PCI_HT_EH_OFE),
	     FLAG(eh, PCI_HT_EH_EOCFE),
	     FLAG(eh, PCI_HT_EH_RFE),
	     FLAG(eh, PCI_HT_EH_CRCFE),
	     FLAG(eh, PCI_HT_EH_SERRFE),
	     FLAG(eh, PCI_HT_EH_CF),
	     FLAG(eh, PCI_HT_EH_RE),
	     FLAG(eh, PCI_HT_EH_PNFE),
	     FLAG(eh, PCI_HT_EH_ONFE),
	     FLAG(eh, PCI_HT_EH_EOCNFE),
	     FLAG(eh, PCI_HT_EH_RNFE),
	     FLAG(eh, PCI_HT_EH_CRCNFE),
	     FLAG(eh, PCI_HT_EH_SERRNFE));
      mbu = get_conf_byte(d, where + PCI_HT_SEC_MBU);
      mlu = get_conf_byte(d, where + PCI_HT_SEC_MLU);
      printf("\t\tPrefetchable memory behind bridge Upper: %02x-%02x\n", mbu, mlu);
    }
}

static void
show_ht(struct device *d, int where, int cmd)
{
  int type;

  switch (cmd & PCI_HT_CMD_TYP_HI)
    {
    case PCI_HT_CMD_TYP_HI_PRI:
      show_ht_pri(d, where, cmd);
      return;
    case PCI_HT_CMD_TYP_HI_SEC:
      show_ht_sec(d, where, cmd);
      return;
    }

  type = cmd & PCI_HT_CMD_TYP;
  switch (type)
    {
    case PCI_HT_CMD_TYP_SW:
      printf("HyperTransport: Switch\n");
      break;
    case PCI_HT_CMD_TYP_IDC:
      printf("HyperTransport: Interrupt Discovery and Configuration\n");
      break;
    case PCI_HT_CMD_TYP_RID:
      printf("HyperTransport: Revision ID: %u.%02u\n",
	     (cmd & PCI_HT_RID_MAJ) >> 5, (cmd & PCI_HT_RID_MIN));
      break;
    case PCI_HT_CMD_TYP_UIDC:
      printf("HyperTransport: UnitID Clumping\n");
      break;
    case PCI_HT_CMD_TYP_ECSA:
      printf("HyperTransport: Extended Configuration Space Access\n");
      break;
    case PCI_HT_CMD_TYP_AM:
      printf("HyperTransport: Address Mapping\n");
      break;
    case PCI_HT_CMD_TYP_MSIM:
      printf("HyperTransport: MSI Mapping\n");
      break;
    case PCI_HT_CMD_TYP_DR:
      printf("HyperTransport: DirectRoute\n");
      break;
    case PCI_HT_CMD_TYP_VCS:
      printf("HyperTransport: VCSet\n");
      break;
    case PCI_HT_CMD_TYP_RM:
      printf("HyperTransport: Retry Mode\n");
      break;
    case PCI_HT_CMD_TYP_X86:
      printf("HyperTransport: X86 (reserved)\n");
      break;
    default:
      printf("HyperTransport: #%02x\n", type >> 11);
    }
}

static void
show_rom(struct device *d)
{
  struct pci_dev *p = d->dev;
  pciaddr_t rom = p->rom_base_addr;
  pciaddr_t len = (p->known_fields & PCI_FILL_SIZES) ? p->rom_size : 0;

  if (!rom && !len)
    return;
  printf("\tExpansion ROM at ");
  if (rom & PCI_ROM_ADDRESS_MASK)
    printf(PCIADDR_T_FMT, rom & PCI_ROM_ADDRESS_MASK);
  else
    printf("<unassigned>");
  if (!(rom & PCI_ROM_ADDRESS_ENABLE))
    printf(" [disabled]");
  show_size(len);
  putchar('\n');
}

static void
show_msi(struct device *d, int where, int cap)
{
  int is64;
  u32 t;
  u16 w;

  printf("Message Signalled Interrupts: 64bit%c Queue=%d/%d Enable%c\n",
	 FLAG(cap, PCI_MSI_FLAGS_64BIT),
	 (cap & PCI_MSI_FLAGS_QSIZE) >> 4,
	 (cap & PCI_MSI_FLAGS_QMASK) >> 1,
	 FLAG(cap, PCI_MSI_FLAGS_ENABLE));
  if (verbose < 2)
    return;
  is64 = cap & PCI_MSI_FLAGS_64BIT;
  if (!config_fetch(d, where + PCI_MSI_ADDRESS_LO, (is64 ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32) + 2 - PCI_MSI_ADDRESS_LO))
    return;
  printf("\t\tAddress: ");
  if (is64)
    {
      t = get_conf_long(d, where + PCI_MSI_ADDRESS_HI);
      w = get_conf_word(d, where + PCI_MSI_DATA_64);
      printf("%08x", t);
    }
  else
    w = get_conf_word(d, where + PCI_MSI_DATA_32);
  t = get_conf_long(d, where + PCI_MSI_ADDRESS_LO);
  printf("%08x  Data: %04x\n", t, w);
}

static void
show_slotid(int cap)
{
  int esr = cap & 0xff;
  int chs = cap >> 8;

  printf("Slot ID: %d slots, First%c, chassis %02x\n",
	 esr & PCI_SID_ESR_NSLOTS,
	 FLAG(esr, PCI_SID_ESR_FIC),
	 chs);
}

static void
show_caps(struct device *d)
{
  if (get_conf_word(d, PCI_STATUS) & PCI_STATUS_CAP_LIST)
    {
      int where = get_conf_byte(d, PCI_CAPABILITY_LIST) & ~3;
      while (where)
	{
	  int id, next, cap;
	  printf("\tCapabilities: ");
	  if (!config_fetch(d, where, 4))
	    {
	      puts("<available only to root>");
	      break;
	    }
	  id = get_conf_byte(d, where + PCI_CAP_LIST_ID);
	  next = get_conf_byte(d, where + PCI_CAP_LIST_NEXT) & ~3;
	  cap = get_conf_word(d, where + PCI_CAP_FLAGS);
	  printf("[%02x] ", where);
	  if (id == 0xff)
	    {
	      printf("<chain broken>\n");
	      break;
	    }
	  switch (id)
	    {
	    case PCI_CAP_ID_PM:
	      show_pm(d, where, cap);
	      break;
	    case PCI_CAP_ID_AGP:
	      show_agp(d, where, cap);
	      break;
	    case PCI_CAP_ID_VPD:
	      printf("Vital Product Data\n");
	      break;
	    case PCI_CAP_ID_SLOTID:
	      show_slotid(cap);
	      break;
	    case PCI_CAP_ID_MSI:
	      show_msi(d, where, cap);
	      break;
	    case PCI_CAP_ID_PCIX:
	      show_pcix(d, where);
	      break;
	    case PCI_CAP_ID_HT:
	      show_ht(d, where, cap);
	      break;
	    default:
	      printf("#%02x [%04x]\n", id, cap);
	    }
	  where = next;
	}
    }
}

static void
show_htype0(struct device *d)
{
  show_bases(d, 6);
  show_rom(d);
  show_caps(d);
}

static void
show_htype1(struct device *d)
{
  u32 io_base = get_conf_byte(d, PCI_IO_BASE);
  u32 io_limit = get_conf_byte(d, PCI_IO_LIMIT);
  u32 io_type = io_base & PCI_IO_RANGE_TYPE_MASK;
  u32 mem_base = get_conf_word(d, PCI_MEMORY_BASE);
  u32 mem_limit = get_conf_word(d, PCI_MEMORY_LIMIT);
  u32 mem_type = mem_base & PCI_MEMORY_RANGE_TYPE_MASK;
  u32 pref_base = get_conf_word(d, PCI_PREF_MEMORY_BASE);
  u32 pref_limit = get_conf_word(d, PCI_PREF_MEMORY_LIMIT);
  u32 pref_type = pref_base & PCI_PREF_RANGE_TYPE_MASK;
  word sec_stat = get_conf_word(d, PCI_SEC_STATUS);
  word brc = get_conf_word(d, PCI_BRIDGE_CONTROL);
  int verb = verbose > 2;

  show_bases(d, 2);
  printf("\tBus: primary=%02x, secondary=%02x, subordinate=%02x, sec-latency=%d\n",
	 get_conf_byte(d, PCI_PRIMARY_BUS),
	 get_conf_byte(d, PCI_SECONDARY_BUS),
	 get_conf_byte(d, PCI_SUBORDINATE_BUS),
	 get_conf_byte(d, PCI_SEC_LATENCY_TIMER));

  if (io_type != (io_limit & PCI_IO_RANGE_TYPE_MASK) ||
      (io_type != PCI_IO_RANGE_TYPE_16 && io_type != PCI_IO_RANGE_TYPE_32))
    printf("\t!!! Unknown I/O range types %x/%x\n", io_base, io_limit);
  else
    {
      io_base = (io_base & PCI_IO_RANGE_MASK) << 8;
      io_limit = (io_limit & PCI_IO_RANGE_MASK) << 8;
      if (io_type == PCI_IO_RANGE_TYPE_32)
	{
	  io_base |= (get_conf_word(d, PCI_IO_BASE_UPPER16) << 16);
	  io_limit |= (get_conf_word(d, PCI_IO_LIMIT_UPPER16) << 16);
	}
      if (io_base <= io_limit || verb)
	printf("\tI/O behind bridge: %08x-%08x\n", io_base, io_limit+0xfff);
    }

  if (mem_type != (mem_limit & PCI_MEMORY_RANGE_TYPE_MASK) ||
      mem_type)
    printf("\t!!! Unknown memory range types %x/%x\n", mem_base, mem_limit);
  else
    {
      mem_base = (mem_base & PCI_MEMORY_RANGE_MASK) << 16;
      mem_limit = (mem_limit & PCI_MEMORY_RANGE_MASK) << 16;
      if (mem_base <= mem_limit || verb)
	printf("\tMemory behind bridge: %08x-%08x\n", mem_base, mem_limit + 0xfffff);
    }

  if (pref_type != (pref_limit & PCI_PREF_RANGE_TYPE_MASK) ||
      (pref_type != PCI_PREF_RANGE_TYPE_32 && pref_type != PCI_PREF_RANGE_TYPE_64))
    printf("\t!!! Unknown prefetchable memory range types %x/%x\n", pref_base, pref_limit);
  else
    {
      pref_base = (pref_base & PCI_PREF_RANGE_MASK) << 16;
      pref_limit = (pref_limit & PCI_PREF_RANGE_MASK) << 16;
      if (pref_base <= pref_limit || verb)
	{
	  if (pref_type == PCI_PREF_RANGE_TYPE_32)
	    printf("\tPrefetchable memory behind bridge: %08x-%08x\n", pref_base, pref_limit + 0xfffff);
	  else
	    printf("\tPrefetchable memory behind bridge: %08x%08x-%08x%08x\n",
		   get_conf_long(d, PCI_PREF_BASE_UPPER32),
		   pref_base,
		   get_conf_long(d, PCI_PREF_LIMIT_UPPER32),
		   pref_limit);
	}
    }

  if (verbose > 1)
    printf("\tSecondary status: 66Mhz%c FastB2B%c ParErr%c DEVSEL=%s >TAbort%c <TAbort%c <MAbort%c <SERR%c <PERR%c\n",
	     FLAG(sec_stat, PCI_STATUS_66MHZ),
	     FLAG(sec_stat, PCI_STATUS_FAST_BACK),
	     FLAG(sec_stat, PCI_STATUS_PARITY),
	     ((sec_stat & PCI_STATUS_DEVSEL_MASK) == PCI_STATUS_DEVSEL_SLOW) ? "slow" :
	     ((sec_stat & PCI_STATUS_DEVSEL_MASK) == PCI_STATUS_DEVSEL_MEDIUM) ? "medium" :
	     ((sec_stat & PCI_STATUS_DEVSEL_MASK) == PCI_STATUS_DEVSEL_FAST) ? "fast" : "??",
	     FLAG(sec_stat, PCI_STATUS_SIG_TARGET_ABORT),
	     FLAG(sec_stat, PCI_STATUS_REC_TARGET_ABORT),
	     FLAG(sec_stat, PCI_STATUS_REC_MASTER_ABORT),
	     FLAG(sec_stat, PCI_STATUS_SIG_SYSTEM_ERROR),
	     FLAG(sec_stat, PCI_STATUS_DETECTED_PARITY));

  show_rom(d);

  if (verbose > 1)
    printf("\tBridgeCtl: Parity%c SERR%c NoISA%c VGA%c MAbort%c >Reset%c FastB2B%c\n",
	   FLAG(brc, PCI_BRIDGE_CTL_PARITY),
	   FLAG(brc, PCI_BRIDGE_CTL_SERR),
	   FLAG(brc, PCI_BRIDGE_CTL_NO_ISA),
	   FLAG(brc, PCI_BRIDGE_CTL_VGA),
	   FLAG(brc, PCI_BRIDGE_CTL_MASTER_ABORT),
	   FLAG(brc, PCI_BRIDGE_CTL_BUS_RESET),
	   FLAG(brc, PCI_BRIDGE_CTL_FAST_BACK));

  show_caps(d);
}

static void
show_htype2(struct device *d)
{
  int i;
  word cmd = get_conf_word(d, PCI_COMMAND);
  word brc = get_conf_word(d, PCI_CB_BRIDGE_CONTROL);
  word exca = get_conf_word(d, PCI_CB_LEGACY_MODE_BASE);
  int verb = verbose > 2;

  show_bases(d, 1);
  printf("\tBus: primary=%02x, secondary=%02x, subordinate=%02x, sec-latency=%d\n",
	 get_conf_byte(d, PCI_CB_PRIMARY_BUS),
	 get_conf_byte(d, PCI_CB_CARD_BUS),
	 get_conf_byte(d, PCI_CB_SUBORDINATE_BUS),
	 get_conf_byte(d, PCI_CB_LATENCY_TIMER));
  for(i=0; i<2; i++)
    {
      int p = 8*i;
      u32 base = get_conf_long(d, PCI_CB_MEMORY_BASE_0 + p);
      u32 limit = get_conf_long(d, PCI_CB_MEMORY_LIMIT_0 + p);
      if (limit > base || verb)
	printf("\tMemory window %d: %08x-%08x%s%s\n", i, base, limit,
	       (cmd & PCI_COMMAND_MEMORY) ? "" : " [disabled]",
	       (brc & (PCI_CB_BRIDGE_CTL_PREFETCH_MEM0 << i)) ? " (prefetchable)" : "");
    }
  for(i=0; i<2; i++)
    {
      int p = 8*i;
      u32 base = get_conf_long(d, PCI_CB_IO_BASE_0 + p);
      u32 limit = get_conf_long(d, PCI_CB_IO_LIMIT_0 + p);
      if (!(base & PCI_IO_RANGE_TYPE_32))
	{
	  base &= 0xffff;
	  limit &= 0xffff;
	}
      base &= PCI_CB_IO_RANGE_MASK;
      limit = (limit & PCI_CB_IO_RANGE_MASK) + 3;
      if (base <= limit || verb)
	printf("\tI/O window %d: %08x-%08x%s\n", i, base, limit,
	       (cmd & PCI_COMMAND_IO) ? "" : " [disabled]");
    }

  if (get_conf_word(d, PCI_CB_SEC_STATUS) & PCI_STATUS_SIG_SYSTEM_ERROR)
    printf("\tSecondary status: SERR\n");
  if (verbose > 1)
    printf("\tBridgeCtl: Parity%c SERR%c ISA%c VGA%c MAbort%c >Reset%c 16bInt%c PostWrite%c\n",
	   FLAG(brc, PCI_CB_BRIDGE_CTL_PARITY),
	   FLAG(brc, PCI_CB_BRIDGE_CTL_SERR),
	   FLAG(brc, PCI_CB_BRIDGE_CTL_ISA),
	   FLAG(brc, PCI_CB_BRIDGE_CTL_VGA),
	   FLAG(brc, PCI_CB_BRIDGE_CTL_MASTER_ABORT),
	   FLAG(brc, PCI_CB_BRIDGE_CTL_CB_RESET),
	   FLAG(brc, PCI_CB_BRIDGE_CTL_16BIT_INT),
	   FLAG(brc, PCI_CB_BRIDGE_CTL_POST_WRITES));
  if (exca)
    printf("\t16-bit legacy interface ports at %04x\n", exca);
}

static void
show_verbose(struct device *d)
{
  struct pci_dev *p = d->dev;
  word status = get_conf_word(d, PCI_STATUS);
  word cmd = get_conf_word(d, PCI_COMMAND);
  word class = get_conf_word(d, PCI_CLASS_DEVICE);
  byte bist = get_conf_byte(d, PCI_BIST);
  byte htype = get_conf_byte(d, PCI_HEADER_TYPE) & 0x7f;
  byte latency = get_conf_byte(d, PCI_LATENCY_TIMER);
  byte cache_line = get_conf_byte(d, PCI_CACHE_LINE_SIZE);
  byte max_lat, min_gnt;
  byte int_pin = get_conf_byte(d, PCI_INTERRUPT_PIN);
  unsigned int irq = p->irq;
  word subsys_v, subsys_d;
  char ssnamebuf[256];

  show_terse(d);

  switch (htype)
    {
    case PCI_HEADER_TYPE_NORMAL:
      if (class == PCI_CLASS_BRIDGE_PCI)
	printf("\t!!! Invalid class %04x for header type %02x\n", class, htype);
      max_lat = get_conf_byte(d, PCI_MAX_LAT);
      min_gnt = get_conf_byte(d, PCI_MIN_GNT);
      subsys_v = get_conf_word(d, PCI_SUBSYSTEM_VENDOR_ID);
      subsys_d = get_conf_word(d, PCI_SUBSYSTEM_ID);
      break;
    case PCI_HEADER_TYPE_BRIDGE:
      if ((class >> 8) != PCI_BASE_CLASS_BRIDGE)
	printf("\t!!! Invalid class %04x for header type %02x\n", class, htype);
      irq = int_pin = min_gnt = max_lat = 0;
      subsys_v = subsys_d = 0;
      break;
    case PCI_HEADER_TYPE_CARDBUS:
      if ((class >> 8) != PCI_BASE_CLASS_BRIDGE)
	printf("\t!!! Invalid class %04x for header type %02x\n", class, htype);
      min_gnt = max_lat = 0;
      subsys_v = get_conf_word(d, PCI_CB_SUBSYSTEM_VENDOR_ID);
      subsys_d = get_conf_word(d, PCI_CB_SUBSYSTEM_ID);
      break;
    default:
      printf("\t!!! Unknown header type %02x\n", htype);
      return;
    }

  if (subsys_v && subsys_v != 0xffff)
    printf("\tSubsystem: %s\n",
	   pci_lookup_name(pacc, ssnamebuf, sizeof(ssnamebuf),
			   PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_VENDOR | PCI_LOOKUP_DEVICE,
			   p->vendor_id, p->device_id, subsys_v, subsys_d));

  if (verbose > 1)
    {
      printf("\tControl: I/O%c Mem%c BusMaster%c SpecCycle%c MemWINV%c VGASnoop%c ParErr%c Stepping%c SERR%c FastB2B%c\n",
	     FLAG(cmd, PCI_COMMAND_IO),
	     FLAG(cmd, PCI_COMMAND_MEMORY),
	     FLAG(cmd, PCI_COMMAND_MASTER),
	     FLAG(cmd, PCI_COMMAND_SPECIAL),
	     FLAG(cmd, PCI_COMMAND_INVALIDATE),
	     FLAG(cmd, PCI_COMMAND_VGA_PALETTE),
	     FLAG(cmd, PCI_COMMAND_PARITY),
	     FLAG(cmd, PCI_COMMAND_WAIT),
	     FLAG(cmd, PCI_COMMAND_SERR),
	     FLAG(cmd, PCI_COMMAND_FAST_BACK));
      printf("\tStatus: Cap%c 66Mhz%c UDF%c FastB2B%c ParErr%c DEVSEL=%s >TAbort%c <TAbort%c <MAbort%c >SERR%c <PERR%c\n",
	     FLAG(status, PCI_STATUS_CAP_LIST),
	     FLAG(status, PCI_STATUS_66MHZ),
	     FLAG(status, PCI_STATUS_UDF),
	     FLAG(status, PCI_STATUS_FAST_BACK),
	     FLAG(status, PCI_STATUS_PARITY),
	     ((status & PCI_STATUS_DEVSEL_MASK) == PCI_STATUS_DEVSEL_SLOW) ? "slow" :
	     ((status & PCI_STATUS_DEVSEL_MASK) == PCI_STATUS_DEVSEL_MEDIUM) ? "medium" :
	     ((status & PCI_STATUS_DEVSEL_MASK) == PCI_STATUS_DEVSEL_FAST) ? "fast" : "??",
	     FLAG(status, PCI_STATUS_SIG_TARGET_ABORT),
	     FLAG(status, PCI_STATUS_REC_TARGET_ABORT),
	     FLAG(status, PCI_STATUS_REC_MASTER_ABORT),
	     FLAG(status, PCI_STATUS_SIG_SYSTEM_ERROR),
	     FLAG(status, PCI_STATUS_DETECTED_PARITY));
      if (cmd & PCI_COMMAND_MASTER)
	{
	  printf("\tLatency: %d", latency);
	  if (min_gnt || max_lat)
	    {
	      printf(" (");
	      if (min_gnt)
		printf("%dns min", min_gnt*250);
	      if (min_gnt && max_lat)
		printf(", ");
	      if (max_lat)
		printf("%dns max", max_lat*250);
	      putchar(')');
	    }
	  if (cache_line)
	    printf(", Cache Line Size %02x", cache_line);
	  putchar('\n');
	}
      if (int_pin || irq)
	printf("\tInterrupt: pin %c routed to IRQ " PCIIRQ_FMT "\n",
	       (int_pin ? 'A' + int_pin - 1 : '?'), irq);
    }
  else
    {
      printf("\tFlags: ");
      if (cmd & PCI_COMMAND_MASTER)
	printf("bus master, ");
      if (cmd & PCI_COMMAND_VGA_PALETTE)
	printf("VGA palette snoop, ");
      if (cmd & PCI_COMMAND_WAIT)
	printf("stepping, ");
      if (cmd & PCI_COMMAND_FAST_BACK)
	printf("fast Back2Back, ");
      if (status & PCI_STATUS_66MHZ)
	printf("66Mhz, ");
      if (status & PCI_STATUS_UDF)
	printf("user-definable features, ");
      printf("%s devsel",
	     ((status & PCI_STATUS_DEVSEL_MASK) == PCI_STATUS_DEVSEL_SLOW) ? "slow" :
	     ((status & PCI_STATUS_DEVSEL_MASK) == PCI_STATUS_DEVSEL_MEDIUM) ? "medium" :
	     ((status & PCI_STATUS_DEVSEL_MASK) == PCI_STATUS_DEVSEL_FAST) ? "fast" : "??");
      if (cmd & PCI_COMMAND_MASTER)
	printf(", latency %d", latency);
      if (irq)
	printf(", IRQ " PCIIRQ_FMT, irq);
      putchar('\n');
    }

  if (bist & PCI_BIST_CAPABLE)
    {
      if (bist & PCI_BIST_START)
	printf("\tBIST is running\n");
      else
	printf("\tBIST result: %02x\n", bist & PCI_BIST_CODE_MASK);
    }

  switch (htype)
    {
    case PCI_HEADER_TYPE_NORMAL:
      show_htype0(d);
      break;
    case PCI_HEADER_TYPE_BRIDGE:
      show_htype1(d);
      break;
    case PCI_HEADER_TYPE_CARDBUS:
      show_htype2(d);
      break;
    }
}

static void
show_hex_dump(struct device *d)
{
  unsigned int i, cnt;

  cnt = d->config_cnt;
  if (show_hex >= 3 && config_fetch(d, cnt, 256-cnt))
    {
      cnt = 256;
      if (show_hex >= 4 && config_fetch(d, 256, 4096-256))
	cnt = 4096;
    }

  for(i=0; i<cnt; i++)
    {
      if (! (i & 15))
	printf("%02x:", i);
      printf(" %02x", get_conf_byte(d, i));
      if ((i & 15) == 15)
	putchar('\n');
    }
}

static void
show_machine(struct device *d)
{
  struct pci_dev *p = d->dev;
  int c;
  word sv_id=0, sd_id=0;
  char classbuf[128], vendbuf[128], devbuf[128], svbuf[128], sdbuf[128];

  switch (get_conf_byte(d, PCI_HEADER_TYPE) & 0x7f)
    {
    case PCI_HEADER_TYPE_NORMAL:
      sv_id = get_conf_word(d, PCI_SUBSYSTEM_VENDOR_ID);
      sd_id = get_conf_word(d, PCI_SUBSYSTEM_ID);
      break;
    case PCI_HEADER_TYPE_CARDBUS:
      sv_id = get_conf_word(d, PCI_CB_SUBSYSTEM_VENDOR_ID);
      sd_id = get_conf_word(d, PCI_CB_SUBSYSTEM_ID);
      break;
    }

  if (verbose)
    {
      printf("Device:\t");
      show_slot_name(d);
      putchar('\n');
      printf("Class:\t%s\n",
	     pci_lookup_name(pacc, classbuf, sizeof(classbuf), PCI_LOOKUP_CLASS, get_conf_word(d, PCI_CLASS_DEVICE), 0, 0, 0));
      printf("Vendor:\t%s\n",
	     pci_lookup_name(pacc, vendbuf, sizeof(vendbuf), PCI_LOOKUP_VENDOR, p->vendor_id, p->device_id, 0, 0));
      printf("Device:\t%s\n",
	     pci_lookup_name(pacc, devbuf, sizeof(devbuf), PCI_LOOKUP_DEVICE, p->vendor_id, p->device_id, 0, 0));
      if (sv_id && sv_id != 0xffff)
	{
	  printf("SVendor:\t%s\n",
		 pci_lookup_name(pacc, svbuf, sizeof(svbuf), PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_VENDOR, p->vendor_id, p->device_id, sv_id, sd_id));
	  printf("SDevice:\t%s\n",
		 pci_lookup_name(pacc, sdbuf, sizeof(sdbuf), PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_DEVICE, p->vendor_id, p->device_id, sv_id, sd_id));
	}
      if (c = get_conf_byte(d, PCI_REVISION_ID))
	printf("Rev:\t%02x\n", c);
      if (c = get_conf_byte(d, PCI_CLASS_PROG))
	printf("ProgIf:\t%02x\n", c);
    }
  else
    {
      show_slot_name(d);
      printf(" \"%s\" \"%s\" \"%s\"",
	     pci_lookup_name(pacc, classbuf, sizeof(classbuf), PCI_LOOKUP_CLASS,
			     get_conf_word(d, PCI_CLASS_DEVICE), 0, 0, 0),
	     pci_lookup_name(pacc, vendbuf, sizeof(vendbuf), PCI_LOOKUP_VENDOR,
			     p->vendor_id, p->device_id, 0, 0),
	     pci_lookup_name(pacc, devbuf, sizeof(devbuf), PCI_LOOKUP_DEVICE,
			     p->vendor_id, p->device_id, 0, 0));
      if (c = get_conf_byte(d, PCI_REVISION_ID))
	printf(" -r%02x", c);
      if (c = get_conf_byte(d, PCI_CLASS_PROG))
	printf(" -p%02x", c);
      if (sv_id && sv_id != 0xffff)
	printf(" \"%s\" \"%s\"",
	       pci_lookup_name(pacc, svbuf, sizeof(svbuf), PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_VENDOR, p->vendor_id, p->device_id, sv_id, sd_id),
	       pci_lookup_name(pacc, sdbuf, sizeof(sdbuf), PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_DEVICE, p->vendor_id, p->device_id, sv_id, sd_id));
      else
	printf(" \"\" \"\"");
      putchar('\n');
    }
}

static void
show_device(struct device *d)
{
  if (machine_readable)
    show_machine(d);
  else if (verbose)
    show_verbose(d);
  else
    show_terse(d);
  if (show_hex)
    show_hex_dump(d);
  if (verbose || show_hex)
    putchar('\n');
}

static void
show(void)
{
  struct device *d;

  for(d=first_dev; d; d=d->next)
    show_device(d);
}

/* Tree output */

struct bridge {
  struct bridge *chain;			/* Single-linked list of bridges */
  struct bridge *next, *child;		/* Tree of bridges */
  struct bus *first_bus;		/* List of busses connected to this bridge */
  unsigned int domain;
  unsigned int primary, secondary, subordinate;	/* Bus numbers */
  struct device *br_dev;
};

struct bus {
  unsigned int domain;
  unsigned int number;
  struct bus *sibling;
  struct device *first_dev, **last_dev;
};

static struct bridge host_bridge = { NULL, NULL, NULL, NULL, 0, ~0, 0, ~0, NULL };

static struct bus *
find_bus(struct bridge *b, unsigned int domain, unsigned int n)
{
  struct bus *bus;

  for(bus=b->first_bus; bus; bus=bus->sibling)
    if (bus->domain == domain && bus->number == n)
      break;
  return bus;
}

static struct bus *
new_bus(struct bridge *b, unsigned int domain, unsigned int n)
{
  struct bus *bus = xmalloc(sizeof(struct bus));

  bus = xmalloc(sizeof(struct bus));
  bus->domain = domain;
  bus->number = n;
  bus->sibling = b->first_bus;
  bus->first_dev = NULL;
  bus->last_dev = &bus->first_dev;
  b->first_bus = bus;
  return bus;
}

static void
insert_dev(struct device *d, struct bridge *b)
{
  struct pci_dev *p = d->dev;
  struct bus *bus;

  if (! (bus = find_bus(b, p->domain, p->bus)))
    {
      struct bridge *c;
      for(c=b->child; c; c=c->next)
	if (c->domain == p->domain && c->secondary <= p->bus && p->bus <= c->subordinate)
          {
            insert_dev(d, c);
            return;
          }
      bus = new_bus(b, p->domain, p->bus);
    }
  /* Simple insertion at the end _does_ guarantee the correct order as the
   * original device list was sorted by (domain, bus, devfn) lexicographically
   * and all devices on the new list have the same bus number.
   */
  *bus->last_dev = d;
  bus->last_dev = &d->next;
  d->next = NULL;
}

static void
grow_tree(void)
{
  struct device *d, *d2;
  struct bridge **last_br, *b;

  /* Build list of bridges */

  last_br = &host_bridge.chain;
  for(d=first_dev; d; d=d->next)
    {
      word class = get_conf_word(d, PCI_CLASS_DEVICE);
      byte ht = get_conf_byte(d, PCI_HEADER_TYPE) & 0x7f;
      if (class == PCI_CLASS_BRIDGE_PCI &&
	  (ht == PCI_HEADER_TYPE_BRIDGE || ht == PCI_HEADER_TYPE_CARDBUS))
	{
	  b = xmalloc(sizeof(struct bridge));
	  b->domain = d->dev->domain;
	  if (ht == PCI_HEADER_TYPE_BRIDGE)
	    {
	      b->primary = get_conf_byte(d, PCI_CB_PRIMARY_BUS);
	      b->secondary = get_conf_byte(d, PCI_CB_CARD_BUS);
	      b->subordinate = get_conf_byte(d, PCI_CB_SUBORDINATE_BUS);
	    }
	  else
	    {
	      b->primary = get_conf_byte(d, PCI_PRIMARY_BUS);
	      b->secondary = get_conf_byte(d, PCI_SECONDARY_BUS);
	      b->subordinate = get_conf_byte(d, PCI_SUBORDINATE_BUS);
	    }
	  *last_br = b;
	  last_br = &b->chain;
	  b->next = b->child = NULL;
	  b->first_bus = NULL;
	  b->br_dev = d;
	}
    }
  *last_br = NULL;

  /* Create a bridge tree */

  for(b=&host_bridge; b; b=b->chain)
    {
      struct bridge *c, *best;
      best = NULL;
      for(c=&host_bridge; c; c=c->chain)
	if (c != b && (c == &host_bridge || b->domain == c->domain) &&
	    b->primary >= c->secondary && b->primary <= c->subordinate &&
	    (!best || best->subordinate - best->primary > c->subordinate - c->primary))
	  best = c;
      if (best)
	{
	  b->next = best->child;
	  best->child = b;
	}
    }

  /* Insert secondary bus for each bridge */

  for(b=&host_bridge; b; b=b->chain)
    if (!find_bus(b, b->domain, b->secondary))
      new_bus(b, b->domain, b->secondary);

  /* Create bus structs and link devices */

  for(d=first_dev; d;)
    {
      d2 = d->next;
      insert_dev(d, &host_bridge);
      d = d2;
    }
}

static void
print_it(byte *line, byte *p)
{
  *p++ = '\n';
  *p = 0;
  fputs(line, stdout);
  for(p=line; *p; p++)
    if (*p == '+' || *p == '|')
      *p = '|';
    else
      *p = ' ';
}

static void show_tree_bridge(struct bridge *, byte *, byte *);

static void
show_tree_dev(struct device *d, byte *line, byte *p)
{
  struct pci_dev *q = d->dev;
  struct bridge *b;
  char namebuf[256];

  p += sprintf(p, "%02x.%x", q->dev, q->func);
  for(b=&host_bridge; b; b=b->chain)
    if (b->br_dev == d)
      {
	if (b->secondary == b->subordinate)
	  p += sprintf(p, "-[%04x:%02x]-", b->domain, b->secondary);
	else
	  p += sprintf(p, "-[%04x:%02x-%02x]-", b->domain, b->secondary, b->subordinate);
        show_tree_bridge(b, line, p);
        return;
      }
  if (verbose)
    p += sprintf(p, "  %s",
		 pci_lookup_name(pacc, namebuf, sizeof(namebuf),
				 PCI_LOOKUP_VENDOR | PCI_LOOKUP_DEVICE,
				 q->vendor_id, q->device_id, 0, 0));
  print_it(line, p);
}

static void
show_tree_bus(struct bus *b, byte *line, byte *p)
{
  if (!b->first_dev)
    print_it(line, p);
  else if (!b->first_dev->next)
    {
      *p++ = '-';
      *p++ = '-';
      show_tree_dev(b->first_dev, line, p);
    }
  else
    {
      struct device *d = b->first_dev;
      while (d->next)
	{
	  p[0] = '+';
	  p[1] = '-';
	  show_tree_dev(d, line, p+2);
	  d = d->next;
	}
      p[0] = '\\';
      p[1] = '-';
      show_tree_dev(d, line, p+2);
    }
}

static void
show_tree_bridge(struct bridge *b, byte *line, byte *p)
{
  *p++ = '-';
  if (!b->first_bus->sibling)
    {
      if (b == &host_bridge)
        p += sprintf(p, "[%04x:%02x]-", b->domain, b->first_bus->number);
      show_tree_bus(b->first_bus, line, p);
    }
  else
    {
      struct bus *u = b->first_bus;
      byte *k;

      while (u->sibling)
        {
          k = p + sprintf(p, "+-[%04x:%02x]-", u->domain, u->number);
          show_tree_bus(u, line, k);
          u = u->sibling;
        }
      k = p + sprintf(p, "\\-[%04x:%02x]-", u->domain, u->number);
      show_tree_bus(u, line, k);
    }
}

static void
show_forest(void)
{
  char line[256];

  grow_tree();
  show_tree_bridge(&host_bridge, line, line);
}

/* Bus mapping mode */

struct bus_bridge {
  struct bus_bridge *next;
  byte this, dev, func, first, last, bug;
};

struct bus_info {
  byte exists;
  byte guestbook;
  struct bus_bridge *bridges, *via;
};

static struct bus_info *bus_info;

static void
map_bridge(struct bus_info *bi, struct device *d, int np, int ns, int nl)
{
  struct bus_bridge *b = xmalloc(sizeof(struct bus_bridge));
  struct pci_dev *p = d->dev;

  b->next = bi->bridges;
  bi->bridges = b;
  b->this = get_conf_byte(d, np);
  b->dev = p->dev;
  b->func = p->func;
  b->first = get_conf_byte(d, ns);
  b->last = get_conf_byte(d, nl);
  printf("## %02x.%02x:%d is a bridge from %02x to %02x-%02x\n",
	 p->bus, p->dev, p->func, b->this, b->first, b->last);
  if (b->this != p->bus)
    printf("!!! Bridge points to invalid primary bus.\n");
  if (b->first > b->last)
    {
      printf("!!! Bridge points to invalid bus range.\n");
      b->last = b->first;
    }
}

static void
do_map_bus(int bus)
{
  int dev, func;
  int verbose = pacc->debugging;
  struct bus_info *bi = bus_info + bus;
  struct device *d;

  if (verbose)
    printf("Mapping bus %02x\n", bus);
  for(dev = 0; dev < 32; dev++)
    if (filter.slot < 0 || filter.slot == dev)
      {
	int func_limit = 1;
	for(func = 0; func < func_limit; func++)
	  if (filter.func < 0 || filter.func == func)
	    {
	      /* XXX: Bus mapping supports only domain 0 */
	      struct pci_dev *p = pci_get_dev(pacc, 0, bus, dev, func);
	      u16 vendor = pci_read_word(p, PCI_VENDOR_ID);
	      if (vendor && vendor != 0xffff)
		{
		  if (!func && (pci_read_byte(p, PCI_HEADER_TYPE) & 0x80))
		    func_limit = 8;
		  if (verbose)
		    printf("Discovered device %02x:%02x.%d\n", bus, dev, func);
		  bi->exists = 1;
		  if (d = scan_device(p))
		    {
		      show_device(d);
		      switch (get_conf_byte(d, PCI_HEADER_TYPE) & 0x7f)
			{
			case PCI_HEADER_TYPE_BRIDGE:
			  map_bridge(bi, d, PCI_PRIMARY_BUS, PCI_SECONDARY_BUS, PCI_SUBORDINATE_BUS);
			  break;
			case PCI_HEADER_TYPE_CARDBUS:
			  map_bridge(bi, d, PCI_CB_PRIMARY_BUS, PCI_CB_CARD_BUS, PCI_CB_SUBORDINATE_BUS);
			  break;
			}
		      free(d);
		    }
		  else if (verbose)
		    printf("But it was filtered out.\n");
		}
	      pci_free_dev(p);
	    }
      }
}

static void
do_map_bridges(int bus, int min, int max)
{
  struct bus_info *bi = bus_info + bus;
  struct bus_bridge *b;

  bi->guestbook = 1;
  for(b=bi->bridges; b; b=b->next)
    {
      if (bus_info[b->first].guestbook)
	b->bug = 1;
      else if (b->first < min || b->last > max)
	b->bug = 2;
      else
	{
	  bus_info[b->first].via = b;
	  do_map_bridges(b->first, b->first, b->last);
	}
    }
}

static void
map_bridges(void)
{
  int i;

  printf("\nSummary of buses:\n\n");
  for(i=0; i<256; i++)
    if (bus_info[i].exists && !bus_info[i].guestbook)
      do_map_bridges(i, 0, 255);
  for(i=0; i<256; i++)
    {
      struct bus_info *bi = bus_info + i;
      struct bus_bridge *b = bi->via;

      if (bi->exists)
	{
	  printf("%02x: ", i);
	  if (b)
	    printf("Entered via %02x:%02x.%d\n", b->this, b->dev, b->func);
	  else if (!i)
	    printf("Primary host bus\n");
	  else
	    printf("Secondary host bus (?)\n");
	}
      for(b=bi->bridges; b; b=b->next)
	{
	  printf("\t%02x.%d Bridge to %02x-%02x", b->dev, b->func, b->first, b->last);
	  switch (b->bug)
	    {
	    case 1:
	      printf(" <overlap bug>");
	      break;
	    case 2:
	      printf(" <crossing bug>");
	      break;
	    }
	  putchar('\n');
	}
    }
}

static void
map_the_bus(void)
{
  if (pacc->method == PCI_ACCESS_PROC_BUS_PCI ||
      pacc->method == PCI_ACCESS_DUMP)
    printf("WARNING: Bus mapping can be reliable only with direct hardware access enabled.\n\n");
  bus_info = xmalloc(sizeof(struct bus_info) * 256);
  bzero(bus_info, sizeof(struct bus_info) * 256);
  if (filter.bus >= 0)
    do_map_bus(filter.bus);
  else
    {
      int bus;
      for(bus=0; bus<256; bus++)
	do_map_bus(bus);
    }
  map_bridges();
}

/* Main */

int
main(int argc, char **argv)
{
  int i;
  char *msg;

  if (argc == 2 && !strcmp(argv[1], "--version"))
    {
      puts("lspci version " PCIUTILS_VERSION);
      return 0;
    }

  pacc = pci_alloc();
  pacc->error = die;
  pci_filter_init(pacc, &filter);

  while ((i = getopt(argc, argv, options)) != -1)
    switch (i)
      {
      case 'n':
	pacc->numeric_ids = 1;
	break;
      case 'v':
	verbose++;
	break;
      case 'b':
	pacc->buscentric = 1;
	buscentric_view = 1;
	break;
      case 's':
	if (msg = pci_filter_parse_slot(&filter, optarg))
	  die("-s: %s", msg);
	break;
      case 'd':
	if (msg = pci_filter_parse_id(&filter, optarg))
	  die("-d: %s", msg);
	break;
      case 'x':
	show_hex++;
	break;
      case 't':
	show_tree++;
	break;
      case 'i':
	pacc->id_file_name = optarg;
	break;
      case 'm':
	machine_readable++;
	break;
      case 'M':
	map_mode++;
	break;
      default:
	if (parse_generic_option(i, pacc, optarg))
	  break;
      bad:
	fprintf(stderr, help_msg, pacc->id_file_name);
	return 1;
      }
  if (optind < argc)
    goto bad;

  pci_init(pacc);
  if (map_mode)
    map_the_bus();
  else
    {
      scan_devices();
      sort_them();
      if (show_tree)
	show_forest();
      else
	show();
    }
  pci_cleanup(pacc);

  return 0;
}
