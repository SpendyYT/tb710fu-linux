#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

// From datasheet
#define AW37504_VOUTP_REG		0x00 
#define AW37504_VOUTN_REG		0x01 

// From IDA. 
#define AW37504_VOUTP_VOLTAGE		0x12
#define AW37504_VOUTN_VOLTAGE		0x12

struct aw37504_data {
    struct regmap *regmap;
    struct gpio_desc *reset_gpio[2];
    bool enabled;
};

static const struct regmap_config aw37504_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x01, /* MAX REGISTER IS 0X21 from DataSheet regmap*/
};

static int aw37504_enable(struct regulator_dev *rdev)
{
    struct aw37504_data *data = rdev_get_drvdata(rdev);
    struct regmap *regmap = data->regmap;
    int ret = 0;

    // Reset GPIO
    if (data->reset_gpio[0]) {
        gpiod_set_value_cansleep(data->reset_gpio[0], 1);
    }
    if (data->reset_gpio[1]) {
        gpiod_set_value_cansleep(data->reset_gpio[1], 1);
    }
	
    // Set voltage 5.8V
    ret |= regmap_write(regmap, AW37504_VOUTP_REG, AW37504_VOUTP_VOLTAGE);

    ret |= regmap_write(regmap, AW37504_VOUTN_REG, AW37504_VOUTN_VOLTAGE);
    if (ret) {
	dev_err(rdev->dev.parent, "Failed to enable AW37504 regulator\n");
        return ret;
    }
	
    data->enabled = true;
    return 0;
}


static int aw37504_disable(struct regulator_dev *rdev)
{
    struct aw37504_data *data = rdev_get_drvdata(rdev);

    if (data->reset_gpio[0]) {
        gpiod_set_value_cansleep(data->reset_gpio[0], 0);
    }
    if (data->reset_gpio[1]) {
        gpiod_set_value_cansleep(data->reset_gpio[1], 0);
    }

    data->enabled = false;
    return 0;
}

static int aw37504_is_enabled(struct regulator_dev *rdev)
{
    struct aw37504_data *data = rdev_get_drvdata(rdev);
    return data->enabled;
}

static int aw37504_get_voltage(struct regulator_dev *rdev)
{
    return 5800000;  // 5.8V
}

static const struct regulator_ops aw37504_ops = {
    .enable = aw37504_enable,
    .disable = aw37504_disable,
    .is_enabled = aw37504_is_enabled,
    .get_voltage = aw37504_get_voltage,
};

static const struct regulator_desc aw37504_reg = {
    .name = "AW37504",
    .id = 0,
    .ops = &aw37504_ops,
    .type = REGULATOR_VOLTAGE,
    .n_voltages = 1,
    .min_uV = 5800000,
    .owner = THIS_MODULE,
};

static int aw37504_i2c_probe(struct i2c_client *i2c)
{
    struct device *dev = &i2c->dev;
    struct regulator_config config = { };
    struct regulator_dev *rdev;
    struct aw37504_data *data;
    int error;

    data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->regmap = devm_regmap_init_i2c(i2c, &aw37504_regmap_config);
    if (IS_ERR(data->regmap))
        return dev_err_probe(dev, PTR_ERR(data->regmap), "failed to init regmap\n");

    /* Get reset-gpio from device tree */
    data->reset_gpio[0] = devm_gpiod_get_index(dev, "reset", 0, GPIOD_OUT_HIGH);
    if (IS_ERR(data->reset_gpio[0]))
        return dev_err_probe(dev, PTR_ERR(data->reset_gpio[0]), "failed to get first reset GPIO\n");

    data->reset_gpio[1] = devm_gpiod_get_index(dev, "reset", 1, GPIOD_OUT_HIGH);
    if (IS_ERR(data->reset_gpio[1]))
        dev_warn(dev, "failed to get second reset GPIO\n");

    config.dev = dev;
    config.regmap = data->regmap;
    config.driver_data = data;
    config.of_node = dev->of_node;
    config.init_data = of_get_regulator_init_data(dev, dev->of_node, &aw37504_reg);
    if (!config.init_data)
        return -ENOMEM;
    data->enabled = false;
    rdev = devm_regulator_register(dev, &aw37504_reg, &config);
    if (IS_ERR(rdev)) {
        error = PTR_ERR(rdev);
        dev_err(dev, "Failed to register aw37504 regulator: %d\n", error);
        return error;
    }

    return 0;
}

static const struct i2c_device_id aw37504_i2c_id[] = {
    { "aw37504" },
    { }
};
MODULE_DEVICE_TABLE(i2c, aw37504_i2c_id);

static const struct of_device_id aw37504_i2c_of_match[] = {
    { .compatible = "awinic,aw37504" },
    { }
};
MODULE_DEVICE_TABLE(of, aw37504_i2c_of_match);

static struct i2c_driver aw37504_regulator_driver = {
    .driver = {
        .name = "aw37504",
        .probe_type = PROBE_PREFER_ASYNCHRONOUS,
        .of_match_table = aw37504_i2c_of_match,
    },
    .probe = aw37504_i2c_probe,
    .id_table = aw37504_i2c_id,
};

module_i2c_driver(aw37504_regulator_driver);

MODULE_DESCRIPTION("Awinic aw37504 reg Driver");
MODULE_AUTHOR("Vladislav Tumarev <tumarev.workspace@gmail.com>");
MODULE_LICENSE("GPL");
