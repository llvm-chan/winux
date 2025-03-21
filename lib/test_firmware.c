// SPDX-License-Identifier: GPL-2.0-only
/*
 * This module provides an interface to trigger and test firmware loading.
 *
 * It is designed to be used for basic evaluation of the firmware loading
 * subsystem (for example when validating firmware verification). It lacks
 * any extra dependencies, and will not normally be loaded by the system
 * unless explicitly requested by name.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <winux/init.h>
#include <winux/module.h>
#include <winux/printk.h>
#include <winux/completion.h>
#include <winux/firmware.h>
#include <winux/device.h>
#include <winux/fs.h>
#include <winux/miscdevice.h>
#include <winux/sizes.h>
#include <winux/slab.h>
#include <winux/uaccess.h>
#include <winux/delay.h>
#include <winux/kstrtox.h>
#include <winux/kthread.h>
#include <winux/vmalloc.h>
#include <winux/efi_embedded_fw.h>

MODULE_IMPORT_NS("TEST_FIRMWARE");

#define TEST_FIRMWARE_NAME	"test-firmware.bin"
#define TEST_FIRMWARE_NUM_REQS	4
#define TEST_FIRMWARE_BUF_SIZE	SZ_1K
#define TEST_UPLOAD_MAX_SIZE	SZ_2K
#define TEST_UPLOAD_BLK_SIZE	37	/* Avoid powers of two in testing */

static DEFINE_MUTEX(test_fw_mutex);
static const struct firmware *test_firmware;
static LIST_HEAD(test_upload_list);

struct test_batched_req {
	u8 idx;
	int rc;
	bool sent;
	const struct firmware *fw;
	const char *name;
	const char *fw_buf;
	struct completion completion;
	struct task_struct *task;
	struct device *dev;
};

/**
 * struct test_config - represents configuration for the test for different triggers
 *
 * @name: the name of the firmware file to look for
 * @into_buf: when the into_buf is used if this is true
 *	request_firmware_into_buf() will be used instead.
 * @buf_size: size of buf to allocate when into_buf is true
 * @file_offset: file offset to request when calling request_firmware_into_buf
 * @partial: partial read opt when calling request_firmware_into_buf
 * @sync_direct: when the sync trigger is used if this is true
 *	request_firmware_direct() will be used instead.
 * @send_uevent: whether or not to send a uevent for async requests
 * @num_requests: number of requests to try per test case. This is trigger
 *	specific.
 * @reqs: stores all requests information
 * @read_fw_idx: index of thread from which we want to read firmware results
 *	from through the read_fw trigger.
 * @upload_name: firmware name to be used with upload_read sysfs node
 * @test_result: a test may use this to collect the result from the call
 *	of the request_firmware*() calls used in their tests. In order of
 *	priority we always keep first any setup error. If no setup errors were
 *	found then we move on to the first error encountered while running the
 *	API. Note that for async calls this typically will be a successful
 *	result (0) unless of course you've used bogus parameters, or the system
 *	is out of memory.  In the async case the callback is expected to do a
 *	bit more homework to figure out what happened, unfortunately the only
 *	information passed today on error is the fact that no firmware was
 *	found so we can only assume -ENOENT on async calls if the firmware is
 *	NULL.
 *
 *	Errors you can expect:
 *
 *	API specific:
 *
 *	0:		success for sync, for async it means request was sent
 *	-EINVAL:	invalid parameters or request
 *	-ENOENT:	files not found
 *
 *	System environment:
 *
 *	-ENOMEM:	memory pressure on system
 *	-ENODEV:	out of number of devices to test
 *	-EINVAL:	an unexpected error has occurred
 * @req_firmware: if @sync_direct is true this is set to
 *	request_firmware_direct(), otherwise request_firmware()
 */
struct test_config {
	char *name;
	bool into_buf;
	size_t buf_size;
	size_t file_offset;
	bool partial;
	bool sync_direct;
	bool send_uevent;
	u8 num_requests;
	u8 read_fw_idx;
	char *upload_name;

	/*
	 * These below don't belong her but we'll move them once we create
	 * a struct fw_test_device and stuff the misc_dev under there later.
	 */
	struct test_batched_req *reqs;
	int test_result;
	int (*req_firmware)(const struct firmware **fw, const char *name,
			    struct device *device);
};

struct upload_inject_err {
	const char *prog;
	enum fw_upload_err err_code;
};

struct test_firmware_upload {
	char *name;
	struct list_head node;
	char *buf;
	size_t size;
	bool cancel_request;
	struct upload_inject_err inject;
	struct fw_upload *fwl;
};

static struct test_config *test_fw_config;

static struct test_firmware_upload *upload_lookup_name(const char *name)
{
	struct test_firmware_upload *tst;

	list_for_each_entry(tst, &test_upload_list, node)
		if (strncmp(name, tst->name, strlen(tst->name)) == 0)
			return tst;

	return NULL;
}

static ssize_t test_fw_misc_read(struct file *f, char __user *buf,
				 size_t size, loff_t *offset)
{
	ssize_t rc = 0;

	mutex_lock(&test_fw_mutex);
	if (test_firmware)
		rc = simple_read_from_buffer(buf, size, offset,
					     test_firmware->data,
					     test_firmware->size);
	mutex_unlock(&test_fw_mutex);
	return rc;
}

