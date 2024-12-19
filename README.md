# `thinkpad_gsw`

Toggles the ThinkPad PMH7's discrete graphics card power using `vga_switcheroo`.

> [!IMPORTANT]
> This uses undocumented tricks, use at your own risk.
> **You may brick your system with this if you have no idea what you're doing!**

## Why this and not ACPI-based Optimus control?

Bit-banging the power management IC with x86 in/out ports is the only way I 
know of to toggle power to a Corebooted ThinkPad's GPU, as of 2024. This will 
probably change at a later time, once the ACPI Optimus control feature in 
Coreboot is complete.

## Compatibility

- ThinkPad W541 (with Quadro K1100M or Quadro K2100M)

## Installation

Use DKMS to install the driver.

## Usage

This utilizes `vga_switcheroo` and Nouveau's `DRI_PRIME=1` feature. For example:

```shell
# echo ON >> /sys/kernel/debug/vgaswitcheroo/switch
$ DRI_PRIME=1 glxgears -info
# echo OFF >> /sys/kernel/debug/vgaswitcheroo/switch
```
