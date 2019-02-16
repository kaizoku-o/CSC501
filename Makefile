#
# By default, the build is done against the running linux kernel source.
# To build against a different kernel source tree, set SYSSRC:
#
#    make SYSSRC=/path/to/kernel/source
Subsystem:
	cd kernel_module && $(MAKE)
	cd library && $(MAKE)
	cd benchmark && $(MAKE)