static const struct file_operations test_fw_fops = {
	.owner          = THIS_MODULE,
	.read           = test_fw_misc_read,
};

static void __test_release_all_firmware(void)
{
	struct test_batched_req *req;
	u8 i;

	if (!test_fw_config->reqs)
		return;

	for (i = 0; i < test_fw_config->num_requests; i++) {
		req = &test_fw_config->reqs[i];
		if (req->fw) {
			if (req->fw_buf) {
				kfree_const(req->fw_buf);
				req->fw_buf = NULL;
			}
			release_firmware(req->fw);
			req->fw = NULL;
		}
	}

	vfree(test_fw_config->reqs);
	test_fw_config->reqs = NULL;
}

static void test_release_all_firmware(void)
{
	mutex_lock(&test_fw_mutex);
	__test_release_all_firmware();
	mutex_unlock(&test_fw_mutex);
}


static void __test_firmware_config_free(void)
{
	__test_release_all_firmware();
	kfree_const(test_fw_config->name);
	test_fw_config->name = NULL;
}

/*
 * XXX: move to kstrncpy() once merged.
 *
 * Users should use kfree_const() when freeing these.
 */
static int __kstrncpy(char **dst, const char *name, size_t count, gfp_t gfp)
{
	*dst = kstrndup(name, count, gfp);
	if (!*dst)
		return -ENOMEM;
	return count;
}

static int __test_firmware_config_init(void)
{
	int ret;

	ret = __kstrncpy(&test_fw_config->name, TEST_FIRMWARE_NAME,
			 strlen(TEST_FIRMWARE_NAME), GFP_KERNEL);
	if (ret < 0)
		goto out;

	test_fw_config->num_requests = TEST_FIRMWARE_NUM_REQS;
	test_fw_config->send_uevent = true;
	test_fw_config->into_buf = false;
	test_fw_config->buf_size = TEST_FIRMWARE_BUF_SIZE;
	test_fw_config->file_offset = 0;
	test_fw_config->partial = false;
	test_fw_config->sync_direct = false;
	test_fw_config->req_firmware = request_firmware;
	test_fw_config->test_result = 0;
	test_fw_config->reqs = NULL;
	test_fw_config->upload_name = NULL;

	return 0;

out:
	__test_firmware_config_free();
	return ret;
}

static ssize_t reset_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	int ret;

	mutex_lock(&test_fw_mutex);

	__test_firmware_config_free();

	ret = __test_firmware_config_init();
	if (ret < 0) {
		ret = -ENOMEM;
		pr_err("could not alloc settings for config trigger: %d\n",
		       ret);
		goto out;
	}

	pr_info("reset\n");
	ret = count;

out:
	mutex_unlock(&test_fw_mutex);

	return ret;
}
static DEVICE_ATTR_WO(reset);

static ssize_t config_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int len = 0;

	mutex_lock(&test_fw_mutex);

	len += scnprintf(buf, PAGE_SIZE - len,
			"Custom trigger configuration for: %s\n",
			dev_name(dev));

	if (test_fw_config->name)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"name:\t%s\n",
				test_fw_config->name);
	else
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"name:\tEMPTY\n");

	len += scnprintf(buf + len, PAGE_SIZE - len,
			"num_requests:\t%u\n", test_fw_config->num_requests);

	len += scnprintf(buf + len, PAGE_SIZE - len,
			"send_uevent:\t\t%s\n",
			test_fw_config->send_uevent ?
			"FW_ACTION_UEVENT" :
			"FW_ACTION_NOUEVENT");
	len += scnprintf(buf + len, PAGE_SIZE - len,
			"into_buf:\t\t%s\n",
			test_fw_config->into_buf ? "true" : "false");
	len += scnprintf(buf + len, PAGE_SIZE - len,
			"buf_size:\t%zu\n", test_fw_config->buf_size);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			"file_offset:\t%zu\n", test_fw_config->file_offset);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			"partial:\t\t%s\n",
			test_fw_config->partial ? "true" : "false");
	len += scnprintf(buf + len, PAGE_SIZE - len,
			"sync_direct:\t\t%s\n",
			test_fw_config->sync_direct ? "true" : "false");
	len += scnprintf(buf + len, PAGE_SIZE - len,
			"read_fw_idx:\t%u\n", test_fw_config->read_fw_idx);
	if (test_fw_config->upload_name)
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"upload_name:\t%s\n",
				test_fw_config->upload_name);
	else
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"upload_name:\tEMPTY\n");

	mutex_unlock(&test_fw_mutex);

	return len;
}
static DEVICE_ATTR_RO(config);

static ssize_t config_name_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;

	mutex_lock(&test_fw_mutex);
	kfree_const(test_fw_config->name);
	ret = __kstrncpy(&test_fw_config->name, buf, count, GFP_KERNEL);
	mutex_unlock(&test_fw_mutex);

	return ret;
}

/*
 * As per sysfs_kf_seq_show() the buf is max PAGE_SIZE.
 */
static ssize_t config_test_show_str(char *dst,
				    char *src)
{
	int len;

	mutex_lock(&test_fw_mutex);
	len = snprintf(dst, PAGE_SIZE, "%s\n", src);
	mutex_unlock(&test_fw_mutex);

	return len;
}

static inline int __test_dev_config_update_bool(const char *buf, size_t size,
				       bool *cfg)
{
	int ret;

	if (kstrtobool(buf, cfg) < 0)
		ret = -EINVAL;
	else
		ret = size;

	return ret;
}

