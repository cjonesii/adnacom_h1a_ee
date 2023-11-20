/*
 *	The PCI Utilities -- List All PCI Devices
 *
 *	Copyright (c) 1997--2020 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "adna.h"
#include <stdbool.h>
#include "eep.h"
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

#include "setpci.h"

#define PLX_VENDOR_ID       (0x10B5)
#define PLX_H1A_DEVICE_ID   (0x8608)
#define ADNATOOL_VERSION    "0.0.1"

/* Options */

int verbose;              /* Show detailed information */
static int opt_hex;       /* Show contents of config space as hexadecimal numbers */
struct pci_filter filter; /* Device filter */
static int opt_path;      /* Show bridge path */
static int opt_machine;   /* Generate machine-readable output */
static int opt_domains;   /* Show domain numbers (0=disabled, 1=auto-detected, 2=requested) */
static int opt_kernel;    /* Show kernel drivers */
char *opt_pcimap;         /* Override path to Linux modules.pcimap */
static int NumDevices = 0;
const char program_name[] = "adna";
char g_h1a_us_port_bar0[256] = "\0";
uint8_t *g_pBuffer = NULL;
struct eep_options EepOptions;

/*** Our view of the PCI bus ***/

struct pci_access *pacc;
struct device *first_dev = NULL;
struct adna_device *first_adna = NULL;
static int seen_errors;
static int need_topology;

struct adnatool_pci_device {
        u16 vid;
        u16 did;
        u32 cls_rev;
} adnatool_pci_devtbl[] = {
#if 1
        { .vid = PLX_VENDOR_ID,     .did = PLX_H1A_DEVICE_ID, .cls_rev = PCI_CLASS_BRIDGE_PCI, },
#else
        /* for debugging purpose, put in some actual PCI devices i have 
         * in my system. TODO: remove these! */
        { .vid = 0x8086, .did = 0x02b0, .cls_rev = PCI_CLASS_BRIDGE_PCI, },
        { .vid = 0x10ec, .did = 0xc82f, .cls_rev = PCI_CLASS_NETWORK_OTHER, },
#endif
        {0}, /* sentinel */

};

struct eep_options {
  bool bVerbose;
  int bLoadFile;
  char    FileName[255];
  char    SerialNumber[4];
  u16     ExtraBytes;
  bool bListOnly;
  bool bSerialNumber;
};

struct adna_device {
  struct adna_device *next;
  u8 bus, dev, func;  /* Bus inside domain, device and function */
  bool bIsD3;         /* Power state */
  int devnum;         /* Assigned NumDevice */
};

int pci_get_devtype(struct pci_dev *pdev);
bool pci_is_upstream(struct pci_dev *pdev);
bool pcidev_is_adnacom(struct pci_dev *p);

void eep_read(struct device *d, uint32_t offset, volatile uint32_t *read_buffer);
void eep_read_16(struct device *d, uint32_t offset, uint16_t *read_buffer);
void eep_write(struct device *d, uint32_t offset, uint32_t write_buffer);
void eep_write_16(struct device *d, uint32_t offset, uint16_t write_buffer);
void eep_init(struct device *d);
#ifndef ADNA
static int adnatool_refresh_device_cache(void)
{
  struct device *d;
  for (d=first_dev; d; d=d->next) {
    /* let's refresh the pcidev details */
    if (!d->dev->cache) {
            u8 *cache;
            if ((cache = calloc(1, 128)) == NULL) {
                    fprintf(stderr, "error allocating pci device config cache!\n");
                    exit(-1);
            }
            pci_setup_cache(d->dev, cache, 128);
    }

    /* refresh the config block */
    if (!pci_read_block(d->dev, 0, d->dev->cache, 128)) {
            fprintf(stderr, "error reading pci device config!\n");
            return -1;
    }
  }

  return 0;
}
#endif
static void pci_get_res0(struct pci_dev *pdev, char *path, size_t pathlen)
{
  snprintf(path, 
          pathlen,
          "/sys/bus/pci/devices/%04x:%02x:%02x.%d/resource0",
          pdev->domain,
          pdev->bus,
          pdev->dev,
          pdev->func);
  return;
}

static uint32_t pcimem(struct pci_dev *p, uint32_t reg, uint32_t data)
{
  int fd;
  void *map_base, *virt_addr;
  uint64_t read_result, writeval, prev_read_result = 0;
  // char *filename;
  off_t target, target_base;
  int access_type = 'w';
  int items_count = 1;
  int read_result_dupped = 0;
  int type_width;
  int i;
  int map_size = 4096UL;

  char filename[256] = "\0";
  pci_get_res0(p, filename, sizeof(filename));
  target = (off_t)reg;

  switch (access_type)
  {
  case 'b':
    type_width = 1;
    break;
  case 'h':
    type_width = 2;
    break;
  case 'w':
    type_width = 4;
    break;
  case 'd':
    type_width = 8;
    break;
  default:
    fprintf(stderr, "Illegal data type '%c'.\n", access_type);
    exit(2);
  }

  if ((fd = open(filename, O_RDWR | O_SYNC)) == -1)
    PRINT_ERROR;
  if (EepOptions.bVerbose) {
    printf("%s opened.\n", filename);
    printf("Target offset is 0x%x, page size is %ld\n", (int)target, sysconf(_SC_PAGE_SIZE));
  }
  fflush(stdout);

  target_base = target & ~(sysconf(_SC_PAGE_SIZE) - 1);
  if (target + items_count * type_width - target_base > map_size)
    map_size = target + items_count * type_width - target_base;

  /* Map one page */
  if (EepOptions.bVerbose)
    printf("mmap(%d, %d, 0x%x, 0x%x, %d, 0x%x)\n", 0, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (int)target);

  map_base = mmap(0, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, target_base);
  if (map_base == (void *)-1)
    PRINT_ERROR;
  if (EepOptions.bVerbose)
    printf("PCI Memory mapped to address 0x%08lx.\n", (unsigned long)map_base);
  fflush(stdout);

  for (i = 0; i < items_count; i++)
  {

    virt_addr = map_base + target + i * type_width - target_base;
    switch (access_type)
    {
    case 'b':
      read_result = *((uint8_t *)virt_addr);
      break;
    case 'h':
      read_result = *((uint16_t *)virt_addr);
      break;
    case 'w':
      read_result = *((uint32_t *)virt_addr);
      break;
    case 'd':
      read_result = *((uint64_t *)virt_addr);
      break;
    }

    if (read_result != prev_read_result || i == 0)
    {
      if (EepOptions.bVerbose)
        printf("Reg 0x%04X: 0x%0*lX\n", (int)(target + i * type_width), type_width * 2, read_result);
      read_result_dupped = 0;
    }
    else
    {
      if (!read_result_dupped)
        printf("...\n");
      read_result_dupped = 1;
    }

    prev_read_result = read_result;
  }

  fflush(stdout);

  if (data)
  {
    writeval = (uint64_t)data;
    switch (access_type)
    {
    case 'b':
      *((uint8_t *)virt_addr) = writeval;
      read_result = *((uint8_t *)virt_addr);
      break;
    case 'h':
      *((uint16_t *)virt_addr) = writeval;
      read_result = *((uint16_t *)virt_addr);
      break;
    case 'w':
      *((uint32_t *)virt_addr) = writeval;
      read_result = *((uint32_t *)virt_addr);
      break;
    case 'd':
      *((uint64_t *)virt_addr) = writeval;
      read_result = *((uint64_t *)virt_addr);
      break;
    }
    if (EepOptions.bVerbose)
      printf("Written 0x%0*lX; readback 0x%*lX\n", type_width,
            writeval, type_width, read_result);
    fflush(stdout);
  }

  if (munmap(map_base, map_size) == -1)
    PRINT_ERROR;
  close(fd);
  return (data ? 0 : (uint32_t)read_result);
}

