#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/sysfs.h>
#include <linux/suspend.h>
#include <linux/pm_runtime.h>
#include <linux/acpi.h>
#include <asm/io.h>

#define THINKPAD_GSW_VERSION "0.10.4"

enum {
    GSW_DONT_CARE = -1,
    GSW_OFF = 0,
    GSW_ON = 1,
};

static int load_state = GSW_DONT_CARE;
static int unload_state = GSW_DONT_CARE;
static struct pci_dev *dis_dev;

// Linux gets confused about GPU PCI device state without this.
static acpi_handle dis_handle;

static struct kobject *gsw_kobj;

/* whether the card was off before suspend or not; on: 0, off: 1 */
static int dis_before_suspend_disabled;

struct kobject *power_kobj;

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Roland Metivier <metivier.roland@chlorophyt.us>");
MODULE_AUTHOR("Peter Wu <lekensteyn@gmail.com>");
MODULE_DESCRIPTION("Toggles the ThinkPad PMH7's discrete graphics card power");
MODULE_VERSION(THINKPAD_GSW_VERSION);
MODULE_PARM_DESC(load_state, "GPU power state when loaded (-1 = don't care, 0 = off, 1 = on)");
module_param(load_state, int, 0400);
MODULE_PARM_DESC(unload_state, "GPU power state when unloaded (-1 = don't care, 0 = off, 1 = on)");
module_param(unload_state, int, 0400);

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define THINKPAD_GSW_EC_LENOVO_PMH7_BASE 0x15e0
#define THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_L (THINKPAD_GSW_EC_LENOVO_PMH7_BASE + 0x0c)
#define THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_H (THINKPAD_GSW_EC_LENOVO_PMH7_BASE + 0x0d)
#define THINKPAD_GSW_EC_LENOVO_PMH7_DATA (THINKPAD_GSW_EC_LENOVO_PMH7_BASE + 0x0e)
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
    if(set) {
        result |= 1 << bit;
    } else {
        result &= ~(1 << bit);
    }

    outb(THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_L, THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_L);
    outb(THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_H, THINKPAD_GSW_EC_LENOVO_PMH7_ADDR_H);
    outb(result, THINKPAD_GSW_EC_LENOVO_PMH7_DATA);
}

// returns 1 if card is disabled, otherwise 0
static int is_card_disabled(void) {
  // Returns 0 if card is powered off
  // Returns 1 if the card is powered on
  int vccgfx_alive = thinkpad_gsw_dgpu_raw_peek(THINKPAD_GSW_EC_LENOVO_PMH7_DGPU_POWER_BIT);

  // Since some GPUs may be powered but dead, check if detected by PCI
  if (vccgfx_alive != 0) {
    u32 cfg_word;
    // read first config word which contains Vendor and Device ID. If all bits
    // are enabled, the device is assumed to be off
    pci_read_config_dword(dis_dev, 0, &cfg_word);
    // if one of the bits is not enabled (the card is enabled), the inverted
    // result will be non-zero and hence logical not will make it 0 ("false")
    return !~cfg_word;
  }

  return 1;
}

/* power bus so we can read PCI configuration space */
static void dis_dev_get(void) {
  if (dis_dev->bus && dis_dev->bus->self)
    pm_runtime_get_sync(&dis_dev->bus->self->dev);
}

static void dis_dev_put(void) {
  if (dis_dev->bus && dis_dev->bus->self)
    pm_runtime_put_sync(&dis_dev->bus->self->dev);
}

static void thinkpad_gsw_try_disable(void) {
  if (is_card_disabled())
    return;

  if (dis_dev->driver) {
    pr_warn("device %s is in use by driver '%s', so it could not be powered off\n",
        dev_name(&dis_dev->dev), dis_dev->driver->name);

    return;
  }

  pr_info("starting to disable discrete graphics\n");

  pci_save_state(dis_dev);
  pci_clear_master(dis_dev);
  pci_disable_device(dis_dev);
  do {
    struct acpi_device *ad = NULL;

    ad = acpi_fetch_acpi_dev(dis_handle);
    if (!ad) {
      pr_warn("Cannot get ACPI device for PCI device\n");
      break;
    }
    if (ad->power.state == ACPI_STATE_UNKNOWN) {
      pr_debug("ACPI power state is unknown, forcing D0\n");
      ad->power.state = ACPI_STATE_D0;
    }
  } while (0);
  pci_set_power_state(dis_dev, PCI_D3cold);

  msleep(50);
  thinkpad_gsw_dgpu_raw_poke(7, 0);
  usleep_range(1000, 2000);
  thinkpad_gsw_dgpu_raw_poke(3, 0);
  msleep(100);

  pr_info("finished disabling discrete graphics\n");
}