static int test_dev_config_update_bool(const char *buf, size_t size,
				       bool *cfg)
{
	int ret;

	mutex_lock(&test_fw_mutex);
	ret = __test_dev_config_update_bool(buf, size, cfg);
	mutex_unlock(&test_fw_mutex);

	return ret;
}

static ssize_t test_dev_config_show_bool(char *buf, bool val)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static int __test_dev_config_update_size_t(
					 const char *buf,
					 size_t size,
					 size_t *cfg)
{
	int ret;
	long new;

	ret = kstrtol(buf, 10, &new);
	if (ret)
		return ret;

	*(size_t *)cfg = new;

	/* Always return full write size even if we didn't consume all */
	return size;
}

static ssize_t test_dev_config_show_size_t(char *buf, size_t val)
{
	return snprintf(buf, PAGE_SIZE, "%zu\n", val);
}

static ssize_t test_dev_config_show_int(char *buf, int val)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static int __test_dev_config_update_u8(const char *buf, size_t size, u8 *cfg)
{
	u8 val;
	int ret;

	ret = kstrtou8(buf, 10, &val);
	if (ret)
		return ret;

	*(u8 *)cfg = val;

	/* Always return full write size even if we didn't consume all */
	return size;
}

static int test_dev_config_update_u8(const char *buf, size_t size, u8 *cfg)
{
	int ret;

	mutex_lock(&test_fw_mutex);
	ret = __test_dev_config_update_u8(buf, size, cfg);
	mutex_unlock(&test_fw_mutex);

	return ret;
}

static ssize_t test_dev_config_show_u8(char *buf, u8 val)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t config_name_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return config_test_show_str(buf, test_fw_config->name);
}
static DEVICE_ATTR_RW(config_name);

static ssize_t config_upload_name_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct test_firmware_upload *tst;
	int ret = count;

	mutex_lock(&test_fw_mutex);
	tst = upload_lookup_name(buf);
	if (tst)
		test_fw_config->upload_name = tst->name;
	else
		ret = -EINVAL;
	mutex_unlock(&test_fw_mutex);

	return ret;
}

static ssize_t config_upload_name_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return config_test_show_str(buf, test_fw_config->upload_name);
}
static DEVICE_ATTR_RW(config_upload_name);

static ssize_t config_num_requests_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	int rc;

	mutex_lock(&test_fw_mutex);
	if (test_fw_config->reqs) {
		pr_err("Must call release_all_firmware prior to changing config\n");
		rc = -EINVAL;
		mutex_unlock(&test_fw_mutex);
		goto out;
	}

	rc = __test_dev_config_update_u8(buf, count,
					 &test_fw_config->num_requests);
	mutex_unlock(&test_fw_mutex);

out:
	return rc;
}

static ssize_t config_num_requests_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return test_dev_config_show_u8(buf, test_fw_config->num_requests);
}
static DEVICE_ATTR_RW(config_num_requests);

static ssize_t config_into_buf_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	return test_dev_config_update_bool(buf,
					   count,
					   &test_fw_config->into_buf);
}

static ssize_t config_into_buf_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	return test_dev_config_show_bool(buf, test_fw_config->into_buf);
}
static DEVICE_ATTR_RW(config_into_buf);

static ssize_t config_buf_size_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int rc;

	mutex_lock(&test_fw_mutex);
	if (test_fw_config->reqs) {
		pr_err("Must call release_all_firmware prior to changing config\n");
		rc = -EINVAL;
		mutex_unlock(&test_fw_mutex);
		goto out;
	}

	rc = __test_dev_config_update_size_t(buf, count,
					     &test_fw_config->buf_size);
	mutex_unlock(&test_fw_mutex);

out:
	return rc;
}

static ssize_t config_buf_size_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	return test_dev_config_show_size_t(buf, test_fw_config->buf_size);
}
static DEVICE_ATTR_RW(config_buf_size);

static ssize_t config_file_offset_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int rc;

	mutex_lock(&test_fw_mutex);
	if (test_fw_config->reqs) {
		pr_err("Must call release_all_firmware prior to changing config\n");
		rc = -EINVAL;
		mutex_unlock(&test_fw_mutex);
		goto out;
	}

	rc = __test_dev_config_update_size_t(buf, count,
					     &test_fw_config->file_offset);
	mutex_unlock(&test_fw_mutex);

out:
	return rc;
}

static ssize_t config_file_offset_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return test_dev_config_show_size_t(buf, test_fw_config->file_offset);
}
static DEVICE_ATTR_RW(config_file_offset);

static ssize_t config_partial_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	return test_dev_config_update_bool(buf,
					   count,
					   &test_fw_config->partial);
}

static ssize_t config_partial_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	return test_dev_config_show_bool(buf, test_fw_config->partial);
}
static DEVICE_ATTR_RW(config_partial);

static ssize_t config_sync_direct_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int rc = test_dev_config_update_bool(buf, count,
					     &test_fw_config->sync_direct);

	if (rc == count)
		test_fw_config->req_firmware = test_fw_config->sync_direct ?
				       request_firmware_direct :
				       request_firmware;
	return rc;
}

static ssize_t config_sync_direct_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return test_dev_config_show_bool(buf, test_fw_config->sync_direct);
}
static DEVICE_ATTR_RW(config_sync_direct);

