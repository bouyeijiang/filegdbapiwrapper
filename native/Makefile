# Makefile - builds libesrigdbnative.so (the C ABI bridge used by the .NET wrapper)
#
# Usage: make            # Release build into ./build
#        make clean

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
INCLUDEDIR:= $(ROOT)/include
LIBDIR    := $(ROOT)/lib
BUILDDIR  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/build)

CXX       ?= g++
# -Wno-unknown-pragmas: Esri's headers carry MSVC-only "#pragma warning".
CXXFLAGS  += -std=c++11 -O2 -fPIC -fvisibility=hidden -Wall -Wextra \
             -Wno-unknown-pragmas -I$(INCLUDEDIR)
DEFINES   += -DUNICODE -D_UNICODE -DUNIX -D_REENTRANT -DFILEGDB_API \
             -D__USE_FILE_OFFSET64 -DUNIX_FILEGDB_API -D_FILE_OFFSET_BITS=64 \
             -D_LARGEFILE64_SOURCE -DESGDB_NATIVE_EXPORTS

# $ORIGIN makes the bridge find libFileGDBAPI.so / libfgdbunixrtl.so sitting
# next to it (the .NET build copies them into the output folder).
# --disable-new-dtags emits DT_RPATH instead of DT_RUNPATH: only DT_RPATH is
# inherited when resolving transitive dependencies (libFileGDBAPI.so itself has
# no runpath entry for libfgdbunixrtl.so).
LDFLAGS   += -shared -Wl,-soname,libesrigdbnative.so -Wl,-rpath,'$$ORIGIN' \
             -Wl,--disable-new-dtags -L$(LIBDIR)
LIBS      += -lFileGDBAPI -lfgdbunixrtl

# Sources: EsriGdbNative.cpp holds the exported extern "C" ABI, the
# EsriGdbBridge*.cpp files hold the internal helpers (see EsriGdbBridge.h).
SRCS      := EsriGdbNative.cpp \
             EsriGdbBridgeUtil.cpp \
             EsriGdbBridgeXml.cpp \
             EsriGdbBridgeRegistry.cpp
HEADERS   := EsriGdbNative.h EsriGdbBridge.h

TARGET    := $(BUILDDIR)/libesrigdbnative.so
OBJS      := $(addprefix $(BUILDDIR)/,$(SRCS:.cpp=.o))

.PHONY: all clean
all: $(TARGET)

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: %.cpp $(HEADERS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(DEFINES) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

clean:
	-rm -rf $(BUILDDIR)
