NAME := ssd1306-2
IP ?= 10.42.0.236

.PHONY: build

build:
	arduino-cli compile --fqbn sophgo:SG200X:oz64 --build-property "compiler.cpp.extra_flags=-DPICO_RISCV" --build-path build .

upload:
	scp build/$(NAME).ino.elf debian@$(IP):/tmp/$(NAME).elf