static ssize_t config_send_uevent_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	return test_dev_config_update_bool(buf, count,
					   &test_fw_config->send_uevent);
}

static ssize_t config_send_uevent_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return test_dev_config_show_bool(buf, test_fw_config->send_uevent);
}
static DEVICE_ATTR_RW(config_send_uevent);

static ssize_t config_read_fw_idx_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	return test_dev_config_update_u8(buf, count,
					 &test_fw_config->read_fw_idx);
}

static ssize_t config_read_fw_idx_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return test_dev_config_show_u8(buf, test_fw_config->read_fw_idx);
}
static DEVICE_ATTR_RW(config_read_fw_idx);


static ssize_t trigger_request_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int rc;
	char *name;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	pr_info("loading '%s'\n", name);

	mutex_lock(&test_fw_mutex);
	release_firmware(test_firmware);
	if (test_fw_config->reqs)
		__test_release_all_firmware();
	test_firmware = NULL;
	rc = request_firmware(&test_firmware, name, dev);
	if (rc) {
		pr_info("load of '%s' failed: %d\n", name, rc);
		goto out;
	}
	pr_info("loaded: %zu\n", test_firmware->size);
	rc = count;

out:
	mutex_unlock(&test_fw_mutex);

	kfree(name);

	return rc;
}
static DEVICE_ATTR_WO(trigger_request);

#ifdef CONFIG_EFI_EMBEDDED_FIRMWARE
extern struct list_head efi_embedded_fw_list;
extern bool efi_embedded_fw_checked;

static ssize_t trigger_request_platform_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	static const u8 test_data[] = {
		0x55, 0xaa, 0x55, 0xaa, 0x01, 0x02, 0x03, 0x04,
		0x55, 0xaa, 0x55, 0xaa, 0x05, 0x06, 0x07, 0x08,
		0x55, 0xaa, 0x55, 0xaa, 0x10, 0x20, 0x30, 0x40,
		0x55, 0xaa, 0x55, 0xaa, 0x50, 0x60, 0x70, 0x80
	};
	struct efi_embedded_fw efi_embedded_fw;
	const struct firmware *firmware = NULL;
	bool saved_efi_embedded_fw_checked;
	char *name;
	int rc;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	pr_info("inserting test platform fw '%s'\n", name);
	efi_embedded_fw.name = name;
	efi_embedded_fw.data = (void *)test_data;
	efi_embedded_fw.length = sizeof(test_data);
	list_add(&efi_embedded_fw.list, &efi_embedded_fw_list);
	saved_efi_embedded_fw_checked = efi_embedded_fw_checked;
	efi_embedded_fw_checked = true;

	pr_info("loading '%s'\n", name);
	rc = firmware_request_platform(&firmware, name, dev);
	if (rc) {
		pr_info("load of '%s' failed: %d\n", name, rc);
		goto out;
	}
	if (firmware->size != sizeof(test_data) ||
	    memcmp(firmware->data, test_data, sizeof(test_data)) != 0) {
		pr_info("firmware contents mismatch for '%s'\n", name);
		rc = -EINVAL;
		goto out;
	}
	pr_info("loaded: %zu\n", firmware->size);
	rc = count;

out:
	efi_embedded_fw_checked = saved_efi_embedded_fw_checked;
	release_firmware(firmware);
	list_del(&efi_embedded_fw.list);
	kfree(name);

	return rc;
}
static DEVICE_ATTR_WO(trigger_request_platform);
#endif

static DECLARE_COMPLETION(async_fw_done);

static void trigger_async_request_cb(const struct firmware *fw, void *context)
{
	test_firmware = fw;
	complete(&async_fw_done);
}

static ssize_t trigger_async_request_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int rc;
	char *name;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	pr_info("loading '%s'\n", name);

	mutex_lock(&test_fw_mutex);
	release_firmware(test_firmware);
	test_firmware = NULL;
	if (test_fw_config->reqs)
		__test_release_all_firmware();
	rc = request_firmware_nowait(THIS_MODULE, 1, name, dev, GFP_KERNEL,
				     NULL, trigger_async_request_cb);
	if (rc) {
		pr_info("async load of '%s' failed: %d\n", name, rc);
		kfree(name);
		goto out;
	}
	/* Free 'name' ASAP, to test for race conditions */
	kfree(name);

	wait_for_completion(&async_fw_done);

	if (test_firmware) {
		pr_info("loaded: %zu\n", test_firmware->size);
		rc = count;
	} else {
		pr_err("failed to async load firmware\n");
		rc = -ENOMEM;
	}

out:
	mutex_unlock(&test_fw_mutex);

	return rc;
}
static DEVICE_ATTR_WO(trigger_async_request);

static ssize_t trigger_custom_fallback_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	int rc;
	char *name;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	pr_info("loading '%s' using custom fallback mechanism\n", name);

	mutex_lock(&test_fw_mutex);
	release_firmware(test_firmware);
	if (test_fw_config->reqs)
		__test_release_all_firmware();
	test_firmware = NULL;
	rc = request_firmware_nowait(THIS_MODULE, FW_ACTION_NOUEVENT, name,
				     dev, GFP_KERNEL, NULL,
				     trigger_async_request_cb);
	if (rc) {
		pr_info("async load of '%s' failed: %d\n", name, rc);
		kfree(name);
		goto out;
	}
	/* Free 'name' ASAP, to test for race conditions */
	kfree(name);

	wait_for_completion(&async_fw_done);

	if (test_firmware) {
		pr_info("loaded: %zu\n", test_firmware->size);
		rc = count;
	} else {
		pr_err("failed to async load firmware\n");
		rc = -ENODEV;
	}

