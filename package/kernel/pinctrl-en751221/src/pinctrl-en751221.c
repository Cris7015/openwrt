// SPDX-License-Identifier: GPL-2.0
/*
 * Pin control for the EcoNet EN751221 SoC.
 *
 * The SoC muxes pads through IOMUX_CONTROL1, at offset 0x104 of the chip SCU
 * block. It is not a regular "n bits per pin" array: each bit enables one
 * (pin, function) pair, the widths differ, and the polarity is not uniform
 * either -- clearing bits 3..7 routes those pads to GPIO, while GPIO31 needs
 * bit 21 set. That is why this is a small SoC-specific driver rather than a
 * pinctrl-single instance.
 *
 * The bit meanings were reconstructed from the OEM GPL tree, not from a
 * datasheet: EN7512LED[] in ledctrl.c gives the LED/GPIO pads, pcmdriver.h
 * gives the voice cluster, and xpon_phy/phy_def.h gives the PON bits. Bits
 * that no OEM code names are deliberately not modelled here.
 */
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf-generic.h>

#define EN751221_IOMUX_CONTROL1		0x104

struct en751221_pinctrl {
	struct pinctrl_dev *pctl;
	struct regmap *regmap;
};

/* The GPIO controllers expose 64 lines; name them all so a group can refer to
 * any pad, even where we cannot yet describe its alternate function.
 */
#define PIN(n) PINCTRL_PIN(n, "gpio" #n)
static const struct pinctrl_pin_desc en751221_pins[] = {
	PIN(0),  PIN(1),  PIN(2),  PIN(3),  PIN(4),  PIN(5),  PIN(6),  PIN(7),
	PIN(8),  PIN(9),  PIN(10), PIN(11), PIN(12), PIN(13), PIN(14), PIN(15),
	PIN(16), PIN(17), PIN(18), PIN(19), PIN(20), PIN(21), PIN(22), PIN(23),
	PIN(24), PIN(25), PIN(26), PIN(27), PIN(28), PIN(29), PIN(30), PIN(31),
	PIN(32), PIN(33), PIN(34), PIN(35), PIN(36), PIN(37), PIN(38), PIN(39),
	PIN(40), PIN(41), PIN(42), PIN(43), PIN(44), PIN(45), PIN(46), PIN(47),
	PIN(48), PIN(49), PIN(50), PIN(51), PIN(52), PIN(53), PIN(54), PIN(55),
	PIN(56), PIN(57), PIN(58), PIN(59), PIN(60), PIN(61), PIN(62), PIN(63),
};
#undef PIN

static const unsigned int grp_gpio3[]  = { 3 };
static const unsigned int grp_gpio7[]  = { 7 };
static const unsigned int grp_gpio8[]  = { 8 };
static const unsigned int grp_gpio9[]  = { 9 };
static const unsigned int grp_gpio10[] = { 10 };
static const unsigned int grp_gpio31[] = { 31 };
static const unsigned int grp_xpon[] = { 16 };
static const unsigned int grp_pcm_reset[] = { 2 };
static const unsigned int grp_zsi2[] = { 4, 5, 6, 7 };

/*
 * One mux choice: which bits of IOMUX_CONTROL1 to change, and to what.
 * A function is applicable to a group when its name appears in the group's
 * list below.
 */
struct en751221_mux {
	const char *function;
	u32 mask;
	u32 val;
};

struct en751221_group {
	const char *name;
	const unsigned int *pins;
	unsigned int npins;
	const struct en751221_mux *muxes;
	unsigned int nmuxes;
};

/* Pads that the OEM LED table describes: clearing the bit hands the pad to
 * the GPIO controller, setting it selects the LED/alternate function.
 */
#define LED_PAD_MUXES(bit)						\
	static const struct en751221_mux muxes_pad##bit[] = {		\
		{ "gpio", BIT(bit), 0 },				\
		{ "led",  BIT(bit), BIT(bit) },				\
	}
