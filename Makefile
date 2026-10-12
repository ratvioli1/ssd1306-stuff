NAME := ssd1306-2
IP ?= 10.42.0.236

.PHONY: build

build:
	arduino-cli compile --fqbn sophgo:SG200X:oz64 --build-property "compiler.cpp.extra_flags=-DPICO_RISCV" --build-path build .

upload:
	scp build/$(NAME).ino.elf debian@$(IP):/tmp/$(NAME).elf

deploy: upload
	ssh -tt debian@$(IP) "sudo sh -ec '\
		if grep -qx running /sys/class/remoteproc/remoteproc0/state; then \
			echo stop > /sys/class/remoteproc/remoteproc0/state; \
		fi; \
		cp /tmp/$(NAME).elf /lib/firmware/$(NAME).elf; \
		echo $(NAME).elf > /sys/class/remoteproc/remoteproc0/firmware; \
		echo start > /sys/class/remoteproc/remoteproc0/state; \
		echo yay \
	'"