static void check_for_ready_or_done(struct device *d)
{
    volatile uint32_t eepCmdStatus = EEP_CMD_STAT_MAX;
    do {
        for (volatile int delay = 0; delay < 5000; delay++) {}
        eepCmdStatus = ((pcimem(d->dev, EEP_STAT_N_CTRL_ADDR, 0)) >> EEP_CMD_STATUS_OFFSET) & 1;
    } while (CMD_COMPLETE != eepCmdStatus);
    if (EepOptions.bVerbose)
        printf("Controller is ready\n");
}

static void eep_data(struct device *d, uint32_t cmd, volatile uint32_t *buffer)
{
    if (EepOptions.bVerbose)
        printf("Function: %s\n", __func__);

    if (EepOptions.bVerbose)
        printf("  EEPROM Control: 0x%08x\n", cmd);
    check_for_ready_or_done(d);
    pcimem(d->dev, EEP_STAT_N_CTRL_ADDR, cmd);
    check_for_ready_or_done(d);

    if (RD_4B_FR_BLKADDR_TO_BUFF == ((cmd >> EEP_CMD_OFFSET) & 0x7)) {
        *buffer = pcimem(d->dev, EEP_BUFFER_ADDR, 0);
        if (EepOptions.bVerbose)
            printf("Read buffer: 0x%08x\n", *buffer);
    }
    check_for_ready_or_done(d);
}

void eep_read(struct device *d, uint32_t offset, volatile uint32_t *read_buffer)
{
    if (EepOptions.bVerbose)
        printf("Function: %s\n", __func__);
    union eep_status_and_control_reg ctrl_reg = {0};
    // Section 6.8.2 step#2
    ctrl_reg.cmd_n_status_struct.cmd = RD_4B_FR_BLKADDR_TO_BUFF;
    ctrl_reg.cmd_n_status_struct.blk_addr = offset;
    // Section 6.8.2 step#3 and step#4
    eep_data(d, ctrl_reg.cmd_u32, read_buffer);
    fflush(stdout);
}

void eep_read_16(struct device *d, uint32_t offset, uint16_t *read_buffer)
{
    if (EepOptions.bVerbose)
        printf("Function: %s\n", __func__);
    union eep_status_and_control_reg ctrl_reg = {0};
    uint32_t buffer_32 = 0;

    ctrl_reg.cmd_n_status_struct.cmd = RD_4B_FR_BLKADDR_TO_BUFF;
    ctrl_reg.cmd_n_status_struct.blk_addr = offset;
    eep_data(d, ctrl_reg.cmd_u32, &buffer_32);

    *read_buffer = buffer_32 & 0xFFFF;
    fflush(stdout);
}

void eep_write(struct device *d, uint32_t offset, uint32_t write_buffer)
{
    if (EepOptions.bVerbose)
        printf("Function: %s\n", __func__);
    union eep_status_and_control_reg ctrl_reg = {0};

    // Section 6.8.1 step#2
    check_for_ready_or_done(d);
    pcimem(d->dev, EEP_BUFFER_ADDR, write_buffer);
    check_for_ready_or_done(d);
    // Section 6.8.1 step#3
    ctrl_reg.cmd_n_status_struct.cmd = SET_WR_EN_LATCH;
    check_for_ready_or_done(d);
    pcimem(d->dev, EEP_STAT_N_CTRL_ADDR, ctrl_reg.cmd_u32);
    check_for_ready_or_done(d);
    // Section 6.8.1 step#4
    ctrl_reg.cmd_n_status_struct.cmd = WR_4B_FR_BUFF_TO_BLKADDR;
    ctrl_reg.cmd_n_status_struct.blk_addr = offset;
    eep_data(d, ctrl_reg.cmd_u32, NULL);

    fflush(stdout);
}

void eep_write_16(struct device *d, uint32_t offset, uint16_t write_buffer)
{
    if (EepOptions.bVerbose)
        printf("Function: %s\n", __func__);
    union eep_status_and_control_reg ctrl_reg = {0};
    uint32_t buffer_32 = (uint32_t)write_buffer;

    // Section 6.8.1 step#2
    check_for_ready_or_done(d);
    pcimem(d->dev, EEP_BUFFER_ADDR, buffer_32);
    check_for_ready_or_done(d);
    // Section 6.8.1 step#3
    ctrl_reg.cmd_n_status_struct.cmd = SET_WR_EN_LATCH;
    check_for_ready_or_done(d);
    pcimem(d->dev, EEP_STAT_N_CTRL_ADDR, ctrl_reg.cmd_u32);
    check_for_ready_or_done(d);
    // Section 6.8.1 step#4
    ctrl_reg.cmd_n_status_struct.cmd = WR_4B_FR_BUFF_TO_BLKADDR;
    ctrl_reg.cmd_n_status_struct.blk_addr = offset;
    eep_data(d, ctrl_reg.cmd_u32, NULL);

    fflush(stdout);
}

void eep_init(struct device *d)
{
    if (EepOptions.bVerbose)
        printf("Function: %s\n", __func__);
    union eep_status_and_control_reg ctrl_reg = {0};

    // Section 6.8.3 step#2
    check_for_ready_or_done(d);
    pcimem(d->dev, EEP_BUFFER_ADDR, EEP_INIT_VAL);
    check_for_ready_or_done(d);
    // Section 6.8.3 step#3
    ctrl_reg.cmd_n_status_struct.cmd = SET_WR_EN_LATCH;
    ctrl_reg.cmd_n_status_struct.addr_width_override = ADDR_WIDTH_WRITABLE;
    ctrl_reg.cmd_n_status_struct.addr_width = TWO_BYTES;
    check_for_ready_or_done(d);
    pcimem(d->dev, EEP_STAT_N_CTRL_ADDR, ctrl_reg.cmd_u32);
    check_for_ready_or_done(d);
    // Section 6.8.3 step#4
    ctrl_reg.cmd_n_status_struct.cmd = WR_4B_FR_BUFF_TO_BLKADDR;
    ctrl_reg.cmd_n_status_struct.addr_width_override = ADDR_WIDTH_WRITABLE;
    ctrl_reg.cmd_n_status_struct.addr_width = TWO_BYTES;
    eep_data(d, ctrl_reg.cmd_u32, NULL);

    printf("EEPROM was initialized. Please restart your system for changes to take effect.\n");
    fflush(stdout);
}