LED_PAD_MUXES(3);	/* GPIO10 */
LED_PAD_MUXES(4);	/* GPIO9  */
LED_PAD_MUXES(5);	/* GPIO8  */
LED_PAD_MUXES(6);	/* GPIO7  */
LED_PAD_MUXES(7);	/* GPIO3  */
#undef LED_PAD_MUXES

/* GPIO31 is inverted with respect to the pads above. */
static const struct en751221_mux muxes_gpio31[] = {
	{ "gpio", BIT(21), BIT(21) },
	{ "led",  BIT(21), 0 },
};

/* The EN7512 xPON PHY setup calls bit 15 RG_GPIO_PON_MODE.  On the XR500v
 * this routes the PON-side pad cluster containing GPIO16/TX_DISABLE.  Keep it
 * separate from RG_PON_I2C_MODE (bit 0), which the PHY owns while talking to
 * the external EN7570.
 */
static const struct en751221_mux muxes_xpon[] = {
	{ "xpon", BIT(15), BIT(15) },
};

/* Voice cluster. GPIO_PCM_RESET puts GPIO2 in PCM-reset mode, and
 * GPIO_ZSI_ISI_2nd claims GPIO4..7 for the second SLIC's ZSI/ISI transport.
 */
static const struct en751221_mux muxes_pcm_reset[] = {
	{ "gpio", BIT(10), 0 },
	{ "pcm",  BIT(10), BIT(10) },
};

static const struct en751221_mux muxes_zsi2[] = {
	{ "gpio", BIT(14), 0 },
	{ "zsi",  BIT(14), BIT(14) },
};

#define GROUP(gname, gpins, gmuxes)					\
	{ .name = gname, .pins = gpins, .npins = ARRAY_SIZE(gpins),	\
	  .muxes = gmuxes, .nmuxes = ARRAY_SIZE(gmuxes) }

static const struct en751221_group en751221_groups[] = {
	GROUP("gpio3",     grp_gpio3,     muxes_pad7),
	GROUP("gpio7",     grp_gpio7,     muxes_pad6),
	GROUP("gpio8",     grp_gpio8,     muxes_pad5),
	GROUP("gpio9",     grp_gpio9,     muxes_pad4),
	GROUP("gpio10",    grp_gpio10,    muxes_pad3),
	GROUP("gpio31",    grp_gpio31,    muxes_gpio31),
	GROUP("xpon",      grp_xpon,      muxes_xpon),
	GROUP("pcm_reset", grp_pcm_reset, muxes_pcm_reset),
	GROUP("zsi2",      grp_zsi2,      muxes_zsi2),
};
#undef GROUP

static const char * const fn_gpio_groups[] = {
	"gpio3", "gpio7", "gpio8", "gpio9", "gpio10", "gpio31",
	"pcm_reset", "zsi2",
};
static const char * const fn_led_groups[] = {
	"gpio3", "gpio7", "gpio8", "gpio9", "gpio10", "gpio31",
};
static const char * const fn_pcm_groups[] = { "pcm_reset" };
static const char * const fn_zsi_groups[] = { "zsi2" };
static const char * const fn_xpon_groups[] = { "xpon" };

struct en751221_function {
	const char *name;
	const char * const *groups;
	unsigned int ngroups;
};

#define FUNCTION(fname, fgroups)					\
	{ .name = fname, .groups = fgroups, .ngroups = ARRAY_SIZE(fgroups) }

static const struct en751221_function en751221_functions[] = {
	FUNCTION("gpio", fn_gpio_groups),
	FUNCTION("led",  fn_led_groups),
	FUNCTION("pcm",  fn_pcm_groups),
	FUNCTION("zsi",  fn_zsi_groups),
	FUNCTION("xpon", fn_xpon_groups),
};
#undef FUNCTION

static int en751221_get_groups_count(struct pinctrl_dev *pctl)
{
	return ARRAY_SIZE(en751221_groups);
}