out:
	mutex_unlock(&test_fw_mutex);

	return rc;
}
static DEVICE_ATTR_WO(trigger_custom_fallback);

static int test_fw_run_batch_request(void *data)
{
	struct test_batched_req *req = data;

	if (!req) {
		test_fw_config->test_result = -EINVAL;
		return -EINVAL;
	}

	if (test_fw_config->into_buf) {
		void *test_buf;

		test_buf = kzalloc(TEST_FIRMWARE_BUF_SIZE, GFP_KERNEL);
		if (!test_buf)
			return -ENOMEM;

		if (test_fw_config->partial)
			req->rc = request_partial_firmware_into_buf
						(&req->fw,
						 req->name,
						 req->dev,
						 test_buf,
						 test_fw_config->buf_size,
						 test_fw_config->file_offset);
		else
			req->rc = request_firmware_into_buf
						(&req->fw,
						 req->name,
						 req->dev,
						 test_buf,
						 test_fw_config->buf_size);
		if (!req->fw)
			kfree(test_buf);
		else
			req->fw_buf = test_buf;
	} else {
		req->rc = test_fw_config->req_firmware(&req->fw,
						       req->name,
						       req->dev);
	}

	if (req->rc) {
		pr_info("#%u: batched sync load failed: %d\n",
			req->idx, req->rc);
		if (!test_fw_config->test_result)
			test_fw_config->test_result = req->rc;
	} else if (req->fw) {
		req->sent = true;
		pr_info("#%u: batched sync loaded %zu\n",
			req->idx, req->fw->size);
	}
	complete(&req->completion);

	req->task = NULL;

	return 0;
}

/*
 * We use a kthread as otherwise the kernel serializes all our sync requests
 * and we would not be able to mimic batched requests on a sync call. Batched
 * requests on a sync call can for instance happen on a device driver when
 * multiple cards are used and firmware loading happens outside of probe.
 */
static ssize_t trigger_batched_requests_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	struct test_batched_req *req;
	int rc;
	u8 i;

	mutex_lock(&test_fw_mutex);

	if (test_fw_config->reqs) {
		rc = -EBUSY;
		goto out_bail;
	}

	test_fw_config->reqs =
		vzalloc(array3_size(sizeof(struct test_batched_req),
				    test_fw_config->num_requests, 2));
	if (!test_fw_config->reqs) {
		rc = -ENOMEM;
		goto out_unlock;
	}

	pr_info("batched sync firmware loading '%s' %u times\n",
		test_fw_config->name, test_fw_config->num_requests);

	for (i = 0; i < test_fw_config->num_requests; i++) {
		req = &test_fw_config->reqs[i];
		req->fw = NULL;
		req->idx = i;
		req->name = test_fw_config->name;
		req->fw_buf = NULL;
		req->dev = dev;
		init_completion(&req->completion);
		req->task = kthread_run(test_fw_run_batch_request, req,
					     "%s-%u", KBUILD_MODNAME, req->idx);
		if (!req->task || IS_ERR(req->task)) {
			pr_err("Setting up thread %u failed\n", req->idx);
			req->task = NULL;
			rc = -ENOMEM;
			goto out_bail;
		}
	}

	rc = count;

	/*
	 * We require an explicit release to enable more time and delay of
	 * calling release_firmware() to improve our chances of forcing a
	 * batched request. If we instead called release_firmware() right away
	 * then we might miss on an opportunity of having a successful firmware
	 * request pass on the opportunity to be come a batched request.
	 */

out_bail:
	for (i = 0; i < test_fw_config->num_requests; i++) {
		req = &test_fw_config->reqs[i];
		if (req->task || req->sent)
			wait_for_completion(&req->completion);
	}

	/* Override any worker error if we had a general setup error */
	if (rc < 0)
		test_fw_config->test_result = rc;

out_unlock:
	mutex_unlock(&test_fw_mutex);

	return rc;
}
static DEVICE_ATTR_WO(trigger_batched_requests);

/*
 * We wait for each callback to return with the lock held, no need to lock here
 */
static void trigger_batched_cb(const struct firmware *fw, void *context)
{
	struct test_batched_req *req = context;

	if (!req) {
		test_fw_config->test_result = -EINVAL;
		return;
	}

	/* forces *some* batched requests to queue up */
	if (!req->idx)
		ssleep(2);

	req->fw = fw;

	/*
	 * Unfortunately the firmware API gives us nothing other than a null FW
	 * if the firmware was not found on async requests.  Best we can do is
	 * just assume -ENOENT. A better API would pass the actual return
	 * value to the callback.
	 */
	if (!fw && !test_fw_config->test_result)
		test_fw_config->test_result = -ENOENT;

	complete(&req->completion);
}

