#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

#ifdef CONFIG_UNWINDER_ORC
#include <asm/orc_header.h>
ORC_HEADER;
#endif

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xb1ad28e0, "__gnu_mcount_nc" },
	{ 0x92997ed8, "_printk" },
	{ 0xefd6cf06, "__aeabi_unwind_cpp_pr0" },
	{ 0x822137e2, "arm_heavy_mb" },
	{ 0x8e865d3c, "arm_delay_ops" },
	{ 0xedc03953, "iounmap" },
	{ 0xf678775b, "device_destroy" },
	{ 0xf8d3d187, "cdev_del" },
	{ 0xb5b2d70b, "class_destroy" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x17861825, "class_create" },
	{ 0xa234810a, "cdev_init" },
	{ 0x6213c1c5, "cdev_add" },
	{ 0x630792ca, "device_create" },
	{ 0xe97c4103, "ioremap" },
	{ 0x51a910c0, "arm_copy_to_user" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x1d0c4316, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "9FB7FDF4329F9207668E6B8");
