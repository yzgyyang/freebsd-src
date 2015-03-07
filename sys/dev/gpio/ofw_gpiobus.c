/*-
 * Copyright (c) 2009, Nathan Whitehorn <nwhitehorn@FreeBSD.org>
 * Copyright (c) 2013, Luiz Otavio O Souza <loos@FreeBSD.org>
 * Copyright (c) 2013 The FreeBSD Foundation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>

#include <dev/gpio/gpiobusvar.h>
#include <dev/ofw/ofw_bus.h>

static struct ofw_gpiobus_devinfo *ofw_gpiobus_setup_devinfo(device_t,
	device_t, phandle_t);
static void ofw_gpiobus_destroy_devinfo(device_t, struct ofw_gpiobus_devinfo *);
static int ofw_gpiobus_parse_gpios_impl(device_t, phandle_t, char *,
	struct gpiobus_softc *, struct gpiobus_pin **);

device_t
ofw_gpiobus_add_fdt_child(device_t bus, const char *drvname, phandle_t child)
{
	device_t childdev;
	struct ofw_gpiobus_devinfo *dinfo;

	/*
	 * Set up the GPIO child and OFW bus layer devinfo and add it to bus.
	 */
	childdev = device_add_child(bus, drvname, -1);
	if (childdev == NULL)
		return (NULL);
	dinfo = ofw_gpiobus_setup_devinfo(bus, childdev, child);
	if (dinfo == NULL) {
		device_delete_child(bus, childdev);
		return (NULL);
	}
	if (device_probe_and_attach(childdev) != 0) {
		ofw_gpiobus_destroy_devinfo(bus, dinfo);
		device_delete_child(bus, childdev);
		return (NULL);
	}

	return (childdev);
}

int
ofw_gpiobus_parse_gpios(device_t consumer, char *pname,
	struct gpiobus_pin **pins)
{

	return (ofw_gpiobus_parse_gpios_impl(consumer,
	    ofw_bus_get_node(consumer), pname, NULL, pins));
}

void
ofw_gpiobus_register_provider(device_t provider)
{
	phandle_t node;

	node = ofw_bus_get_node(provider);
	OF_device_register_xref(OF_xref_from_node(node), provider);
}

void
ofw_gpiobus_unregister_provider(device_t provider)
{
	phandle_t node;

	node = ofw_bus_get_node(provider);
	OF_device_register_xref(OF_xref_from_node(node), NULL);
}

