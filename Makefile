CXX ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++2c -shared -fPIC -fno-gnu-unique -Wall
LDFLAGS += -Wl,-z,now
PKGS = pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon

all: hypr-continuous-scrolling.so

hypr-continuous-scrolling.so: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o $@ `pkg-config --cflags $(PKGS)` $(LDFLAGS)

clean:
	rm -f hypr-continuous-scrolling.so

.PHONY: all clean