int pci_get_devtype(struct pci_dev *pdev)
{
  struct pci_cap *cap;
  cap = pci_find_cap(pdev, PCI_CAP_ID_EXP, PCI_CAP_NORMAL);
  int devtype = pci_read_word(pdev, cap->addr + PCI_EXP_FLAGS);
  return ((devtype & PCI_EXP_FLAGS_TYPE) >> 4) & 0xFF;
}

bool pci_is_upstream(struct pci_dev *pdev)
{
  return pci_get_devtype(pdev) == PCI_EXP_TYPE_UPSTREAM;
}

bool pcidev_is_adnacom(struct pci_dev *p)
{
        struct adnatool_pci_device *entry;
        pci_fill_info(p, PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_CLASS);
        for (entry = adnatool_pci_devtbl; entry->vid != 0; entry++) {
                if (p->vendor_id != entry->vid)
                        continue;
                if (p->device_id != entry->did)
                        continue;
                if (p->device_class != entry->cls_rev)
                        continue;
                return true;
        }
        return false;
}

int config_fetch(struct device *d, unsigned int pos, unsigned int len)
{
  unsigned int end = pos+len;
  int result;

  while (pos < d->config_bufsize && len && d->present[pos])
    pos++, len--;
  while (pos+len <= d->config_bufsize && len && d->present[pos+len-1])
    len--;
  if (!len)
    return 1;

  if (end > d->config_bufsize)
    {
      int orig_size = d->config_bufsize;
      while (end > d->config_bufsize)
        d->config_bufsize *= 2;
      d->config = xrealloc(d->config, d->config_bufsize);
      d->present = xrealloc(d->present, d->config_bufsize);
      memset(d->present + orig_size, 0, d->config_bufsize - orig_size);
    }
  result = pci_read_block(d->dev, pos, d->config + pos, len);
  if (result)
    memset(d->present + pos, 1, len);
  return result;
}

struct device *scan_device(struct pci_dev *p)
{
  struct device *d;

  if (p->domain && !opt_domains)
    opt_domains = 1;
  if (!pci_filter_match(&filter, p) && !need_topology)
    return NULL;

  if (!pcidev_is_adnacom(p))
    return NULL;

  d = xmalloc(sizeof(struct device));
  memset(d, 0, sizeof(*d));
  d->dev = p;
  d->config_cached = d->config_bufsize = 256;
  d->config = xmalloc(256);
  d->present = xmalloc(256);
  memset(d->present, 1, 256);

  if (!pci_read_block(p, 0, d->config, 256)) {
    fprintf(stderr, "adna: Unable to read the standard configuration space header of device %04x:%02x:%02x.%d\n",
            p->domain, p->bus, p->dev, p->func);
    seen_errors++;
    return NULL;
  }

  pci_setup_cache(p, d->config, d->config_cached);
  pci_fill_info(p, PCI_FILL_IDENT | PCI_FILL_CLASS);
  return d;
}

static void scan_devices(void)
{
  struct device *d;
  struct pci_dev *p;

  pci_scan_bus(pacc);
  for (p=pacc->devices; p; p=p->next)
    if (d = scan_device(p)) {
      d->next = first_dev;
      first_dev = d;
    }
}

/*** Config space accesses ***/
static void check_conf_range(struct device *d, unsigned int pos, unsigned int len)
{
  while (len)
    if (!d->present[pos])
      die("Internal bug: Accessing non-read configuration byte at position %x", pos);
    else
      pos++, len--;
}

byte get_conf_byte(struct device *d, unsigned int pos)
{
  check_conf_range(d, pos, 1);
  return d->config[pos];
}

word get_conf_word(struct device *d, unsigned int pos)
{
  check_conf_range(d, pos, 2);
  return d->config[pos] | (d->config[pos+1] << 8);
}

u32 get_conf_long(struct device *d, unsigned int pos)
{
  check_conf_range(d, pos, 4);
  return d->config[pos] |
    (d->config[pos+1] << 8) |
    (d->config[pos+2] << 16) |
    (d->config[pos+3] << 24);
}

/*** Sorting ***/
static int compare_them(const void *A, const void *B)
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

static int count_upstream(void)
{
  struct device *d;
  int i=0;
  for (d=first_dev; d; d=d->next) {
    if (pci_is_upstream(d->dev))
      d->NumDevice = ++i;
    else
      d->NumDevice = 0;
  }
  return i;
}

static void sort_them(void)
{
  struct device **index, **h, **last_dev;
  int cnt;
  struct device *d;

  cnt = 0;
  for (d=first_dev; d; d=d->next)
    cnt++;
  h = index = alloca(sizeof(struct device *) * cnt);
  for (d=first_dev; d; d=d->next)
    *h++ = d;
  qsort(index, cnt, sizeof(struct device *), compare_them);
  last_dev = &first_dev;
  h = index;
  while (cnt--) {
    *last_dev = *h;
    last_dev = &(*h)->next;
    h++;
  }
  *last_dev = NULL;
}

/*** Normal output ***/
static void show_slot_path(struct device *d)
{
  struct pci_dev *p = d->dev;

  if (opt_path)
    {
      struct bus *bus = d->parent_bus;
      struct bridge *br = bus->parent_bridge;

      if (br && br->br_dev)
	{
	  show_slot_path(br->br_dev);
	  if (opt_path > 1)
	    printf("/%02x:%02x.%d", p->bus, p->dev, p->func);
	  else
	    printf("/%02x.%d", p->dev, p->func);
	  return;
	}
    }
  if (d->NumDevice)
    printf("[%d]\t", d->NumDevice);
  else
    printf("\t");
  printf("%02x:%02x.%d", p->bus, p->dev, p->func);
}

static void show_slot_name(struct device *d)
{
  struct pci_dev *p = d->dev;

  if (!opt_machine ? opt_domains : (p->domain || opt_domains >= 2))
    printf("%04x:", p->domain);
  show_slot_path(d);
}

void get_subid(struct device *d, word *subvp, word *subdp)
{
  byte htype = get_conf_byte(d, PCI_HEADER_TYPE) & 0x7f;

  if (htype == PCI_HEADER_TYPE_NORMAL)
    {
      *subvp = get_conf_word(d, PCI_SUBSYSTEM_VENDOR_ID);
      *subdp = get_conf_word(d, PCI_SUBSYSTEM_ID);
    }
  else if (htype == PCI_HEADER_TYPE_CARDBUS && d->config_cached >= 128)
    {
      *subvp = get_conf_word(d, PCI_CB_SUBSYSTEM_VENDOR_ID);
      *subdp = get_conf_word(d, PCI_CB_SUBSYSTEM_ID);
    }
  else
    *subvp = *subdp = 0xffff;
}