static const char *en751221_get_group_name(struct pinctrl_dev *pctl,
					   unsigned int sel)
{
	return en751221_groups[sel].name;
}

static int en751221_get_group_pins(struct pinctrl_dev *pctl, unsigned int sel,
				   const unsigned int **pins,
				   unsigned int *npins)
{
	*pins = en751221_groups[sel].pins;
	*npins = en751221_groups[sel].npins;
	return 0;
}

static const struct pinctrl_ops en751221_pinctrl_ops = {
	.get_groups_count = en751221_get_groups_count,
	.get_group_name = en751221_get_group_name,
	.get_group_pins = en751221_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_group,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static int en751221_get_functions_count(struct pinctrl_dev *pctl)
{
	return ARRAY_SIZE(en751221_functions);
}

static const char *en751221_get_function_name(struct pinctrl_dev *pctl,
					      unsigned int sel)
{
	return en751221_functions[sel].name;
}

static int en751221_get_function_groups(struct pinctrl_dev *pctl,
					unsigned int sel,
					const char * const **groups,
					unsigned int *ngroups)
{
	*groups = en751221_functions[sel].groups;
	*ngroups = en751221_functions[sel].ngroups;
	return 0;
}

static int en751221_set_mux(struct pinctrl_dev *pctl, unsigned int fsel,
			    unsigned int gsel)
{
	struct en751221_pinctrl *pc = pinctrl_dev_get_drvdata(pctl);
	const struct en751221_group *grp = &en751221_groups[gsel];
	const char *fname = en751221_functions[fsel].name;
	unsigned int i;

	for (i = 0; i < grp->nmuxes; i++) {
		if (strcmp(grp->muxes[i].function, fname))
			continue;

		return regmap_update_bits(pc->regmap, EN751221_IOMUX_CONTROL1,
					  grp->muxes[i].mask,
					  grp->muxes[i].val);
	}

	/* pinmux core already checks the function/group pairing, so this only
	 * happens if the tables above disagree with each other.
	 */
	return -EINVAL;
}

static const struct pinmux_ops en751221_pinmux_ops = {
	.get_functions_count = en751221_get_functions_count,
	.get_function_name = en751221_get_function_name,
	.get_function_groups = en751221_get_function_groups,
	.set_mux = en751221_set_mux,
	.strict = true,
};

static struct pinctrl_desc en751221_pinctrl_desc = {
	.name = "en751221-pinctrl",
	.pins = en751221_pins,
	.npins = ARRAY_SIZE(en751221_pins),
	.pctlops = &en751221_pinctrl_ops,
	.pmxops = &en751221_pinmux_ops,
	.owner = THIS_MODULE,
};

static int en751221_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct en751221_pinctrl *pc;

	pc = devm_kzalloc(dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;

	/* The mux register lives in the chip SCU, so we are a child of it. */
	pc->regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(pc->regmap))
		return dev_err_probe(dev, PTR_ERR(pc->regmap),
				     "no SCU regmap\n");

	pc->pctl = devm_pinctrl_register(dev, &en751221_pinctrl_desc, pc);
	if (IS_ERR(pc->pctl))
		return dev_err_probe(dev, PTR_ERR(pc->pctl),
				     "cannot register pin controller\n");

	return 0;
}

static const struct of_device_id en751221_pinctrl_of[] = {
	{ .compatible = "econet,en751221-pinctrl" },
	{ }
};
MODULE_DEVICE_TABLE(of, en751221_pinctrl_of);

static struct platform_driver en751221_pinctrl_driver = {
	.probe = en751221_pinctrl_probe,
	.driver = {
		.name = "pinctrl-en751221",
		.of_match_table = en751221_pinctrl_of,
	},
};
module_platform_driver(en751221_pinctrl_driver);

MODULE_DESCRIPTION("EcoNet EN751221 pin control");
MODULE_LICENSE("GPL");