static
ssize_t trigger_batched_requests_async_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct test_batched_req *req;
	bool send_uevent;
	int rc;
	u8 i;

	mutex_lock(&test_fw_mutex);

	if (test_fw_config->reqs) {
		rc = -EBUSY;
		goto out_bail;
	}

	test_fw_config->reqs =
		vzalloc(array3_size(sizeof(struct test_batched_req),
				    test_fw_config->num_requests, 2));
	if (!test_fw_config->reqs) {
		rc = -ENOMEM;
		goto out;
	}

	pr_info("batched loading '%s' custom fallback mechanism %u times\n",
		test_fw_config->name, test_fw_config->num_requests);

	send_uevent = test_fw_config->send_uevent ? FW_ACTION_UEVENT :
		FW_ACTION_NOUEVENT;

	for (i = 0; i < test_fw_config->num_requests; i++) {
		req = &test_fw_config->reqs[i];
		req->name = test_fw_config->name;
		req->fw_buf = NULL;
		req->fw = NULL;
		req->idx = i;
		init_completion(&req->completion);
		rc = request_firmware_nowait(THIS_MODULE, send_uevent,
					     req->name,
					     dev, GFP_KERNEL, req,
					     trigger_batched_cb);
		if (rc) {
			pr_info("#%u: batched async load failed setup: %d\n",
				i, rc);
			req->rc = rc;
			goto out_bail;
		} else
			req->sent = true;
	}

	rc = count;

out_bail:

	/*
	 * We require an explicit release to enable more time and delay of
	 * calling release_firmware() to improve our chances of forcing a
	 * batched request. If we instead called release_firmware() right away
	 * then we might miss on an opportunity of having a successful firmware
	 * request pass on the opportunity to be come a batched request.
	 */

	for (i = 0; i < test_fw_config->num_requests; i++) {
		req = &test_fw_config->reqs[i];
		if (req->sent)
			wait_for_completion(&req->completion);
	}

	/* Override any worker error if we had a general setup error */
	if (rc < 0)
		test_fw_config->test_result = rc;

out:
	mutex_unlock(&test_fw_mutex);

	return rc;
}
static DEVICE_ATTR_WO(trigger_batched_requests_async);

static void upload_release(struct test_firmware_upload *tst)
{
	firmware_upload_unregister(tst->fwl);
	kfree(tst->buf);
	kfree(tst->name);
	kfree(tst);
}

static void upload_release_all(void)
{
	struct test_firmware_upload *tst, *tmp;

	list_for_each_entry_safe(tst, tmp, &test_upload_list, node) {
		list_del(&tst->node);
		upload_release(tst);
	}
	test_fw_config->upload_name = NULL;
}

/*
 * This table is replicated from .../firmware_loader/sysfs_upload.c
 * and needs to be kept in sync.
 */
static const char * const fw_upload_err_str[] = {
	[FW_UPLOAD_ERR_NONE]	     = "none",
	[FW_UPLOAD_ERR_HW_ERROR]     = "hw-error",
	[FW_UPLOAD_ERR_TIMEOUT]	     = "timeout",
	[FW_UPLOAD_ERR_CANCELED]     = "user-abort",
	[FW_UPLOAD_ERR_BUSY]	     = "device-busy",
	[FW_UPLOAD_ERR_INVALID_SIZE] = "invalid-file-size",
	[FW_UPLOAD_ERR_RW_ERROR]     = "read-write-error",
	[FW_UPLOAD_ERR_WEAROUT]	     = "flash-wearout",
	[FW_UPLOAD_ERR_FW_INVALID]   = "firmware-invalid",
};

static void upload_err_inject_error(struct test_firmware_upload *tst,
				    const u8 *p, const char *prog)
{
	enum fw_upload_err err;

	for (err = FW_UPLOAD_ERR_NONE + 1; err < FW_UPLOAD_ERR_MAX; err++) {
		if (strncmp(p, fw_upload_err_str[err],
			    strlen(fw_upload_err_str[err])) == 0) {
			tst->inject.prog = prog;
			tst->inject.err_code = err;
			return;
		}
	}
}

static void upload_err_inject_prog(struct test_firmware_upload *tst,
				   const u8 *p)
{
	static const char * const progs[] = {
		"preparing:", "transferring:", "programming:"
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(progs); i++) {
		if (strncmp(p, progs[i], strlen(progs[i])) == 0) {
			upload_err_inject_error(tst, p + strlen(progs[i]),
						progs[i]);
			return;
		}
	}
}

#define FIVE_MINUTES_MS	(5 * 60 * 1000)
static enum fw_upload_err
fw_upload_wait_on_cancel(struct test_firmware_upload *tst)
{
	int ms_delay;

	for (ms_delay = 0; ms_delay < FIVE_MINUTES_MS; ms_delay += 100) {
		msleep(100);
		if (tst->cancel_request)
			return FW_UPLOAD_ERR_CANCELED;
	}
	return FW_UPLOAD_ERR_NONE;
}

