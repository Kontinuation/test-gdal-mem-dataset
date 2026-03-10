CXX ?= c++
GDAL_CONFIG ?= gdal-config
UNAME_S := $(shell uname -s)

CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -pthread $(shell $(GDAL_CONFIG) --cflags)
LDFLAGS = $(shell $(GDAL_CONFIG) --libs) -pthread
TARGET = gdal_mem_test
CLANG_FORMAT ?= clang-format
FORMAT_FILES = gdal_mem_test.cpp

ifeq ($(UNAME_S),Linux)
LDFLAGS += -ldl
endif

all: $(TARGET)

$(TARGET): gdal_mem_test.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) gdal_mem_test.cpp $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

.PHONY: all clean format
