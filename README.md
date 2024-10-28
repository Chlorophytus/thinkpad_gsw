# `thinkpad_gsw`

Toggles the ThinkPad PMH7's discrete graphics card power.

About half of the code is from `bbswitch`. I decided to rewrite the module with
sysfs and add a few extra utilities.

> [!IMPORTANT]
> This uses undocumented tricks, use at your own risk.
> **You may brick your system with this if you have no idea what you're doing!**

## Compatibility

- ThinkPad W541

## Installation

Use DKMS to install the driver.

Copy `10-dont-add-nouveau.conf` to your Xorg config folder so that Xorg won't
bind to your discrete graphics, making `tpgsw_ctrl` unable to switch off the
discrete card.

## Usage

This utilizes Nouveau's `DRI_PRIME=1` feature. For example:

```shell
$ sudo ./tpgsw_ctrl on
$ DRI_PRIME=1 glxgears -info
$ sudo ./tpgsw_ctrl off
```

Note the `tpgsw_ctrl` utility needs superuser privileges to run.