static void show_terse(struct device *d)
{
  int c;
  struct pci_dev *p = d->dev;
  char classbuf[128], devbuf[128];

  show_slot_name(d);
  printf(" %s: %s",
         pci_lookup_name(pacc, classbuf, sizeof(classbuf),
                         PCI_LOOKUP_CLASS,
                         p->device_class),
         pci_lookup_name(pacc, devbuf, sizeof(devbuf),
                         PCI_LOOKUP_VENDOR | PCI_LOOKUP_DEVICE,
                         p->vendor_id, p->device_id));
  if (c = get_conf_byte(d, PCI_REVISION_ID))
    printf(" (rev %02x)", c);
  if (verbose)
  {
    char *x;
    c = get_conf_byte(d, PCI_CLASS_PROG);
    x = pci_lookup_name(pacc, devbuf, sizeof(devbuf),
                        PCI_LOOKUP_PROGIF | PCI_LOOKUP_NO_NUMBERS,
                        p->device_class, c);
    if (c || x)
    {
      printf(" (prog-if %02x", c);
      if (x)
        printf(" [%s]", x);
      putchar(')');
    }
  }
  putchar('\n');

  if (verbose || opt_kernel)
    {
      word subsys_v, subsys_d;
#ifndef ADNA
      char ssnamebuf[256];
#endif

      pci_fill_info(p, PCI_FILL_LABEL);

      if (p->label)
        printf("\tDeviceName: %s", p->label);
      get_subid(d, &subsys_v, &subsys_d);
#ifndef ADNA
      if (subsys_v && subsys_v != 0xffff)
	printf("\tSubsystem: %s\n",
		pci_lookup_name(pacc, ssnamebuf, sizeof(ssnamebuf),
			PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_VENDOR | PCI_LOOKUP_DEVICE,
			p->vendor_id, p->device_id, subsys_v, subsys_d));
#endif // ADNA
    }
}

