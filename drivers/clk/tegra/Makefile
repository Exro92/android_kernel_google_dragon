obj-y					+= clk.o
obj-y					+= clk-audio-sync.o
obj-y					+= clk-dfll.o
obj-y					+= clk-divider.o
obj-y					+= clk-emc.o
obj-y					+= clk-periph.o
obj-y					+= clk-periph-gate.o
obj-y					+= clk-pll.o
obj-y					+= clk-pll-out.o
obj-y					+= clk-super.o
obj-y					+= clk-tegra-audio.o
obj-y					+= clk-tegra-periph.o
obj-y					+= clk-tegra-pmc.o
obj-y					+= clk-tegra-fixed.o
obj-y					+= clk-tegra-super-gen4.o
obj-y					+= clk-tegra-super-gen5.o
obj-y					+= clk-cbus-shared.o
obj-y					+= clk-tegra-shared.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)         += clk-tegra20.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)         += clk-tegra30.o
obj-$(CONFIG_ARCH_TEGRA_114_SOC)	+= clk-tegra114.o
ifneq ($(filter y, $(CONFIG_ARCH_TEGRA_124_SOC) $(CONFIG_ARCH_TEGRA_132_SOC)),)
obj-y					+= clk-tegra124.o
endif
ifneq ($(filter y, $(CONFIG_ARCH_TEGRA_124_SOC) $(CONFIG_ARCH_TEGRA_210_SOC)),)
obj-y					+= clk-tegra124-dfll-fcpu.o
endif
obj-$(CONFIG_ARCH_TEGRA_210_SOC)	+= clk-tegra210.o
obj-y					+= cvb.o
