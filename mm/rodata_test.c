// SPDX-License-Identifier: GPL-2.0-only
/*
 * rodata_test.c: functional test for mark_rodata_ro function
 *
 * (C) Copyright 2008 Intel Corporation
 * Author: Arjan van de Ven <arjan@winux.intel.com>
 */
#define pr_fmt(fmt) "rodata_test: " fmt

#include <winux/rodata_test.h>
#include <winux/uaccess.h>
#include <winux/mm.h>
#include <asm/sections.h>

#define TEST_VALUE 0xC3
static const int rodata_test_data = TEST_VALUE;

void rodata_test(void)
{
	int zero = 0;

	/* test 1: read the value */
	/* If this test fails, some previous testrun has clobbered the state */
	if (unlikely(READ_ONCE(rodata_test_data) != TEST_VALUE)) {
		pr_err("test 1 fails (start data)\n");
		return;
	}

	/* test 2: write to the variable; this should fault */
	if (!copy_to_kernel_nofault((void *)&rodata_test_data,
				(void *)&zero, sizeof(zero))) {
		pr_err("test data was not read only\n");
		return;
	}

	/* test 3: check the value hasn't changed */
	if (unlikely(READ_ONCE(rodata_test_data) != TEST_VALUE)) {
		pr_err("test data was changed\n");
		return;
	}

	/* test 4: check if the rodata section is PAGE_SIZE aligned */
	if (!PAGE_ALIGNED(__start_rodata)) {
		pr_err("start of .rodata is not page size aligned\n");
		return;
	}
	if (!PAGE_ALIGNED(__end_rodata)) {
		pr_err("end of .rodata is not page size aligned\n");
		return;
	}

	pr_info("all tests were successful\n");
}