/*** Verbose output ***/
static void show_size(u64 x)
{
  static const char suffix[][2] = { "", "K", "M", "G", "T" };
  unsigned i;
  if (!x)
    return;
  for (i = 0; i < (sizeof(suffix) / sizeof(*suffix) - 1); i++) {
    if (x % 1024)
      break;
    x /= 1024;
  }
  printf(" [size=%u%s]", (unsigned)x, suffix[i]);
}
#ifndef ADNA
static void
show_range(char *prefix, u64 base, u64 limit, int is_64bit)
{
  printf("%s:", prefix);
  if (base <= limit || verbose > 2)
    {
      if (is_64bit)
        printf(" %016" PCI_U64_FMT_X "-%016" PCI_U64_FMT_X, base, limit);
      else
        printf(" %08x-%08x", (unsigned) base, (unsigned) limit);
    }
  if (base <= limit)
    show_size(limit - base + 1);
  else
    printf(" [disabled]");
  putchar('\n');
}
#endif
static void show_bases(struct device *d, int cnt)
{
  struct pci_dev *p = d->dev;
  word cmd = get_conf_word(d, PCI_COMMAND);
  int i;
  int virtual = 0;

  for (i=0; i<cnt; i++)
    {
      pciaddr_t pos = p->base_addr[i];
      pciaddr_t len = (p->known_fields & PCI_FILL_SIZES) ? p->size[i] : 0;
      pciaddr_t ioflg = (p->known_fields & PCI_FILL_IO_FLAGS) ? p->flags[i] : 0;
      u32 flg = get_conf_long(d, PCI_BASE_ADDRESS_0 + 4*i);
      u32 hw_lower;
      u32 hw_upper = 0;
      int broken = 0;

      if (flg == 0xffffffff)
	flg = 0;
      if (!pos && !flg && !len)
	continue;

      if (verbose > 1)
	printf("\tRegion %d: ", i);
      else
	putchar('\t');

      /* Read address as seen by the hardware */
      if (flg & PCI_BASE_ADDRESS_SPACE_IO)
	hw_lower = flg & PCI_BASE_ADDRESS_IO_MASK;
      else
	{
	  hw_lower = flg & PCI_BASE_ADDRESS_MEM_MASK;
	  if ((flg & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64)
	    {
	      if (i >= cnt - 1)
		broken = 1;
	      else
		{
		  i++;
		  hw_upper = get_conf_long(d, PCI_BASE_ADDRESS_0 + 4*i);
		}
	    }
	}

      /* Detect virtual regions, which are reported by the OS, but unassigned in the device */
      if (pos && !hw_lower && !hw_upper && !(ioflg & PCI_IORESOURCE_PCI_EA_BEI))
	{
	  flg = pos;
	  virtual = 1;
	}

      /* Print base address */
      if (flg & PCI_BASE_ADDRESS_SPACE_IO)
	{
	  pciaddr_t a = pos & PCI_BASE_ADDRESS_IO_MASK;
	  printf("I/O ports at ");
	  if (a || (cmd & PCI_COMMAND_IO))
	    printf(PCIADDR_PORT_FMT, a);
	  else if (hw_lower)
	    printf("<ignored>");
	  else
	    printf("<unassigned>");
	  if (virtual)
	    printf(" [virtual]");
	  else if (!(cmd & PCI_COMMAND_IO))
	    printf(" [disabled]");
	}
      else
	{
	  int t = flg & PCI_BASE_ADDRESS_MEM_TYPE_MASK;
	  pciaddr_t a = pos & PCI_ADDR_MEM_MASK;

	  printf("Memory at ");
	  if (broken)
	    printf("<broken-64-bit-slot>");
	  else if (a)
	    printf(PCIADDR_T_FMT, a);
	  else if (hw_lower || hw_upper)
	    printf("<ignored>");
	  else
	    printf("<unassigned>");
	  printf(" (%s, %sprefetchable)",
		 (t == PCI_BASE_ADDRESS_MEM_TYPE_32) ? "32-bit" :
		 (t == PCI_BASE_ADDRESS_MEM_TYPE_64) ? "64-bit" :
		 (t == PCI_BASE_ADDRESS_MEM_TYPE_1M) ? "low-1M" : "type 3",
		 (flg & PCI_BASE_ADDRESS_MEM_PREFETCH) ? "" : "non-");
	  if (virtual)
	    printf(" [virtual]");
	  else if (!(cmd & PCI_COMMAND_MEMORY))
	    printf(" [disabled]");
	}

      if (ioflg & PCI_IORESOURCE_PCI_EA_BEI)
	printf(" [enhanced]");

      show_size(len);
      putchar('\n');
    }
}
#ifndef ADNA
static void
show_rom(struct device *d, int reg)
{
  struct pci_dev *p = d->dev;
  pciaddr_t rom = p->rom_base_addr;
  pciaddr_t len = (p->known_fields & PCI_FILL_SIZES) ? p->rom_size : 0;
  pciaddr_t ioflg = (p->known_fields & PCI_FILL_IO_FLAGS) ? p->rom_flags : 0;
  u32 flg = get_conf_long(d, reg);
  word cmd = get_conf_word(d, PCI_COMMAND);
  int virtual = 0;

  if (!rom && !flg && !len)
    return;

  if ((rom & PCI_ROM_ADDRESS_MASK) && !(flg & PCI_ROM_ADDRESS_MASK) && !(ioflg & PCI_IORESOURCE_PCI_EA_BEI))
    {
      flg = rom;
      virtual = 1;
    }

  printf("\tExpansion ROM at ");
  if (rom & PCI_ROM_ADDRESS_MASK)
    printf(PCIADDR_T_FMT, rom & PCI_ROM_ADDRESS_MASK);
  else if (flg & PCI_ROM_ADDRESS_MASK)
    printf("<ignored>");
  else
    printf("<unassigned>");

  if (virtual)
    printf(" [virtual]");

  if (!(flg & PCI_ROM_ADDRESS_ENABLE))
    printf(" [disabled]");
  else if (!virtual && !(cmd & PCI_COMMAND_MEMORY))
    printf(" [disabled by cmd]");

  if (ioflg & PCI_IORESOURCE_PCI_EA_BEI)
      printf(" [enhanced]");

  show_size(len);
  putchar('\n');
}
#endif // ADNA
static void show_htype0(struct device *d)
{
#ifndef ADNA
  show_bases(d, 6);
  show_rom(d, PCI_ROM_ADDRESS);
#endif // ADNA
  show_caps(d, PCI_CAPABILITY_LIST);
}

static void show_htype1(struct device *d)
{
  show_caps(d, PCI_CAPABILITY_LIST);
}

static void show_htype2(struct device *d)
{
  int i;
  word cmd = get_conf_word(d, PCI_COMMAND);
  word brc = get_conf_word(d, PCI_CB_BRIDGE_CONTROL);
  word exca;
  int verb = verbose > 2;

  show_bases(d, 1);
  printf("\tBus: primary=%02x, secondary=%02x, subordinate=%02x, sec-latency=%d\n",
	 get_conf_byte(d, PCI_CB_PRIMARY_BUS),
	 get_conf_byte(d, PCI_CB_CARD_BUS),
	 get_conf_byte(d, PCI_CB_SUBORDINATE_BUS),
	 get_conf_byte(d, PCI_CB_LATENCY_TIMER));
  for (i=0; i<2; i++)
    {
      int p = 8*i;
      u32 base = get_conf_long(d, PCI_CB_MEMORY_BASE_0 + p);
      u32 limit = get_conf_long(d, PCI_CB_MEMORY_LIMIT_0 + p);
      limit = limit + 0xfff;
      if (base <= limit || verb)
	printf("\tMemory window %d: %08x-%08x%s%s\n", i, base, limit,
	       (cmd & PCI_COMMAND_MEMORY) ? "" : " [disabled]",
	       (brc & (PCI_CB_BRIDGE_CTL_PREFETCH_MEM0 << i)) ? " (prefetchable)" : "");
    }
  for (i=0; i<2; i++)
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

  if (d->config_cached < 128)
    {
      printf("\t<access denied to the rest>\n");
      return;
    }

  exca = get_conf_word(d, PCI_CB_LEGACY_MODE_BASE);
  if (exca)
    printf("\t16-bit legacy interface ports at %04x\n", exca);
  show_caps(d, PCI_CB_CAPABILITY_LIST);
}

static void show_verbose(struct device *d)
{
  struct pci_dev *p = d->dev;
  word class = p->device_class;
  byte htype = get_conf_byte(d, PCI_HEADER_TYPE) & 0x7f;
  unsigned int irq;
  byte max_lat, min_gnt;
  char *dt_node;

#ifndef ADNA
  char *iommu_group;
  byte int_pin = get_conf_byte(d, PCI_INTERRUPT_PIN);
  byte latency = get_conf_byte(d, PCI_LATENCY_TIMER);
  byte cache_line = get_conf_byte(d, PCI_CACHE_LINE_SIZE);
  byte bist = get_conf_byte(d, PCI_BIST);
  word status = get_conf_word(d, PCI_STATUS);
#else
  (void)(min_gnt);
  (void)(irq);
#endif

  show_terse(d);

  word cmd = get_conf_word(d, PCI_COMMAND);

  if ((FLAG(cmd, PCI_COMMAND_IO) == '-') ||
      (FLAG(cmd, PCI_COMMAND_MEMORY) == '-') ||
      (FLAG(cmd, PCI_COMMAND_MASTER) == '-') ) {
    byte command = (byte)(cmd | 0x7);
    pci_write_byte(d->dev, PCI_COMMAND, command);
  }

  pci_fill_info(p, PCI_FILL_IRQ | PCI_FILL_BASES | PCI_FILL_ROM_BASE | PCI_FILL_SIZES |
    PCI_FILL_PHYS_SLOT | PCI_FILL_NUMA_NODE | PCI_FILL_DT_NODE | PCI_FILL_IOMMU_GROUP);
  irq = p->irq;

  switch (htype)
  {
  case PCI_HEADER_TYPE_NORMAL:
    if (class == PCI_CLASS_BRIDGE_PCI)
      printf("\t!!! Invalid class %04x for header type %02x\n", class, htype);
    max_lat = get_conf_byte(d, PCI_MAX_LAT);
    min_gnt = get_conf_byte(d, PCI_MIN_GNT);
    break;
  case PCI_HEADER_TYPE_BRIDGE:
    if ((class >> 8) != PCI_BASE_CLASS_BRIDGE)
      printf("\t!!! Invalid class %04x for header type %02x\n", class, htype);
    min_gnt = max_lat = 0;
    break;
  case PCI_HEADER_TYPE_CARDBUS:
    if ((class >> 8) != PCI_BASE_CLASS_BRIDGE)
      printf("\t!!! Invalid class %04x for header type %02x\n", class, htype);
    min_gnt = max_lat = 0;
    break;
  default:
    printf("\t!!! Unknown header type %02x\n", htype);
    return;
  }

  if (p->phy_slot)
    printf("\tPhysical Slot: %s\n", p->phy_slot);

  if (dt_node = pci_get_string_property(p, PCI_FILL_DT_NODE))
    printf("\tDevice tree node: %s\n", dt_node);

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
  printf("\n");
}

/*** Machine-readable dumps ***/
static void show_hex_dump(struct device *d)
{
  unsigned int i, cnt;

  cnt = d->config_cached;
  if (opt_hex >= 3 && config_fetch(d, cnt, 256-cnt))
    {
      cnt = 256;
      if (opt_hex >= 4 && config_fetch(d, 256, 4096-256))
        cnt = 4096;
    }

  for (i=0; i<cnt; i++)
    {
      if (! (i & 15))
        printf("%02x:", i);
      printf(" %02x", get_conf_byte(d, i));
      if ((i & 15) == 15)
        putchar('\n');
    }
}

static void print_shell_escaped(char *c)
{
  printf(" \"");
  while (*c)
    {
      if (*c == '"' || *c == '\\')
	putchar('\\');
      putchar(*c++);
    }
  putchar('"');
}

static void show_machine(struct device *d)
{
  struct pci_dev *p = d->dev;
  int c;
  word sv_id, sd_id;
  char classbuf[128], vendbuf[128], devbuf[128], svbuf[128], sdbuf[128];
  char *dt_node, *iommu_group;

  get_subid(d, &sv_id, &sd_id);

  if (verbose)
    {
      pci_fill_info(p, PCI_FILL_PHYS_SLOT | PCI_FILL_NUMA_NODE | PCI_FILL_DT_NODE | PCI_FILL_IOMMU_GROUP);
      printf((opt_machine >= 2) ? "Slot:\t" : "Device:\t");
      show_slot_name(d);
      putchar('\n');
      printf("Class:\t%s\n",
	     pci_lookup_name(pacc, classbuf, sizeof(classbuf), PCI_LOOKUP_CLASS, p->device_class));
      printf("Vendor:\t%s\n",
	     pci_lookup_name(pacc, vendbuf, sizeof(vendbuf), PCI_LOOKUP_VENDOR, p->vendor_id, p->device_id));
      printf("Device:\t%s\n",
	     pci_lookup_name(pacc, devbuf, sizeof(devbuf), PCI_LOOKUP_DEVICE, p->vendor_id, p->device_id));
      if (sv_id && sv_id != 0xffff)
	{
	  printf("SVendor:\t%s\n",
		 pci_lookup_name(pacc, svbuf, sizeof(svbuf), PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_VENDOR, sv_id));
	  printf("SDevice:\t%s\n",
		 pci_lookup_name(pacc, sdbuf, sizeof(sdbuf), PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_DEVICE, p->vendor_id, p->device_id, sv_id, sd_id));
	}
      if (p->phy_slot)
	printf("PhySlot:\t%s\n", p->phy_slot);
      if (c = get_conf_byte(d, PCI_REVISION_ID))
	printf("Rev:\t%02x\n", c);
      if (c = get_conf_byte(d, PCI_CLASS_PROG))
	printf("ProgIf:\t%02x\n", c);
      if (opt_kernel)
	show_kernel_machine(d);
      if (p->numa_node != -1)
	printf("NUMANode:\t%d\n", p->numa_node);
      if (dt_node = pci_get_string_property(p, PCI_FILL_DT_NODE))
        printf("DTNode:\t%s\n", dt_node);
      if (iommu_group = pci_get_string_property(p, PCI_FILL_IOMMU_GROUP))
	printf("IOMMUGroup:\t%s\n", iommu_group);
    }
  else
    {
      show_slot_name(d);
      print_shell_escaped(pci_lookup_name(pacc, classbuf, sizeof(classbuf), PCI_LOOKUP_CLASS, p->device_class));
      print_shell_escaped(pci_lookup_name(pacc, vendbuf, sizeof(vendbuf), PCI_LOOKUP_VENDOR, p->vendor_id, p->device_id));
      print_shell_escaped(pci_lookup_name(pacc, devbuf, sizeof(devbuf), PCI_LOOKUP_DEVICE, p->vendor_id, p->device_id));
      if (c = get_conf_byte(d, PCI_REVISION_ID))
	printf(" -r%02x", c);
      if (c = get_conf_byte(d, PCI_CLASS_PROG))
	printf(" -p%02x", c);
      if (sv_id && sv_id != 0xffff)
	{
	  print_shell_escaped(pci_lookup_name(pacc, svbuf, sizeof(svbuf), PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_VENDOR, sv_id));
	  print_shell_escaped(pci_lookup_name(pacc, sdbuf, sizeof(sdbuf), PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_DEVICE, p->vendor_id, p->device_id, sv_id, sd_id));
	}
      else
	printf(" \"\" \"\"");
      putchar('\n');
    }
}

/*** Main show function ***/
void show_device(struct device *d)
{
  if (opt_machine)
    show_machine(d); // not used by Adna
  else
  {
    if (verbose)
      show_verbose(d);
    else 
      show_terse(d);
#ifndef ADNA
    if (opt_kernel || verbose)
      show_kernel(d);
#endif // ADNA
  }
  if (opt_hex)
    show_hex_dump(d);
  if (verbose || opt_hex)
    putchar('\n');
}

static void show(void)
{
  struct device *d;

  for (d=first_dev; d; d=d->next)
    if (pci_filter_match(&filter, d->dev))
      show_verbose(d);
}

static int delete_adna_list(void)
{
  struct adna_device *a, *b;
  for (a=first_adna;a;a=b) {
    b=a->next;
    free(a);
  }
  return 0;
}

static int save_to_adna_list(void)
{
  struct device *d;
  struct adna_device *a;

  for (d=first_dev; d; d=d->next) {
    if (d->NumDevice) {
      a = xmalloc(sizeof(struct adna_device));
      memset(a, 0, sizeof(*a));
      a->devnum = d->NumDevice;
      a->bus = d->dev->bus;
      a->dev = d->dev->dev;
      a->func = d->dev->func;
      a->bIsD3 = false;
      a->next = first_adna;
      first_adna = a;
    }
  }
  return 0;
}

static int adna_pacc_cleanup(void)
{
  show_kernel_cleanup();
  pci_cleanup(pacc);
  return 0;
}

static int adna_pacc_init(void)
{
  pacc = pci_alloc();
  pacc->error = die;
  pci_filter_init(pacc, &filter);
  pci_init(pacc);
  return 0;
}

static int adna_pci_process(void)
{
  adna_pacc_init();
  scan_devices();
  sort_them();

  NumDevices = count_upstream();
  if (NumDevices == 0) {
    printf("No Adnacom device detected.\n");
    return -1;
  }

  save_to_adna_list();
  show();

  adna_pacc_cleanup();

  return 0;
}

void adna_set_d3_flag(int devnum)
{
  struct adna_device *a;
  for (a = first_adna; a; a=a->next) {
    if (a->devnum == devnum)
      a->bIsD3 = true;
  }
}

static int adna_d3_to_d0(void)
{
  struct adna_device *a;
  char *argv[4];
  int status = EXIT_SUCCESS;

  for (int i = 0; i < 4; i++) {
    argv[i] = malloc(14);
  }

  snprintf(argv[0],
           14,
           "%s",
           "setpci");

  snprintf(argv[1],
           14,
           "-s");
  snprintf(argv[3],
           14,
           "%s",
           "CAP_PM+4.b=0");

  for (a=first_adna; a; a=a->next) {
    if (a->bIsD3 == true) {
      snprintf(argv[2], 
               14,
               "%02x:%02x.%d",
               a->bus,
               a->dev,
               a->func);
      status = setpci(4, argv);
      if (EXIT_FAILURE == status)
        return status;
    }
  }

  for (int i = 0; i < 4; i++) {
    free(argv[i]);
  }

  return status;
}

static void str_to_bin(char *binary_data, const char *serialnumber)
{
  // Initialize the binary_data buffer
  memset(binary_data, 0, 4);

  // Iterate through each pair of characters in the hexadecimal input
  for (int i = 0; i < 4; i++) {
    // Extract a pair of characters from the hexadecimal string
    char hex_pair[3];
    strncpy(hex_pair, serialnumber + (i * 2), 2);
    hex_pair[2] = '\0';

    // Convert the hex_pair to an integer
    unsigned int hex_value;
    if (sscanf(hex_pair, "%x", &hex_value) != 1) {
      fprintf(stderr, "Error: Invalid hexadecimal input.\n");
      exit(1);
    }

    // Store the integer value in the binary_data buffer
    binary_data[i] = (char)hex_value;
  }
}

static int is_valid_hex(const char *serialnumber) {
    // Check if the input is a valid hexadecimal value and its length is even (byte-aligned)
    for (int i = 0; serialnumber[i] != '\0'; i++) {
        if (!isxdigit(serialnumber[i])) {
            return 0; // Not a valid hexadecimal character
        }
    }
    return 1; // Valid hexadecimal value
}

static uint8_t EepromFileLoad(struct device *d)
{
    printf("Function: %s\n", __func__);
    uint8_t rc;
    uint8_t four_byte_count;
    uint16_t Verify_Value_16 = 0;
    uint32_t value;
    uint32_t Verify_Value = 0;
    uint32_t offset;
    uint32_t FileSize;
    FILE *pFile;

    g_pBuffer   = NULL;

    printf("Load EEPROM file... \n");
    fflush(stdout);

    // Open the file to read
    pFile = fopen(EepOptions.FileName, "rb");
    if (pFile == NULL) {
        printf("ERROR: Unable to load \"%s\"\n", EepOptions.FileName);
        return EEP_FAIL;
    }

    // Move to end-of-file
    fseek(pFile, 0, SEEK_END);

    // Determine file size
    FileSize = ftell(pFile);

    // Move back to start of file
    fseek(pFile, 0, SEEK_SET);

    // Allocate a buffer for the data
    g_pBuffer = malloc(FileSize);
    if (g_pBuffer == NULL) {
        fclose(pFile);
        return EEP_FAIL;
    }

    // Read data from file
    if (fread(
            g_pBuffer,        // Buffer for data
            sizeof(uint8_t),// Item size
            FileSize,       // Buffer size
            pFile           // File pointer
            ) <= 0) {
        // Added for compiler warning
    }

    // Close the file
    fclose(pFile);

    printf("Ok (%dB)\n", (int)FileSize);

    // Load serial number
    printf("Load Serial Number\n");
    for (uint8_t i = 0; i < FileSize; i++) {
      if ((g_pBuffer[i] == 0x42) && 
          (g_pBuffer[i+1] == 0x00)) {
        g_pBuffer[i+5] = EepOptions.SerialNumber[0];
        g_pBuffer[i+4] = EepOptions.SerialNumber[1];
        g_pBuffer[i+3] = EepOptions.SerialNumber[2];
        g_pBuffer[i+2] = EepOptions.SerialNumber[3];
        break;
      }
    }
    printf("Ok\n");

    // Default to successful operation
    rc = EXIT_SUCCESS;

    printf("Program EEPROM..... \n");

    // Write 32-bit aligned buffer into EEPROM
    for (offset = 0, four_byte_count = 0; offset < (FileSize & ~0x3); four_byte_count++, offset += sizeof(uint32_t))
    {
        // Periodically update status
        if ((offset & 0x7) == 0) {
            // Display current status
            printf("%02u%%\b\b\b", ((offset * 100) / FileSize));
            fflush( stdout );
        }

        // Get next value
        value = *(uint32_t*)(g_pBuffer + offset);

        // Write value & read back to verify
        eep_write(d, four_byte_count, value);
        eep_read(d, four_byte_count, &Verify_Value);

        if (Verify_Value != value) {
            printf("ERROR: offset:%02X  wrote:%08X  read:%08X\n",
                   offset, value, Verify_Value);
            rc = EEP_FAIL;
            goto _Exit_File_Load;
        }
    }

    // Write any remaining 16-bit unaligned value
    if (offset < FileSize) {
        // Get next value
        value = *(uint16_t*)(g_pBuffer + offset);

        // Write value & read back to verify
        eep_write_16(d, offset, (uint16_t)value);
        eep_read_16(d, offset, &Verify_Value_16);

        if (Verify_Value_16 != (uint16_t)value) {
            printf("ERROR: offset:%02X  wrote:%04X  read:%04X\n",
                   offset, value, Verify_Value_16);
            goto _Exit_File_Load;
        }
    }
    printf("Ok \n");

_Exit_File_Load:
    // Release the buffer
    if (g_pBuffer != NULL) {
        free(g_pBuffer);
    }

    return rc;
}

static uint8_t EepromFileSave(struct device *d)
{
    printf("Function: %s\n", __func__);
    volatile uint32_t value = 0;
    uint32_t offset;
    uint8_t four_byte_count;
    uint32_t EepSize;
    FILE *pFile;

    printf("Get EEPROM data size.. \n");

    g_pBuffer = NULL;

    // Start with EEPROM header size
    EepSize = sizeof(uint32_t);

    // Get EEPROM header
    eep_read(d, 0x0, &value);

    // Add register byte count
    EepSize += (value >> 16);

    printf("Ok (%d Bytes", EepSize);

    /* ExtraBytes may not be needed */
    if (EepOptions.ExtraBytes) {
        printf(" + %dB extra", EepOptions.ExtraBytes);

        // Adjust for extra bytes
        EepSize += EepOptions.ExtraBytes;

        // Make sure size aligned on 16-bit boundary
        EepSize = (EepSize + 1) & ~(uint32_t)0x1;
    }
    printf(")\n");

    printf("Read EEPROM data...... \n");
    fflush(stdout);

    // Allocate a buffer for the EEPROM data
    g_pBuffer = malloc(EepSize);
    if (g_pBuffer == NULL) {
        return EEP_FAIL;
    }

    // Each EEPROM read via BAR0 is 4 bytes so offset is represented in bytes (aligned in 32 bits)
    // while four_byte_count is represented in count of 4-byte access
    for (offset = 0, four_byte_count = 0; offset < (EepSize & ~0x3); offset += sizeof(uint32_t), four_byte_count++) {
        eep_read(d, four_byte_count, (uint32_t*)(g_pBuffer + offset));
    }

    // Read any remaining 16-bit aligned byte
    if (offset < EepSize) {
        eep_read_16(d, four_byte_count, (uint16_t*)(g_pBuffer + offset));
    }
    printf("Ok\n");

    if ((EepOptions.bSerialNumber == false) && 
        (EepOptions.bLoadFile == false)) {
      printf("Write data to file.... \n");
      fflush(stdout);

      // Open the file to write
      pFile = fopen(EepOptions.FileName, "wb");
      if (pFile == NULL) {
          return EEP_FAIL;
      }

      // Write buffer to file
      fwrite(
          g_pBuffer,        // Buffer to write
          sizeof(uint8_t),     // Item size
          EepSize,        // Buffer size
          pFile           // File pointer
          );

      // Close the file
      fclose(pFile);
    } else if ((EepOptions.bSerialNumber == false) && 
               (EepOptions.bLoadFile == true)) {
      // Save serial number
      printf("Save Serial Number to buffer\n");
      for (uint8_t i = 0; i < EepSize; i++) {
        if ((g_pBuffer[i] == 0x42) && 
            (g_pBuffer[i+1] == 0x00)) {
          EepOptions.SerialNumber[0] = g_pBuffer[i+5];
          EepOptions.SerialNumber[1] = g_pBuffer[i+4];
          EepOptions.SerialNumber[2] = g_pBuffer[i+3];
          EepOptions.SerialNumber[3] = g_pBuffer[i+2];
          break;
        }
      }
    } else {}

    // Release the buffer
    if (g_pBuffer != NULL) {
        free(g_pBuffer);
    }

    printf("Ok %s\n", (EepOptions.bLoadFile == true) ? "" : EepOptions.FileName);

    return EXIT_SUCCESS;
}

static uint8_t EepFile(struct device *d)
{
  if (EepOptions.bVerbose)
    printf("Function: %s\n", __func__);
  if (EepOptions.bLoadFile) {
      if (EepOptions.bSerialNumber == false) {
        printf("Get Serial Number from device\n");
        EepromFileSave(d);
      }
      return EepromFileLoad(d);
  } else {
      return EepromFileSave(d);
  }
}

static int eep_process(int j)
{
  struct device *d;
  struct adna_device *a;
  int eep_present = EEP_PRSNT_MAX;
  uint32_t read;
  int status = EXIT_FAILURE;

  adna_pacc_init();
  scan_devices();
  sort_them();

  for (a=first_adna; a; a=a->next) { // loop through adnacom device list
    if (j == a->devnum) {          // to locate the target NumDevice
      for (d=first_dev; d; d=d->next) { // loop through the pacc list
        if ((a->bus == d->dev->bus) &&
            (a->dev == d->dev->dev) &&
            (a->func == d->dev->func)) { // to locate the pci dev
          check_for_ready_or_done(d);
          read = pcimem(d->dev, EEP_STAT_N_CTRL_ADDR, 0);
          check_for_ready_or_done(d);
          if (read == PCI_MEM_ERROR) {
            printf("Unexpected error. Exiting.\n");
            exit(-1);
          }

          eep_present = (read >> EEP_PRSNT_OFFSET) & 3;;

          switch (eep_present) {
          case NOT_PRSNT:
              printf("No EEPROM Present\n");
          break;
          case PRSNT_VALID:
              printf("EEPROM present with valid data\n");
              status = EXIT_SUCCESS;
          break;
          case PRSNT_INVALID:
              printf("Present but invalid data/CRC error/blank\n");
              eep_init(d);
              printf("EEPROM initialization done, please restart your computer.\n");
          break;
          }

          if (EXIT_SUCCESS == status) {
              status = EepFile(d);
          } else {
              return status;
          }
        }
      }
    }
  }

  adna_pacc_cleanup();
  return status;
}

static void DisplayHelp(void)
{
    printf(
        "\n"
        "EEPROM file utility for Adnacom devices.\n"
        "\n"
        " Usage: adna [-w|-s file | -e] [-n serial_num] [-v]\n"
        "\n"
        " Options:\n"
        "   -w | -s       Write (-w) file to EEPROM -OR- Save (-s) EEPROM to file\n"
        "   file          Specifies the file to load or save\n"
        "   -e            Enumerate (-e) Adnacom devices\n"
        "   -n            Specifies the serial number to write\n"
        "   -v            Verbose output (for debug purposes)\n"
        "   -h or -?      This help screen\n"
        "\n"
        "  Sample command\n"
        "  -----------------\n"
        "  adna -w MyEeprom.bin\n"
        "\n"
        );
}

static uint8_t ProcessCommandLine(int argc, char *argv[])
{
    uint16_t i;
    bool bGetFileName;
    bool bGetSerialNumber;
    bGetFileName  = false;
    bGetSerialNumber = false;
    for (i = 1; i < argc; i++) {
        if (bGetFileName) {
            if (argv[i][0] == '-') {
                printf("ERROR: File name not specified\n");
                return CMD_LINE_ERR;
            }

            // Get file name
            strcpy(EepOptions.FileName, argv[i]);

            // Flag parameter retrieved
            bGetFileName = false;
        } else if (bGetSerialNumber) {
            if (argv[i][0] == '-') {
                printf("ERROR: Serial number not specified\n");
                return CMD_LINE_ERR;
            }

            if (strlen(argv[i]) != 8) {
                printf("ERROR: Serial number input should be 8 characters long.\n");
                return CMD_LINE_ERR;
            }

            if (!is_valid_hex(argv[i])) {
                printf("ERROR: Invalid hexadecimal input. It should be a valid hexadecimal input (e.g., 0011AABB)\n");
                return CMD_LINE_ERR;
            }

            // Get serial number
            str_to_bin(EepOptions.SerialNumber, argv[i]);

            // Flag parameter retrieved
            bGetSerialNumber = false;
        } else if ((strcasecmp(argv[i], "-?") == 0) ||
                   (strcasecmp(argv[i], "-h") == 0)) {
            
            DisplayHelp();
            return EXIT_FAILURE;
        } else if (strcasecmp(argv[i], "-v") == 0) {
            EepOptions.bVerbose = true;
        } else if (strcasecmp(argv[i], "-w") == 0) {
            EepOptions.bLoadFile = true;

            // Set flag to get file name
            bGetFileName = true;
        } else if (strcasecmp(argv[i], "-s") == 0) {
            EepOptions.bLoadFile = false;
            EepOptions.bSerialNumber = false;

            // Set flag to get file name
            bGetFileName = true;
        } else if (strcasecmp(argv[i], "-e") == 0) {
            EepOptions.bListOnly = true;
        } else if (strcasecmp(argv[i], "-n") == 0) {
            EepOptions.bSerialNumber = true;
            bGetSerialNumber = true;
        } else {
            printf("ERROR: Invalid argument \'%s\'\n", argv[i]);
            return CMD_LINE_ERR;
        }

        // Make sure next parameter exists
        if ((i + 1) == argc) {
            if (bGetFileName) {
                printf("ERROR: File name not specified\n");
                return CMD_LINE_ERR;
            }

            if (bGetSerialNumber) {
                printf("ERROR: Serial number not specified\n");
                return CMD_LINE_ERR;
            }
        }
    }

    // Make sure required parameters were provided
    if (EepOptions.bListOnly  == true) {
        // Allow list only
    } else if ((EepOptions.bLoadFile == 0xFF) || (EepOptions.FileName[0] == '\0')) {
        printf("ERROR: EEPROM operation not specified. Use 'adna -h' for usage.\n");
        return EXIT_FAILURE;
    } else if ((EepOptions.bLoadFile == false) && (EepOptions.bSerialNumber == true)) {
        printf("WARNING: Serial number parameter on Save command will be ignored.\n");
    } else {}

    return EXIT_SUCCESS;
}

/* Main */
int main(int argc, char **argv)
{
  verbose = 2; // flag used by pci process
  int status = EXIT_SUCCESS;

  if (argc == 2 && !strcmp(argv[1], "--version")) {
    puts("Adnacom version " ADNATOOL_VERSION);
    return 0;
  }

  status = ProcessCommandLine(argc, argv);
  if (status != EXIT_SUCCESS)
    exit(1);

  status = adna_pci_process();
  if (status != EXIT_SUCCESS)
    exit(1);

  status = adna_d3_to_d0();
  if (status != EXIT_SUCCESS)
    exit(1);

  if (EepOptions.bListOnly == true)
    goto __exit;

  printf("[0] Cancel\n\n");
  char line[10];
  int num;
  printf("    Device selection --> ");
  while (fgets(line, sizeof(line), stdin) != NULL) {
    if (sscanf(line, "%d", &num) == 1) {
      if ((num == 0) ||
          (num > NumDevices)) {
            goto __exit;
      } else {
        break;
      }
    } else {
      printf("    Invalid input\n");
      goto __exit;
    }
  }

  status = eep_process(num);
  if (status == EXIT_FAILURE)
    goto __exit;

__exit:
  delete_adna_list();
  return (seen_errors ? 2 : 0);
}
