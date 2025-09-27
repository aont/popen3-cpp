CXX ?= g++
override CPPFLAGS += -I../include
override CXXFLAGS += -std=c++20 -Wall -Wextra -pedantic
# LDFLAGS ?=
override WINDOWS_LIBS += -lws2_32

LINUX_SOURCES := $(wildcard linux_*.cpp)
WINDOWS_SOURCES := $(wildcard windows_*.cpp)
LINUX_TARGETS := $(LINUX_SOURCES:.cpp=)
WINDOWS_TARGETS := $(WINDOWS_SOURCES:.cpp=)

ifeq ($(OS),Windows_NT)
DEFAULT_TARGETS := $(WINDOWS_TARGETS)
else
DEFAULT_TARGETS := $(LINUX_TARGETS)
endif

all: $(DEFAULT_TARGETS)

linux: $(LINUX_TARGETS)

windows: $(WINDOWS_TARGETS)

$(LINUX_TARGETS): %: %.cpp ../include/popen3.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(WINDOWS_TARGETS): %: %.cpp ../include/popen3.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(WINDOWS_LIBS)

clean:
	rm -f $(LINUX_TARGETS) $(WINDOWS_TARGETS)

.PHONY: all linux windows clean
