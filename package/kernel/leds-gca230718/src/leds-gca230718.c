// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCA230718 LED support (e.g. for D-Link M30) using I2C
 *
 * Copyright 2022 Roland Reinl <reinlroland+github@gmail.com>
 *
 * This driver can control RGBW LEDs which are connected to a GCA230718.
 */

#include <linux/delay.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/version.h>

#define GCA230718_MAX_LEDS (4u)

#define GCA230718_OPMODE_DISABLED (0x00u)
#define GCA230718_OPMODE_NO_TOGGLE (0x01u)
#define GCA230718_OPMODE_TOGGLE_RAMP_CONTROL_DISABLED (0x02u)
#define GCA230718_OPMODE_TOGGLE_RAMP_CONTROL_ENSABLED (0x03u)

#define GCA230718_1ST_SEQUENCE_BYTE_1 (0x02u)
#define GCA230718_2ND_SEQUENCE_BYTE_1 (0x01u)
#define GCA230718_3RD_SEQUENCE_BYTE_1 (0x03u)

struct gca230718_led {
	enum led_brightness brightness;
	struct i2c_client *client;
	struct led_classdev ledClassDev;
};

struct gca230718_private {
	struct mutex lock;
	struct gca230718_led leds[GCA230718_MAX_LEDS];
};

static void gca230718_send_sequence(struct i2c_client *client, u8 byte0,
				    struct gca230718_private *priv)
{
	int status = 0;
	u8 ledIndex;
	const u8 resetCommand[2] = { 0x81, 0xE4 };
	const u8 resetCommandRegister = 0x00;

	u8 controlCommand[13];
	const u8 controlCommandRegister = 0x03;

	controlCommand[0] = 0x0C; /* Unknown */
	controlCommand[1] = byte0;
	controlCommand[2] = GCA230718_OPMODE_NO_TOGGLE;
	/* Byte 3-6 are set below to the brighness value of the individual LEDs */
	controlCommand[7] =
		0x01; /* Frequency, doesn't care as long as GCA230718_OPMODE_NO_TOGGLE is used above */
	/* Byte 8-11 are set below to the brighness value of the individual LEDs */
	controlCommand[12] = 0x87;

	for (ledIndex = 0; ledIndex < GCA230718_MAX_LEDS; ledIndex++) {
		controlCommand[3 + ledIndex] = priv->leds[ledIndex].brightness;
		controlCommand[8 + ledIndex] = priv->leds[ledIndex].brightness;
	}

	mutex_lock(&(priv->lock));

	if ((status = i2c_smbus_write_i2c_block_data(
		     client, resetCommandRegister, sizeof(resetCommand),
		     resetCommand)) != 0) {
		pr_info("Error %i during call of i2c_smbus_write_i2c_block_data for reset command\n",
			status);
	} else if ((status = i2c_smbus_write_i2c_block_data(
			    client, controlCommandRegister,
			    sizeof(controlCommand), controlCommand)) != 0) {
		pr_info("Error %i during call of i2c_smbus_write_i2c_block_data for control command\n",
			status);
	}

	mutex_unlock(&(priv->lock));
}

static int gca230718_set_brightness(struct led_classdev *led_cdev,
				    enum led_brightness value)
{
	struct gca230718_led *led;
	struct i2c_client *client;

	led = container_of(led_cdev, struct gca230718_led, ledClassDev);
	client = led->client;

	if (client) {
		struct gca230718_private *priv;

		led->brightness = value;
		priv = i2c_get_clientdata(client);

		gca230718_send_sequence(client, GCA230718_2ND_SEQUENCE_BYTE_1,
					priv);
	}

	return 0;
}

static int gca230718_probe(struct i2c_client *client)
{
	struct gca230718_private *priv;
	int err;

	pr_info("Enter gca230718_probe for device address %u\n", client->addr);
	priv = devm_kzalloc(&client->dev, sizeof(struct gca230718_private),
			    GFP_KERNEL);
	if (!priv) {
		pr_info("Error during allocating memory for private data\n");
		return -ENOMEM;
	}

	err = devm_mutex_init(&client->dev, &priv->lock);
	if (err)
		return err;

	i2c_set_clientdata(client, priv);

	struct device_node *ledNode;
	for_each_child_of_node(client->dev.of_node, ledNode) {
		u32 regValue = 0;
		if (of_property_read_u32(ledNode, "reg", &regValue))
			pr_info("Missing entry \"reg\" in node %s\n",
				ledNode->name);
		else if (regValue >= GCA230718_MAX_LEDS)
			pr_info("Invalid entry \"reg\" in node %s (%u)\n",
				ledNode->name, regValue);
		else {
			struct led_classdev *ledClassDev =
				&(priv->leds[regValue].ledClassDev);
			struct led_init_data init_data = {};

			priv->leds[regValue].client = client;
			init_data.fwnode = of_fwnode_handle(ledNode);

			pr_info("Creating LED for node %s: reg=%u\n",
				ledNode->name, regValue);

			ledClassDev->name =
				of_get_property(ledNode, "label", NULL);
			if (!ledClassDev->name)
				ledClassDev->name = ledNode->name;

			ledClassDev->brightness = LED_OFF;
			ledClassDev->max_brightness = LED_FULL;
			ledClassDev->brightness_set_blocking =
				gca230718_set_brightness;

			if (devm_led_classdev_register_ext(
				    &client->dev, ledClassDev, &init_data))
				pr_info("Error during call of devm_led_classdev_register_ext");
		}
	}

	/*
	Send full initialization sequence.
	Afterwards only GCA230718_2ND_SEQUENCE_BYTE_1 must be send to upddate the brightness values.
	*/
	gca230718_send_sequence(client, GCA230718_1ST_SEQUENCE_BYTE_1, priv);
	gca230718_send_sequence(client, GCA230718_2ND_SEQUENCE_BYTE_1, priv);
	gca230718_send_sequence(client, GCA230718_3RD_SEQUENCE_BYTE_1, priv);

	return 0;
}

static const struct i2c_device_id gca230718_i2c_ids[] = {
	{ "gca230718", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, gca230718_i2c_ids);

static const struct of_device_id gca230718_dt_ids[] = {
	{ .compatible = "unknown,gca230718" },
	{},
};
MODULE_DEVICE_TABLE(of, gca230718_dt_ids);

static struct i2c_driver gca230718_driver = {
	.probe		= gca230718_probe,
	.id_table	= gca230718_i2c_ids,
	.driver = {
		.name		= KBUILD_MODNAME,
		.of_match_table	= gca230718_dt_ids,
	},
};

module_i2c_driver(gca230718_driver);

MODULE_AUTHOR("Roland Reinl <reinlroland+github@gmail.com>");
MODULE_DESCRIPTION("GCA230718 LED support (e.g. for D-Link M30) using I2C");
MODULE_LICENSE("GPL");