static void thinkpad_gsw_try_enable(void) {
  if (!is_card_disabled())
    return;

  pr_info("starting to enable discrete graphics\n");

  msleep(50);
  thinkpad_gsw_dgpu_raw_poke(7, 0);
  thinkpad_gsw_dgpu_raw_poke(3, 1);
  usleep_range(10000, 20000);
  thinkpad_gsw_dgpu_raw_poke(7, 1);
  msleep(100);

  pci_set_power_state(dis_dev, PCI_D0);
  pci_restore_state(dis_dev);
  if(pci_enable_device(dis_dev))
    pr_warn("failed to enable %s\n", dev_name(&dis_dev->dev));
  pci_set_master(dis_dev);
  
  pr_info("finished enabling discrete graphics\n");
}

static int tpgsw_pm_handler(struct notifier_block *nbp, unsigned long event_type, void *p) {
  switch (event_type) {
  case PM_HIBERNATION_PREPARE:
  case PM_SUSPEND_PREPARE:
    dis_dev_get();
    dis_before_suspend_disabled = is_card_disabled();
    // enable the device before suspend to avoid the PCI config space from
    // being saved incorrectly
    if (dis_before_suspend_disabled)
        thinkpad_gsw_try_enable();
    dis_dev_put();
    break;
  case PM_POST_HIBERNATION:
  case PM_POST_SUSPEND:
  case PM_POST_RESTORE:
    // after suspend, the card is on, but if it was off before suspend,
    // disable it again
    if (dis_before_suspend_disabled) {
        dis_dev_get();
        thinkpad_gsw_try_disable();
        dis_dev_put();
    }
    break;
  case PM_RESTORE_PREPARE:
    // deliberately don't do anything as it does not occur before suspend
    // nor hibernate, but before restoring a saved image. In that case,
    // either PM_POST_HIBERNATION or PM_POST_RESTORE will be called
    break;
  }
  return 0;
}

static struct notifier_block tpgsw_pm_block = {
  .notifier_call = &tpgsw_pm_handler
};

static ssize_t gpu_state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
  dis_dev_get();
  int stat = !is_card_disabled();
  dis_dev_put();
  return sysfs_emit(buf, "%d\n", stat);
}

static ssize_t gpu_state_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
  if (count > 0) {
    switch(buf[0]) {
      case '0':
        dis_dev_get();
        thinkpad_gsw_try_disable();
        dis_dev_put();
        break;
      case '1':
        dis_dev_get();
        thinkpad_gsw_try_enable();
        dis_dev_put();
        break;
      default:
        break;
    }
    return 1;
  }
  return 0;
}

static struct kobj_attribute gpu_state_attribute = __ATTR(gpu_state, 0664, gpu_state_show, gpu_state_store);
static struct attribute *attrs[] = {
  &gpu_state_attribute.attr,
  NULL,
};
static struct attribute_group attr_group = { .attrs = attrs, };

static int __init thinkpad_gsw_init(void) {
	pr_info("ThinkPad PMH7 graphics switcher v" THINKPAD_GSW_VERSION " loaded\n");

  struct pci_dev *pdev = NULL;

  while((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev)) != NULL) {
    int pci_class = pdev->class >> 8;

    if(pci_class != PCI_CLASS_DISPLAY_VGA && pci_class != PCI_CLASS_DISPLAY_3D)
      continue;

    // we only care about Nvidia
    //
    // NOTE: if the PMH7 supports switching on/off other GPUs, then this can be
    // changed
    if(pdev->vendor == PCI_VENDOR_ID_NVIDIA) {
      dis_dev = pdev;
      dis_handle = ACPI_HANDLE(&pdev->dev);

      pr_info("Found discrete Nvidia graphics processor at %s\n", dev_name(&dis_dev->dev));
    }
  }

  if(dis_dev == NULL) {
    pr_err("No discrete graphics processor found\n");
    return -ENODEV;
  }

  gsw_kobj = kobject_create_and_add("thinkpad_gsw", power_kobj);

  if(!gsw_kobj) {
    pr_err("Could not allocate kobject\n");
    return -ENOMEM;
  }

  int gsw_group_retval = sysfs_create_group(gsw_kobj, &attr_group);
  if(gsw_group_retval) {
    pr_err("Could not allocate sysfs entries\n");
    kobject_put(gsw_kobj);
    return -ENOMEM;
  }

  dis_dev_get();

  if (!is_card_disabled()) {
    if (pci_enable_device(dis_dev))
      pr_warn("Failed to initially enable graphics card\n");
  }

  switch (load_state) {
    case GSW_ON: thinkpad_gsw_try_enable(); break;
    case GSW_OFF: thinkpad_gsw_try_disable(); break;
    default: break;
  }
  dis_dev_put();

  register_pm_notifier(&tpgsw_pm_block);

	return 0;
}

static void __exit thinkpad_gsw_exit(void) {
  pr_info("Unloading\n");

  // remove kobject
  kobject_put(gsw_kobj);

  dis_dev_get();
  switch (unload_state) {
    case GSW_ON: thinkpad_gsw_try_enable(); break;
    case GSW_OFF: thinkpad_gsw_try_disable(); break;
    default: break;
  }
  dis_dev_put();

  if (tpgsw_pm_block.notifier_call)
    unregister_pm_notifier(&tpgsw_pm_block);
}

module_init(thinkpad_gsw_init);
module_exit(thinkpad_gsw_exit);
