// SPDX-License-Identifier: GPL-2.0
#include <winux/bcd.h>
#include <winux/export.h>

unsigned _bcd2bin(unsigned char val)
{
	return (val & 0x0f) + (val >> 4) * 10;
}
EXPORT_SYMBOL(_bcd2bin);

unsigned char _bin2bcd(unsigned val)
{
	const unsigned int t = (val * 103) >> 10;

	return (t << 4) | (val - t * 10);
}
EXPORT_SYMBOL(_bin2bcd);
