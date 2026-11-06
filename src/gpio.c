/*
 *  Hamlib Interface - gpio support
 *  Copyright (c) 2016 by Jeroen Vreeken
 *
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "hamlib/config.h"

#include <string.h>
#include <errno.h>

#include "gpio.h"
#include "hamlib/port.h"

#ifdef HAVE_LIBGPIOD

/*
 * GPIO character device (/dev/gpiochipN) through libgpiod 2.x.
 *
 * The sysfs interface below (/sys/class/gpio) is deprecated and absent from
 * current kernels, and where it does exist the GPIO numbers it expects are
 * global numbers that no longer match the pin numbers users know (e.g. 512 +
 * BCM number on a Raspberry Pi with kernel 6.6+).
 *
 * Port pathname:
 *   "<offset>"         line <offset> on /dev/gpiochip0 (on a Raspberry Pi the
 *                      BCM GPIO number, e.g. "17")
 *   "<chip>:<offset>"  line <offset> on another chip, where <chip> is a name
 *                      such as "gpiochip4" or a path such as "/dev/gpiochip4"
 *
 * Every port keeps its own line request in port->handle (unused for PTT and
 * DCD ports otherwise), so PTT and DCD can both be GPIO lines, on one chip
 * or on two.  GPION is requested active-low, so the kernel does the
 * inversion: a logical 1 always means PTT/DCD on, and a PTT line is driven
 * to "PTT off" in the request itself -- it never comes up keyed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <gpiod.h>

#define GPIO_DEFAULT_CHIP "/dev/gpiochip0"

static int gpio_parse_pathname(const char *pathname, char *chip,
                               size_t chiplen, unsigned int *offset)
{
    const char *colon = strrchr(pathname, ':');
    const char *num = pathname;
    char *end;
    unsigned long val;
    int n;

    if (colon)
    {
        int len = (int)(colon - pathname);

        if (len == 0)
        {
            return -RIG_EINVAL;
        }

        n = snprintf(chip, chiplen, "%s%.*s",
                     pathname[0] == '/' ? "" : "/dev/", len, pathname);
        num = colon + 1;
    }
    else
    {
        n = snprintf(chip, chiplen, "%s", GPIO_DEFAULT_CHIP);
    }

    if (n < 0 || (size_t) n >= chiplen)
    {
        return -RIG_EINVAL;
    }

    if (num[0] < '0' || num[0] > '9')
    {
        return -RIG_EINVAL;
    }

    errno = 0;
    val = strtoul(num, &end, 10);

    if (errno != 0 || *end != '\0' || val > UINT_MAX)
    {
        return -RIG_EINVAL;
    }

    *offset = (unsigned int) val;
    return RIG_OK;
}


int gpio_open(hamlib_port_t *port, int output, int on_value)
{
    char chip_path[HAMLIB_FILPATHLEN + 8];
    unsigned int offset;
    struct gpiod_chip *chip;
    struct gpiod_line_settings *settings;
    struct gpiod_line_config *line_cfg;
    struct gpiod_request_config *req_cfg;
    struct gpiod_line_request *req = NULL;
    int err = 0;

    port->parm.gpio.on_value = on_value;
    port->parm.gpio.value = 0;
    port->handle = NULL;

    if (gpio_parse_pathname(port->pathname, chip_path, sizeof(chip_path),
                            &offset) != RIG_OK)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "%s: invalid GPIO \"%s\", expected <offset> or <chip>:<offset>\n",
                  __func__, port->pathname);
        return -RIG_EINVAL;
    }

    rig_debug(RIG_DEBUG_VERBOSE, "%s: %s line %u as %s, active-%s\n", __func__,
              chip_path, offset, output ? "output" : "input",
              on_value ? "high" : "low");

    chip = gpiod_chip_open(chip_path);

    if (!chip)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: open %s: %s\n", __func__, chip_path,
                  strerror(errno));
        return -RIG_EIO;
    }

    settings = gpiod_line_settings_new();
    line_cfg = gpiod_line_config_new();
    req_cfg = gpiod_request_config_new();

    if (!settings || !line_cfg || !req_cfg)
    {
        err = ENOMEM;
    }
    else
    {
        gpiod_line_settings_set_active_low(settings, on_value == 0);

        if (output)
        {
            gpiod_line_settings_set_direction(settings,
                                              GPIOD_LINE_DIRECTION_OUTPUT);
            gpiod_line_settings_set_output_value(settings,
                                                 GPIOD_LINE_VALUE_INACTIVE);
        }
        else
        {
            gpiod_line_settings_set_direction(settings,
                                              GPIOD_LINE_DIRECTION_INPUT);
        }

        gpiod_request_config_set_consumer(req_cfg,
                                          output ? "hamlib-ptt" : "hamlib-dcd");

        if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1,
                                                settings) < 0)
        {
            err = errno;
        }
        else
        {
            req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

            if (!req)
            {
                err = errno;
            }
        }
    }

    if (req_cfg) { gpiod_request_config_free(req_cfg); }

    if (line_cfg) { gpiod_line_config_free(line_cfg); }

    if (settings) { gpiod_line_settings_free(settings); }

    /* the request has its own fd; the chip is not needed to keep the line */
    gpiod_chip_close(chip);

    if (!req)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: request %s line %u: %s\n", __func__,
                  chip_path, offset, strerror(err));
        return -RIG_EIO;
    }

    port->handle = req;
    port->fd = gpiod_line_request_get_fd(req);
    return port->fd;
}


int gpio_close(hamlib_port_t *port)
{
    if (port->handle)
    {
        gpiod_line_request_release((struct gpiod_line_request *) port->handle);
        port->handle = NULL;
    }

    port->fd = -1;
    return RIG_OK;
}


