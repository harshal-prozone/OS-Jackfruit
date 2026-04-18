#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

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



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x92997ed8, "_printk" },
	{ 0xb84d867, "cdev_init" },
	{ 0xd0d015c2, "cdev_add" },
	{ 0x1df5d450, "class_create" },
	{ 0xfe5fa52a, "cdev_del" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0x1218406a, "device_create" },
	{ 0x2572539b, "class_destroy" },
	{ 0xc6f46339, "init_timer_key" },
	{ 0x15ba50a6, "jiffies" },
	{ 0xc38c83b8, "mod_timer" },
	{ 0x950eb34e, "__list_del_entry_valid_or_report" },
	{ 0x82ee90dc, "timer_delete_sync" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0x37a0cba, "kfree" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0xb424469e, "device_destroy" },
	{ 0xd946d4cf, "find_vpid" },
	{ 0xe1795837, "pid_task" },
	{ 0xa65c6def, "alt_cb_patch_nops" },
	{ 0xeeb9267e, "get_task_mm" },
	{ 0xc7aa6951, "mmput" },
	{ 0x296695f, "refcount_warn_saturate" },
	{ 0x714c984e, "__put_task_struct" },
	{ 0x18004e9, "send_sig" },
	{ 0xdcb764ad, "memset" },
	{ 0x12a4e128, "__arch_copy_from_user" },
	{ 0x7cd1ad8c, "kmalloc_caches" },
	{ 0xf1bbe560, "__kmalloc_cache_noprof" },
	{ 0x9166fada, "strncpy" },
	{ 0x7696f8c7, "__list_add_valid_or_report" },
	{ 0x75ca79b5, "__fortify_panic" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0xeefb4d61, "module_layout" },
};

MODULE_INFO(depends, "");