static struct ofw_gpiobus_devinfo *
ofw_gpiobus_setup_devinfo(device_t bus, device_t child, phandle_t node)
{
	int i, npins;
	struct gpiobus_ivar *devi;
	struct gpiobus_pin *pins;
	struct gpiobus_softc *sc;
	struct ofw_gpiobus_devinfo *dinfo;

	sc = device_get_softc(bus);
	dinfo = malloc(sizeof(*dinfo), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (dinfo == NULL)
		return (NULL);
	if (ofw_bus_gen_setup_devinfo(&dinfo->opd_obdinfo, node) != 0) {
		free(dinfo, M_DEVBUF);
		return (NULL);
	}
	/* Parse the gpios property for the child. */
	npins = ofw_gpiobus_parse_gpios_impl(child, node, "gpios", sc, &pins);
	if (npins <= 0) {
		ofw_bus_gen_destroy_devinfo(&dinfo->opd_obdinfo);
		free(dinfo, M_DEVBUF);
		return (NULL);
	}
	/* Initialize the irq resource list. */
	resource_list_init(&dinfo->opd_dinfo.rl);
	/* Allocate the child ivars and copy the parsed pin data. */
	devi = &dinfo->opd_dinfo;
	devi->npins = (uint32_t)npins;
	if (gpiobus_alloc_ivars(devi) != 0) {
		free(pins, M_DEVBUF);
		ofw_gpiobus_destroy_devinfo(bus, dinfo);
		return (NULL);
	}
	for (i = 0; i < devi->npins; i++) {
		devi->flags[i] = pins[i].flags;
		devi->pins[i] = pins[i].pin;
	}
	free(pins, M_DEVBUF);
	/* Parse the interrupt resources. */
	if (ofw_bus_intr_to_rl(bus, node, &dinfo->opd_dinfo.rl) != 0) {
		ofw_gpiobus_destroy_devinfo(bus, dinfo);
		return (NULL);
	}
	device_set_ivars(child, dinfo);

	return (dinfo);
}

static void
ofw_gpiobus_destroy_devinfo(device_t bus, struct ofw_gpiobus_devinfo *dinfo)
{
	int i;
	struct gpiobus_ivar *devi;
	struct gpiobus_softc *sc;

	sc = device_get_softc(bus);
	devi = &dinfo->opd_dinfo;
	for (i = 0; i < devi->npins; i++) {
		if (devi->pins[i] > sc->sc_npins)
			continue;
		sc->sc_pins_mapped[devi->pins[i]] = 0;
	}
	gpiobus_free_ivars(devi);
	resource_list_free(&dinfo->opd_dinfo.rl);
	ofw_bus_gen_destroy_devinfo(&dinfo->opd_obdinfo);
	free(dinfo, M_DEVBUF);
}

static int
ofw_gpiobus_parse_gpios_impl(device_t consumer, phandle_t cnode, char *pname,
	struct gpiobus_softc *bussc, struct gpiobus_pin **pins)
{
	int gpiocells, i, j, ncells, npins;
	pcell_t *gpios;
	phandle_t gpio;

	ncells = OF_getencprop_alloc(cnode, pname, sizeof(*gpios),
            (void **)&gpios);
	if (ncells == -1) {
		device_printf(consumer,
		    "Warning: No %s specified in fdt data; "
		    "device may not function.\n", pname);
		return (-1);
	}
	/*
	 * The gpio-specifier is controller independent, the first pcell has
	 * the reference to the GPIO controller phandler.
	 * Count the number of encoded gpio-specifiers on the first pass.
	 */
	i = 0;
	npins = 0;
	while (i < ncells) {
		/* Allow NULL specifiers. */
		if (gpios[i] == 0) {
			npins++;
			i++;
			continue;
		}
		gpio = OF_node_from_xref(gpios[i]);
		/* If we have bussc, ignore devices from other gpios. */
		if (bussc != NULL)
			if (ofw_bus_get_node(bussc->sc_dev) != gpio)
				return (0);
		/*
		 * Check for gpio-controller property and read the #gpio-cells
		 * for this GPIO controller.
		 */
		if (!OF_hasprop(gpio, "gpio-controller") ||
		    OF_getencprop(gpio, "#gpio-cells", &gpiocells,
		    sizeof(gpiocells)) < 0) {
			device_printf(consumer,
			    "gpio reference is not a gpio-controller.\n");
			free(gpios, M_OFWPROP);
			return (-1);
		}
		if (ncells - i < gpiocells + 1) {
			device_printf(consumer,
			    "%s cells doesn't match #gpio-cells.\n", pname);
			return (-1);
		}
		npins++;
		i += gpiocells + 1;
	}
	if (npins == 0 || pins == NULL) {
		if (npins == 0)
			device_printf(consumer, "no pin specified in %s.\n",
			    pname);
		free(gpios, M_OFWPROP);
		return (npins);
	}
	*pins = malloc(sizeof(struct gpiobus_pin) * npins, M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (*pins == NULL) {
		free(gpios, M_OFWPROP);
		return (-1);
	}
	/* Decode the gpio specifier on the second pass. */
	i = 0;
	j = 0;
	while (i < ncells) {
		/* Allow NULL specifiers. */
		if (gpios[i] == 0) {
			j++;
			i++;
			continue;
		}
		gpio = OF_node_from_xref(gpios[i]);
		/* Read gpio-cells property for this GPIO controller. */
		if (OF_getencprop(gpio, "#gpio-cells", &gpiocells,
		    sizeof(gpiocells)) < 0) {
			device_printf(consumer,
			    "gpio does not have the #gpio-cells property.\n");
			goto fail;
		}
		/* Return the device reference for the GPIO controller. */
		(*pins)[j].dev = OF_device_from_xref(gpios[i]);
		if ((*pins)[j].dev == NULL) {
			device_printf(consumer,
			    "no device registered for the gpio controller.\n");
			goto fail;
		}
		/*
		 * If the gpiobus softc is NULL we use the GPIO_GET_BUS() to
		 * retrieve it.  The GPIO_GET_BUS() method is only valid after
		 * the child is probed and attached.
		 */
		if (bussc == NULL) {
			if (GPIO_GET_BUS((*pins)[j].dev) == NULL) {
				device_printf(consumer,
				    "no gpiobus reference for %s.\n",
				    device_get_nameunit((*pins)[j].dev));
				goto fail;
			}
			bussc = device_get_softc(GPIO_GET_BUS((*pins)[j].dev));
		}
		/* Get the GPIO pin number and flags. */
		if (gpio_map_gpios((*pins)[j].dev, cnode, gpio, gpiocells,
		    &gpios[i + 1], &(*pins)[j].pin, &(*pins)[j].flags) != 0) {
			device_printf(consumer,
			    "cannot map the gpios specifier.\n");
			goto fail;
		}
		/* Reserve the GPIO pin. */
		if (gpiobus_map_pin(bussc->sc_busdev, (*pins)[j].pin) != 0)
			goto fail;
		j++;
		i += gpiocells + 1;
	}
	free(gpios, M_OFWPROP);

	return (npins);

fail:
	free(gpios, M_OFWPROP);
	free(*pins, M_DEVBUF);
	return (-1);
}

static int
ofw_gpiobus_probe(device_t dev)
{

	if (ofw_bus_get_node(dev) == -1)
		return (ENXIO);
	device_set_desc(dev, "OFW GPIO bus");

	return (0);
}

static int
ofw_gpiobus_attach(device_t dev)
{
	int err;
	phandle_t child;

	err = gpiobus_init_softc(dev);
	if (err != 0)
		return (err);
	bus_generic_probe(dev);
	bus_enumerate_hinted_children(dev);
	/*
	 * Attach the children represented in the device tree.
	 */
	for (child = OF_child(ofw_bus_get_node(dev)); child != 0;
	    child = OF_peer(child)) {
		if (!OF_hasprop(child, "gpios"))
			continue;
		if (ofw_gpiobus_add_fdt_child(dev, NULL, child) == NULL)
			continue;
	}

	return (bus_generic_attach(dev));
}

static device_t
ofw_gpiobus_add_child(device_t dev, u_int order, const char *name, int unit)
{
	device_t child;
	struct ofw_gpiobus_devinfo *devi;

	child = device_add_child_ordered(dev, order, name, unit);
	if (child == NULL)
		return (child);
	devi = malloc(sizeof(struct ofw_gpiobus_devinfo), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (devi == NULL) {
		device_delete_child(dev, child);
		return (0);
	}

	/*
	 * NULL all the OFW-related parts of the ivars for non-OFW
	 * children.
	 */
	devi->opd_obdinfo.obd_node = -1;
	devi->opd_obdinfo.obd_name = NULL;
	devi->opd_obdinfo.obd_compat = NULL;
	devi->opd_obdinfo.obd_type = NULL;
	devi->opd_obdinfo.obd_model = NULL;

	device_set_ivars(child, devi);

	return (child);
}

static const struct ofw_bus_devinfo *
ofw_gpiobus_get_devinfo(device_t bus, device_t dev)
{
	struct ofw_gpiobus_devinfo *dinfo;

	dinfo = device_get_ivars(dev);

	return (&dinfo->opd_obdinfo);
}

static device_method_t ofw_gpiobus_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		ofw_gpiobus_probe),
	DEVMETHOD(device_attach,	ofw_gpiobus_attach),

	/* Bus interface */
	DEVMETHOD(bus_child_pnpinfo_str,	ofw_bus_gen_child_pnpinfo_str),
	DEVMETHOD(bus_add_child,	ofw_gpiobus_add_child),

	/* ofw_bus interface */
	DEVMETHOD(ofw_bus_get_devinfo,	ofw_gpiobus_get_devinfo),
	DEVMETHOD(ofw_bus_get_compat,	ofw_bus_gen_get_compat),
	DEVMETHOD(ofw_bus_get_model,	ofw_bus_gen_get_model),
	DEVMETHOD(ofw_bus_get_name,	ofw_bus_gen_get_name),
	DEVMETHOD(ofw_bus_get_node,	ofw_bus_gen_get_node),
	DEVMETHOD(ofw_bus_get_type,	ofw_bus_gen_get_type),

	DEVMETHOD_END
};

static devclass_t ofwgpiobus_devclass;

DEFINE_CLASS_1(gpiobus, ofw_gpiobus_driver, ofw_gpiobus_methods,
    sizeof(struct gpiobus_softc), gpiobus_driver);
DRIVER_MODULE(ofw_gpiobus, gpio, ofw_gpiobus_driver, ofwgpiobus_devclass, 0, 0);
MODULE_VERSION(ofw_gpiobus, 1);
MODULE_DEPEND(ofw_gpiobus, gpiobus, 1, 1, 1);
