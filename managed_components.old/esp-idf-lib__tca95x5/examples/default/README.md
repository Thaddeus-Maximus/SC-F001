## What the example does

* Initializes the driver
* Configure a pin as input
* Registers an interrupt that logs port value of the INT pin
* Blinks, or toggles, P10 output

## Wiring

Connect an LED with an appropriate register to P10.

Connect a switch that toggle its voltage HIGH or LOW to INT GPIO. The INT GPIO
can be set in `menuconfig`. See the defaults in `Kconfig.projbuild`.