static enum fw_upload_err test_fw_upload_prepare(struct fw_upload *fwl,
						 const u8 *data, u32 size)
{
	struct test_firmware_upload *tst = fwl->dd_handle;
	enum fw_upload_err ret = FW_UPLOAD_ERR_NONE;
	const char *progress = "preparing:";

	tst->cancel_request = false;

	if (!size || size > TEST_UPLOAD_MAX_SIZE) {
		ret = FW_UPLOAD_ERR_INVALID_SIZE;
		goto err_out;
	}

	if (strncmp(data, "inject:", strlen("inject:")) == 0)
		upload_err_inject_prog(tst, data + strlen("inject:"));

	memset(tst->buf, 0, TEST_UPLOAD_MAX_SIZE);
	tst->size = size;

	if (tst->inject.err_code == FW_UPLOAD_ERR_NONE ||
	    strncmp(tst->inject.prog, progress, strlen(progress)) != 0)
		return FW_UPLOAD_ERR_NONE;

	if (tst->inject.err_code == FW_UPLOAD_ERR_CANCELED)
		ret = fw_upload_wait_on_cancel(tst);
	else
		ret = tst->inject.err_code;

err_out:
	/*
	 * The cleanup op only executes if the prepare op succeeds.
	 * If the prepare op fails, it must do it's own clean-up.
	 */
	tst->inject.err_code = FW_UPLOAD_ERR_NONE;
	tst->inject.prog = NULL;

	return ret;
}

static enum fw_upload_err test_fw_upload_write(struct fw_upload *fwl,
					       const u8 *data, u32 offset,
					       u32 size, u32 *written)
{
	struct test_firmware_upload *tst = fwl->dd_handle;
	const char *progress = "transferring:";
	u32 blk_size;

	if (tst->cancel_request)
		return FW_UPLOAD_ERR_CANCELED;

	blk_size = min_t(u32, TEST_UPLOAD_BLK_SIZE, size);
	memcpy(tst->buf + offset, data + offset, blk_size);

	*written = blk_size;

	if (tst->inject.err_code == FW_UPLOAD_ERR_NONE ||
	    strncmp(tst->inject.prog, progress, strlen(progress)) != 0)
		return FW_UPLOAD_ERR_NONE;

	if (tst->inject.err_code == FW_UPLOAD_ERR_CANCELED)
		return fw_upload_wait_on_cancel(tst);

	return tst->inject.err_code;
}

static enum fw_upload_err test_fw_upload_complete(struct fw_upload *fwl)
{
	struct test_firmware_upload *tst = fwl->dd_handle;
	const char *progress = "programming:";

	if (tst->cancel_request)
		return FW_UPLOAD_ERR_CANCELED;

	if (tst->inject.err_code == FW_UPLOAD_ERR_NONE ||
	    strncmp(tst->inject.prog, progress, strlen(progress)) != 0)
		return FW_UPLOAD_ERR_NONE;

	if (tst->inject.err_code == FW_UPLOAD_ERR_CANCELED)
		return fw_upload_wait_on_cancel(tst);

	return tst->inject.err_code;
}

static void test_fw_upload_cancel(struct fw_upload *fwl)
{
	struct test_firmware_upload *tst = fwl->dd_handle;

	tst->cancel_request = true;
}

static void test_fw_cleanup(struct fw_upload *fwl)
{
	struct test_firmware_upload *tst = fwl->dd_handle;

	tst->inject.err_code = FW_UPLOAD_ERR_NONE;
	tst->inject.prog = NULL;
}

static const struct fw_upload_ops upload_test_ops = {
	.prepare = test_fw_upload_prepare,
	.write = test_fw_upload_write,
	.poll_complete = test_fw_upload_complete,
	.cancel = test_fw_upload_cancel,
	.cleanup = test_fw_cleanup
};

static ssize_t upload_register_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct test_firmware_upload *tst;
	struct fw_upload *fwl;
	char *name;
	int ret;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	mutex_lock(&test_fw_mutex);
	tst = upload_lookup_name(name);
	if (tst) {
		ret = -EEXIST;
		goto free_name;
	}

	tst = kzalloc(sizeof(*tst), GFP_KERNEL);
	if (!tst) {
		ret = -ENOMEM;
		goto free_name;
	}

	tst->name = name;
	tst->buf = kzalloc(TEST_UPLOAD_MAX_SIZE, GFP_KERNEL);
	if (!tst->buf) {
		ret = -ENOMEM;
		goto free_tst;
	}

	fwl = firmware_upload_register(THIS_MODULE, dev, tst->name,
				       &upload_test_ops, tst);
	if (IS_ERR(fwl)) {
		ret = PTR_ERR(fwl);
		goto free_buf;
	}

	tst->fwl = fwl;
	list_add_tail(&tst->node, &test_upload_list);
	mutex_unlock(&test_fw_mutex);
	return count;

free_buf:
	kfree(tst->buf);

free_tst:
	kfree(tst);

free_name:
	mutex_unlock(&test_fw_mutex);
	kfree(name);

	return ret;
}
static DEVICE_ATTR_WO(upload_register);

static ssize_t upload_unregister_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct test_firmware_upload *tst;
	int ret = count;

	mutex_lock(&test_fw_mutex);
	tst = upload_lookup_name(buf);
	if (!tst) {
		ret = -EINVAL;
		goto out;
	}

	if (test_fw_config->upload_name == tst->name)
		test_fw_config->upload_name = NULL;

	list_del(&tst->node);
	upload_release(tst);

out:
	mutex_unlock(&test_fw_mutex);
	return ret;
}
static DEVICE_ATTR_WO(upload_unregister);

static ssize_t test_result_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return test_dev_config_show_int(buf, test_fw_config->test_result);
}
static DEVICE_ATTR_RO(test_result);

static ssize_t release_all_firmware_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	test_release_all_firmware();
	return count;
}
static DEVICE_ATTR_WO(release_all_firmware);