int gpio_ptt_set(hamlib_port_t *port, ptt_t pttx)
{
    struct gpiod_line_request *req = port->handle;
    enum gpiod_line_value val;

    if (!req)
    {
        return -RIG_EIO;
    }

    val = pttx != RIG_PTT_OFF ? GPIOD_LINE_VALUE_ACTIVE
          : GPIOD_LINE_VALUE_INACTIVE;

    if (gpiod_line_request_set_values(req, &val) < 0)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: GPIO%s: %s\n", __func__, port->pathname,
                  strerror(errno));
        return -RIG_EIO;
    }

    port->parm.gpio.value = pttx != RIG_PTT_OFF;
    return RIG_OK;
}


int gpio_ptt_get(hamlib_port_t *port, ptt_t *pttx)
{
    *pttx = port->parm.gpio.value ? RIG_PTT_ON : RIG_PTT_OFF;
    return RIG_OK;
}


int gpio_dcd_get(hamlib_port_t *port, dcd_t *dcdx)
{
    struct gpiod_line_request *req = port->handle;
    enum gpiod_line_value val;

    if (!req)
    {
        return -RIG_EIO;
    }

    if (gpiod_line_request_get_values(req, &val) < 0)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: GPIO%s: %s\n", __func__, port->pathname,
                  strerror(errno));
        return -RIG_EIO;
    }

    rig_debug(RIG_DEBUG_VERBOSE, "%s: GPIO%s logical value %d\n", __func__,
              port->pathname, (int) val);

    *dcdx = val == GPIOD_LINE_VALUE_ACTIVE ? RIG_DCD_ON : RIG_DCD_OFF;
    return RIG_OK;
}

#else /* !HAVE_LIBGPIOD: legacy sysfs interface */

#include <unistd.h>
#include <fcntl.h>


int gpio_open(hamlib_port_t *port, int output, int on_value)
{
    char pathname[HAMLIB_FILPATHLEN * 2];
    FILE *fexp, *fdir;
    int fd;
    char *dir;

    port->parm.gpio.on_value = on_value;

    SNPRINTF(pathname, HAMLIB_FILPATHLEN, "/sys/class/gpio/export");
    fexp = fopen(pathname, "w");

    if (!fexp)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "Export GPIO%s (using %s): %s\n",
                  port->pathname,
                  pathname,
                  strerror(errno));
        return -RIG_EIO;
    }

    fprintf(fexp, "%s\n", port->pathname);
    fclose(fexp);

    SNPRINTF(pathname,
             sizeof(pathname),
             "/sys/class/gpio/gpio%s/direction",
             port->pathname);
    fdir = fopen(pathname, "w");

    if (!fdir)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "GPIO%s direction (using %s): %s\n",
                  port->pathname,
                  pathname,
                  strerror(errno));
        return -RIG_EIO;
    }

    dir = output ? "out" : "in";
    rig_debug(RIG_DEBUG_VERBOSE, "Setting direction of GPIO%s to %s\n",
              port->pathname, dir);
    fprintf(fdir, "%s\n", dir);
    fclose(fdir);

    SNPRINTF(pathname,
             sizeof(pathname),
             "/sys/class/gpio/gpio%s/value",
             port->pathname);
    fd = open(pathname, O_RDWR);

    if (fd < 0)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "GPIO%s opening value file %s: %s\n",
                  port->pathname,
                  pathname,
                  strerror(errno));
        return -RIG_EIO;
    }

    port->fd = fd;
    return fd;
}


int gpio_close(hamlib_port_t *port)
{
    int retval;
    char pathname[HAMLIB_FILPATHLEN * 2];
    FILE *fexp;

    retval = close(port->fd);

    SNPRINTF(pathname, HAMLIB_FILPATHLEN, "/sys/class/gpio/unexport");
    fexp = fopen(pathname, "w");

    if (!fexp)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "Export GPIO%s (using %s): %s\n",
                  port->pathname,
                  pathname,
                  strerror(errno));
        return -RIG_EIO;
    }

    fprintf(fexp, "%s\n", port->pathname);
    fclose(fexp);
    return retval;
}


int gpio_ptt_set(hamlib_port_t *port, ptt_t pttx)
{
    char *val;
    port->parm.gpio.value = pttx != RIG_PTT_OFF;

    if ((port->parm.gpio.value && port->parm.gpio.on_value)
            || (!port->parm.gpio.value && !port->parm.gpio.on_value))
    {
        val = "1\n";
    }
    else
    {
        val = "0\n";
    }

    if (write(port->fd, val, strlen(val)) <= 0)
    {
        return -RIG_EIO;
    }

    return RIG_OK;
}


int gpio_ptt_get(hamlib_port_t *port, ptt_t *pttx)
{
    if (port->parm.gpio.value)
    {
        *pttx = RIG_PTT_ON;
    }
    else
    {
        *pttx = RIG_PTT_OFF;
    }

    return RIG_OK;
}

int gpio_dcd_get(hamlib_port_t *port, dcd_t *dcdx)
{
    char val;
    int port_value;

    lseek(port->fd, 0, SEEK_SET);

    if (read(port->fd, &val, sizeof(val)) <= 0)
    {
        return -RIG_EIO;
    }

    rig_debug(RIG_DEBUG_VERBOSE, "DCD GPIO pin value: %c\n", val);

    port_value = val - '0';

    if (port_value == port->parm.gpio.on_value)
    {
        *dcdx = RIG_DCD_ON;
    }
    else
    {
        *dcdx = RIG_DCD_OFF;
    }

    return RIG_OK;
}

#endif /* HAVE_LIBGPIOD */
