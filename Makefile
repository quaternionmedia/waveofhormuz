# Wave of Hormuz — VCV Rack 2 Plugin
# Point RACK_DIR at your Rack SDK before building:
#   make RACK_DIR=/path/to/Rack-SDK

RACK_DIR ?= ../Rack-SDK

SLUG = WaveOfHormuz
VERSION = 2.0.0

FLAGS +=
SOURCES += src/plugin.cpp src/WaveOfHormuz.cpp

DISTRIBUTABLES += res

include $(RACK_DIR)/plugin.mk