static ssize_t read_firmware_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct test_batched_req *req;
	u8 idx;
	ssize_t rc = 0;

	mutex_lock(&test_fw_mutex);

	idx = test_fw_config->read_fw_idx;
	if (idx >= test_fw_config->num_requests) {
		rc = -ERANGE;
		goto out;
	}

	if (!test_fw_config->reqs) {
		rc = -EINVAL;
		goto out;
	}

	req = &test_fw_config->reqs[idx];
	if (!req->fw) {
		pr_err("#%u: failed to async load firmware\n", idx);
		rc = -ENOENT;
		goto out;
	}

	pr_info("#%u: loaded %zu\n", idx, req->fw->size);

	if (req->fw->size > PAGE_SIZE) {
		pr_err("Testing interface must use PAGE_SIZE firmware for now\n");
		rc = -EINVAL;
		goto out;
	}
	memcpy(buf, req->fw->data, req->fw->size);

	rc = req->fw->size;
out:
	mutex_unlock(&test_fw_mutex);

	return rc;
}
static DEVICE_ATTR_RO(read_firmware);

static ssize_t upload_read_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct test_firmware_upload *tst = NULL;
	struct test_firmware_upload *tst_iter;
	int ret = -EINVAL;

	if (!test_fw_config->upload_name) {
		pr_err("Set config_upload_name before using upload_read\n");
		return -EINVAL;
	}

	mutex_lock(&test_fw_mutex);
	list_for_each_entry(tst_iter, &test_upload_list, node)
		if (tst_iter->name == test_fw_config->upload_name) {
			tst = tst_iter;
			break;
		}

	if (!tst) {
		pr_err("Firmware name not found: %s\n",
		       test_fw_config->upload_name);
		goto out;
	}

	if (tst->size > PAGE_SIZE) {
		pr_err("Testing interface must use PAGE_SIZE firmware for now\n");
		goto out;
	}

	memcpy(buf, tst->buf, tst->size);
	ret = tst->size;
out:
	mutex_unlock(&test_fw_mutex);
	return ret;
}
static DEVICE_ATTR_RO(upload_read);

#define TEST_FW_DEV_ATTR(name)          &dev_attr_##name.attr

static struct attribute *test_dev_attrs[] = {
	TEST_FW_DEV_ATTR(reset),

	TEST_FW_DEV_ATTR(config),
	TEST_FW_DEV_ATTR(config_name),
	TEST_FW_DEV_ATTR(config_num_requests),
	TEST_FW_DEV_ATTR(config_into_buf),
	TEST_FW_DEV_ATTR(config_buf_size),
	TEST_FW_DEV_ATTR(config_file_offset),
	TEST_FW_DEV_ATTR(config_partial),
	TEST_FW_DEV_ATTR(config_sync_direct),
	TEST_FW_DEV_ATTR(config_send_uevent),
	TEST_FW_DEV_ATTR(config_read_fw_idx),
	TEST_FW_DEV_ATTR(config_upload_name),

	/* These don't use the config at all - they could be ported! */
	TEST_FW_DEV_ATTR(trigger_request),
	TEST_FW_DEV_ATTR(trigger_async_request),
	TEST_FW_DEV_ATTR(trigger_custom_fallback),
#ifdef CONFIG_EFI_EMBEDDED_FIRMWARE
	TEST_FW_DEV_ATTR(trigger_request_platform),
#endif

	/* These use the config and can use the test_result */
	TEST_FW_DEV_ATTR(trigger_batched_requests),
	TEST_FW_DEV_ATTR(trigger_batched_requests_async),

	TEST_FW_DEV_ATTR(release_all_firmware),
	TEST_FW_DEV_ATTR(test_result),
	TEST_FW_DEV_ATTR(read_firmware),
	TEST_FW_DEV_ATTR(upload_read),
	TEST_FW_DEV_ATTR(upload_register),
	TEST_FW_DEV_ATTR(upload_unregister),
	NULL,
};

ATTRIBUTE_GROUPS(test_dev);

static struct miscdevice test_fw_misc_device = {
	.minor          = MISC_DYNAMIC_MINOR,
	.name           = "test_firmware",
	.fops           = &test_fw_fops,
	.groups 	= test_dev_groups,
};

static int __init test_firmware_init(void)
{
	int rc;

	test_fw_config = kzalloc(sizeof(struct test_config), GFP_KERNEL);
	if (!test_fw_config)
		return -ENOMEM;

	rc = __test_firmware_config_init();
	if (rc) {
		kfree(test_fw_config);
		pr_err("could not init firmware test config: %d\n", rc);
		return rc;
	}

	rc = misc_register(&test_fw_misc_device);
	if (rc) {
		__test_firmware_config_free();
		kfree(test_fw_config);
		pr_err("could not register misc device: %d\n", rc);
		return rc;
	}

	pr_warn("interface ready\n");

	return 0;
}

module_init(test_firmware_init);

static void __exit test_firmware_exit(void)
{
	mutex_lock(&test_fw_mutex);
	release_firmware(test_firmware);
	misc_deregister(&test_fw_misc_device);
	upload_release_all();
	__test_firmware_config_free();
	kfree(test_fw_config);
	mutex_unlock(&test_fw_mutex);

	pr_warn("removed interface\n");
}

module_exit(test_firmware_exit);

MODULE_AUTHOR("Kees Cook <keescook@chromium.org>");
MODULE_DESCRIPTION("interface to trigger and test firmware loading");
MODULE_LICENSE("GPL");
