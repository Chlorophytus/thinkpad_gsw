#include <asm/io.h>
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/vga_switcheroo.h>

#define THINKPAD_GSW_VERSION "0.11"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Roland Metivier <metivier.roland@chlorophyt.us>");
MODULE_DESCRIPTION("Toggles the ThinkPad PMH7's discrete graphics card power");
MODULE_VERSION(THINKPAD_GSW_VERSION);

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define THINKPAD_GSW_EC_LENOVO_PMH7_BASE 0x15e0
#define THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_L                                     \
  (THINKPAD_GSW_EC_LENOVO_PMH7_BASE + 0x0c)
#define THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_H                                     \
  (THINKPAD_GSW_EC_LENOVO_PMH7_BASE + 0x0d)
#define THINKPAD_GSW_EC_LENOVO_PMH7_DATA                                       \
  (THINKPAD_GSW_EC_LENOVO_PMH7_BASE + 0x0e)
#define THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_L 0x50
#define THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_H 0x00
#define THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_WRITE_BIT 7
#define THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_POWER_BIT 3

// Returns 1 if that DGPU control bit is set
static int thinkpad_gsw_dgpu_raw_peek(unsigned char bit) {
  outb(THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_L, THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_L);
  outb(THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_H, THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_H);
  unsigned char result = inb(THINKPAD_GSW_EC_LENOVO_PMH7_DATA);
  result &= 1 << bit;
  return result != 0x00;
}
// Sets (set != 0) some DGPU control bit, or clears (set == 0) it
static void thinkpad_gsw_dgpu_raw_poke(unsigned char bit, int set) {
  outb(THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_L, THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_L);
  outb(THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_H, THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_H);
  unsigned char result = inb(THINKPAD_GSW_EC_LENOVO_PMH7_DATA);
  if (set) {
    result |= 1 << bit;
  } else {
    result &= ~(1 << bit);
  }

  outb(THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_L, THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_L);
  outb(THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_H, THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_H);
  outb(result, THINKPAD_GSW_EC_LENOVO_PMH7_DATA);
}

// The good stuff
static void thinkpad_gsw_dgpu_disable(void) {
  pr_info("Disabling discrete GPU power rails\n");
  thinkpad_gsw_dgpu_raw_poke(7, 0);
  usleep_range(1000, 2000);
  thinkpad_gsw_dgpu_raw_poke(3, 0);
  msleep(100);
}
static void thinkpad_gsw_dgpu_enable(void) {
  pr_info("Enabling discrete GPU power rails\n");
  thinkpad_gsw_dgpu_raw_poke(7, 0);
  thinkpad_gsw_dgpu_raw_poke(3, 1);
  usleep_range(10000, 20000);
  thinkpad_gsw_dgpu_raw_poke(7, 1);
  msleep(100);
}

enum vga_switcheroo_client_id
thinkpad_gsw_switcheroo_get_client_id(struct pci_dev *pdev) {
  switch (pdev->vendor) {
  case PCI_VENDOR_ID_NVIDIA: {
    return VGA_SWITCHEROO_DIS;
  }
  case PCI_VENDOR_ID_INTEL: {
    return VGA_SWITCHEROO_IGD;
  }
  default: {
    pr_warn("Unrecognized GPU vendor ID\n");
    return VGA_SWITCHEROO_UNKNOWN_ID;
  }
  }
}

static int thinkpad_gsw_switcheroo_switchto(enum vga_switcheroo_client_id id) {
  // This Power Management IC is not a multiplexer
  // Just NO-OP here...
  return 0;
}

static int
thinkpad_gsw_switcheroo_power_state(enum vga_switcheroo_client_id id,
                                    enum vga_switcheroo_state state) {
  if (id == VGA_SWITCHEROO_DIS) {
    switch (state) {
    case VGA_SWITCHEROO_OFF: {
      thinkpad_gsw_dgpu_disable();
      return 0;
    }
    case VGA_SWITCHEROO_ON: {
      thinkpad_gsw_dgpu_enable();
      return 0;
    }
    default: {
      pr_warn("Unrecognized power state: %d\n", id);
      return -EINVAL;
    }
    }
  } else {
    return 0;
  }
}

static struct vga_switcheroo_handler thinkpad_gsw_switcheroo{
    .init = NULL,
    .switchto = &thinkpad_gsw_switcheroo_switchto,
    .switch_ddc = NULL,
    .power_state = &thinkpad_gsw_switcheroo_power_state,
    .get_client_id = &thinkpad_gsw_switcheroo_get_client_id,
};

static int __init thinkpad_gsw_init(void) {
  pr_info("ThinkPad PMH7 graphics switcher v" THINKPAD_GSW_VERSION "\n");

  int handler_ok = vga_switcheroo_register_handler(&thinkpad_gsw_switcheroo, 0);

  if(handler_ok < 0) {
    pr_err("Registering vga_switcheroo handler failed with error %d\n", handler_ok);
    return handler_ok;
  }
  
  return 0;
}

static void __exit thinkpad_gsw_exit(void) {
  vga_switcheroo_unregister_handler();
}

module_init(thinkpad_gsw_init);
module_exit(thinkpad_gsw_exit);
