# Executable name
EXEC = cppaudiocap

# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -O2 # Example: Enable warnings and optimization

# --- Include Directories ---
# Current directory for vosk_api.h
# PortAudio include directory
INCLUDE_DIRS = -I. \
               -I./lib/portaudio/include

# --- Library Directories ---
# Current directory for libvosk.so or libvosk.a
LIB_DIRS = -L. \
           -L./lib/portaudio/lib/.libs # If libportaudio.so is there, otherwise direct path to .a is fine

# --- Libraries to Link ---

STATIC_LIBS = ./lib/portaudio/lib/.libs/libportaudio.a

# Vosk, and system libraries (rt, asound, jack, pthread, dl for Vosk)
SHARED_LIBS = -lvosk \
              -ldl \
              -lrt \
              -lasound \
              -ljack \
              -pthread

# --- Source Files ---
SRCS = main.cpp

# --- Main Target: Build the executable ---
$(EXEC): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDE_DIRS) -o $@ $^ $(LIB_DIRS) $(STATIC_LIBS) $(SHARED_LIBS) -Wl,-rpath,'$ORIGIN'

# --- Dependency Installation ---
install-deps:
	mkdir -p lib
	# Consider checking if portaudio dir exists to avoid re-downloading/re-building
	if [ ! -d "lib/portaudio" ]; then \
		curl -L https://files.portaudio.com/archives/pa_stable_v190700_20210406.tgz | tar -zx -C lib; \
		cd lib/portaudio && ./configure && $(MAKE) -j; \
	else \
		echo "PortAudio already seems to be set up in lib/portaudio."; \
	fi
.PHONY: install-deps

# --- Uninstall Dependencies ---
uninstall-deps:
	if [ -d "lib/portaudio" ]; then \
		cd lib/portaudio && $(MAKE) uninstall; \
		rm -rf ../portaudio; \
	else \
		echo "PortAudio directory not found for uninstallation."; \
	fi
.PHONY: uninstall-deps

# --- Clean Target ---
clean:
	rm -f $(EXEC)
.PHONY: clean
