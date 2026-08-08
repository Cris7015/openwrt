// SPDX-License-Identifier: GPL-2.0-only
/* Board-level, fail-closed optical controls for EcoNet xPON devices. */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "../inc/common/xpon_board.h"

static struct gpio_desc *xpon_tx_disable;
static bool xpon_board_driver_registered;

bool xpon_board_tx_disable_ready(void)
{
	return READ_ONCE(xpon_tx_disable) != NULL;
}

int xpon_board_set_tx_disable(bool disable)
{
	struct gpio_desc *desc = READ_ONCE(xpon_tx_disable);

	if (!desc)
		return -ENODEV;

	/* The XR500v controller is MMIO-backed and cannot sleep.  Probe rejects
	 * any future board that cannot honour this contract from PHY paths.
	 */
	gpiod_set_value(desc, disable);
	return 0;
}

static int xpon_board_probe(struct platform_device *pdev)
{
	struct gpio_desc *desc;

	/* Assert the physical kill before publishing the descriptor.  The DT
	 * polarity describes assertion, so this remains correct per board.
	 */
	desc = devm_gpiod_get(&pdev->dev, "tx-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(desc))
		return dev_err_probe(&pdev->dev, PTR_ERR(desc),
				     "cannot claim TX_DISABLE\n");
	if (gpiod_cansleep(desc))
		return dev_err_probe(&pdev->dev, -EOPNOTSUPP,
				     "TX_DISABLE GPIO may sleep\n");

	WRITE_ONCE(xpon_tx_disable, desc);
	dev_info(&pdev->dev, "physical optical TX_DISABLE asserted\n");
	return 0;
}

static void xpon_board_remove(struct platform_device *pdev)
{
	struct gpio_desc *desc = READ_ONCE(xpon_tx_disable);

	if (desc)
		gpiod_set_value(desc, 1);
	WRITE_ONCE(xpon_tx_disable, NULL);
}

static const struct of_device_id xpon_board_of_match[] = {
	{ .compatible = "tplink,archer-xr500v-v1-xpon" },
	{ }
};
MODULE_DEVICE_TABLE(of, xpon_board_of_match);

static struct platform_driver xpon_board_driver = {
	.probe = xpon_board_probe,
	.remove = xpon_board_remove,
	.driver = {
		.name = "econet-xpon-board",
		.of_match_table = xpon_board_of_match,
	},
};

int xpon_board_register(void)
{
	int ret;

	ret = platform_driver_register(&xpon_board_driver);
	if (!ret)
		xpon_board_driver_registered = true;
	return ret;
}

void xpon_board_unregister(void)
{
	if (!xpon_board_driver_registered)
		return;

	/* Keep the kill asserted through every other xPON teardown action. */
	xpon_board_set_tx_disable(true);
	platform_driver_unregister(&xpon_board_driver);
	xpon_board_driver_registered = false;
}
