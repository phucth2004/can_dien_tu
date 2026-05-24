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
	{ 0xefd6cf06, "__aeabi_unwind_cpp_pr0" },
	{ 0x9f65c84e, "i2c_transfer_buffer_flags" },
	{ 0xc3055d20, "usleep_range_state" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x828ce6bb, "mutex_lock" },
	{ 0xf9a482f9, "msleep" },
	{ 0x9618ede0, "mutex_unlock" },
	{ 0xf678775b, "device_destroy" },
	{ 0xf8d3d187, "cdev_del" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x17861825, "class_create" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0x95a2eb75, "i2c_register_driver" },
	{ 0xb5b2d70b, "class_destroy" },
	{ 0x2d77f5dd, "i2c_del_driver" },
	{ 0xa234810a, "cdev_init" },
	{ 0x6213c1c5, "cdev_add" },
	{ 0x630792ca, "device_create" },
	{ 0x92997ed8, "_printk" },
	{ 0x5f754e5a, "memset" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0x84b183ae, "strncmp" },
	{ 0xae353d77, "arm_copy_from_user" },
	{ 0xcbd4898c, "fortify_panic" },
	{ 0x1d0c4316, "module_layout" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("i2c:i2c_lcd");
MODULE_ALIAS("of:N*T*Ccustom,i2c-lcd");
MODULE_ALIAS("of:N*T*Ccustom,i2c-lcdC*");
